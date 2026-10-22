/*
 * Faders OBS plugin
 * Copyright (C) 2026 Josh Phillips
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "uplink.h"
#include "ws.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define SAMPLE_RATE 48000
#define FRAME_MS 20
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000) // 960
#define CHUNK_MS 500
#define FRAMES_PER_CHUNK (CHUNK_MS / FRAME_MS) // 25
#define STEMS 3
// Every stem is stereo at the same bitrate, whatever it carries. Twitch serves ~160 kbps stereo
// AAC-LC; 128 kbps stereo Opus is at least as good, and is the default. The hub can raise it
// ({"type":"quality","kbps":N}) for channels whose plan includes more.
#define CHANNELS 2
#define OPUS_BITRATE 128000
#define MIN_BITRATE 64000
#define MAX_BITRATE 256000
#define MAX_PACKET 1500
#define BACKLOG_CHUNKS (60000 / CHUNK_MS)        // 60 s, replayed when a listener arrives
#define RING_SAMPLES (SAMPLE_RATE * 4 * CHANNELS) // 4 s per stem, interleaved; overflow drops the oldest audio
#define HEADER_BYTES 16
#define PROTOCOL_VERSION 1
#define PING_EVERY_NS 20000000000ULL    // heartbeat while connected
#define SILENCE_LIMIT_NS 60000000000ULL // nothing from the hub for this long: reconnect
// Stream-track check: silent means below -80 dBFS; "has sound" means above -60 dBFS.
#define STREAM_SOUND_PEAK 0.0001f
#define STEM_SOUND_PEAK 0.001f
#define STREAM_SILENT_NS 10000000000ULL

struct ring {
	float buf[RING_SAMPLES];
	size_t head; // read position
	size_t len;
};

struct chunk {
	uint32_t seq;
	uint8_t *data;
	size_t len;
};

struct tap {
	struct fdr_uplink *u;
	int stem;
	size_t mix_idx;
};

struct fdr_uplink {
	char *server;
	char *key;
	void (*names_changed)(void);
	struct tap taps[STEMS];

	pthread_mutex_t mu; // guards rings, start_ts, ws state, status
	struct ring rings[STEMS];
	uint64_t start_ts; // first audio timestamp all stems accept, so they start aligned

	OpusEncoder *enc[STEMS];
	int bitrate;      // what the encoders use (worker thread only)
	int want_bitrate; // set from the hub's message (guarded by mu)
	// The chunk being assembled: per stem, FRAMES_PER_CHUNK Opus packets.
	uint8_t packets[STEMS][FRAMES_PER_CHUNK][MAX_PACKET];
	int16_t packet_len[STEMS][FRAMES_PER_CHUNK];
	int frames_in_chunk;

	uint32_t seq0;   // stream clock: wall-clock based so a restarted OBS continues it
	uint32_t chunk_n;
	struct chunk backlog[BACKLOG_CHUNKS];
	int64_t last_sent_seq;
	uint32_t conn_gen; // bumped on every (re)connect; stale flushes don't overwrite progress

	fdr_ws_t *ws;
	bool connected;
	int listeners;
	// Heartbeat: an idle connection (no listeners, nothing sent) can die silently behind a
	// NAT or proxy. The hub answers "ping" with "pong"; no reply for a while means reconnect.
	uint64_t last_rx_ns;
	uint64_t last_ping_ns;
	uint64_t next_connect_ns;
	uint64_t backoff_ns;

	uint64_t window_start_ns;
	size_t window_bytes;
	fdr_uplink_status status;

	// Stream-track check (fdr_uplink_watch_stream_track). Times are os_gettime_ns.
	struct tap stream_tap; // stem = -1
	bool watching_stream;
	uint64_t watch_start_ns;
	uint64_t stream_heard_ns; // last time the streamed track had sound
	uint64_t stems_heard_ns;  // last time any Faders track had sound
	bool stream_status_dirty; // tell the hub (again): changed, or reconnected

	pthread_t thread;
	os_event_t *stop_event;
};

static float peak_of(const struct audio_data *data, int channels)
{
	const float *s = (const float *)data->data[0];
	float peak = 0.0f;
	for (uint32_t i = 0; i < data->frames * channels; i++) {
		float v = s[i] < 0 ? -s[i] : s[i];
		if (v > peak)
			peak = v;
	}
	return peak;
}

// ── Audio thread: copy mixer output into per-stem rings ─────────────────────────────────

static void ring_push(struct ring *r, const float *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (r->len == RING_SAMPLES) { // full: drop the oldest sample
			r->head = (r->head + 1) % RING_SAMPLES;
			r->len--;
		}
		r->buf[(r->head + r->len) % RING_SAMPLES] = src[i];
		r->len++;
	}
}

static void ring_pop(struct ring *r, float *dst, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = r->buf[(r->head + i) % RING_SAMPLES];
	r->head = (r->head + n) % RING_SAMPLES;
	r->len -= n;
}

static void on_audio(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct tap *t = param;
	struct fdr_uplink *u = t->u;
	pthread_mutex_lock(&u->mu);
	// OBS delivers every mix for the same tick with the same timestamp. The first callback
	// picks a start a little in the future; every stem drops audio before it, so all three
	// rings begin on the same sample even though the callbacks were registered one by one.
	if (!u->start_ts)
		u->start_ts = data->timestamp + 50000000ULL;
	if (data->timestamp >= u->start_ts)
		ring_push(&u->rings[t->stem], (const float *)data->data[0], data->frames * CHANNELS);
	if (u->watching_stream && peak_of(data, CHANNELS) > STEM_SOUND_PEAK)
		u->stems_heard_ns = os_gettime_ns();
	pthread_mutex_unlock(&u->mu);
}

// The streamed track: only its level is used.
static void on_stream_audio(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct fdr_uplink *u = ((struct tap *)param)->u;
	if (peak_of(data, 1) <= STREAM_SOUND_PEAK)
		return;
	pthread_mutex_lock(&u->mu);
	u->stream_heard_ns = os_gettime_ns();
	pthread_mutex_unlock(&u->mu);
}

// Worker thread. Decides whether the streamed track is silent and tells the hub when that changes.
static void check_stream_track(struct fdr_uplink *u, uint64_t now)
{
	pthread_mutex_lock(&u->mu);
	bool silent = u->watching_stream && now - u->watch_start_ns >= STREAM_SILENT_NS &&
		      now - u->stream_heard_ns >= STREAM_SILENT_NS && now - u->stems_heard_ns < STREAM_SILENT_NS;
	if (silent != u->status.stream_track_silent) {
		u->status.stream_track_silent = silent;
		u->stream_status_dirty = true;
		if (silent)
			blog(LOG_WARNING, "[faders] Track %d, which OBS streams, is silent while the Faders tracks have sound: viewers hear nothing and the extension can't sync. Tick Track %d for your sources in Edit → Advanced Audio Properties.", u->status.stream_track, u->status.stream_track);
		else
			blog(LOG_INFO, "[faders] Track %d is no longer flagged as silent", u->status.stream_track);
	}
	bool send = u->stream_status_dirty && u->ws && u->connected && u->watching_stream;
	if (send)
		u->stream_status_dirty = false;
	char msg[96];
	snprintf(msg, sizeof(msg), "{\"type\":\"status\",\"streamAudio\":\"%s\",\"track\":%d}", silent ? "silent" : "ok", u->status.stream_track);
	pthread_mutex_unlock(&u->mu);
	if (send && !fdr_ws_send_text(u->ws, msg)) {
		pthread_mutex_lock(&u->mu);
		u->stream_status_dirty = true;
		pthread_mutex_unlock(&u->mu);
	}
}

// ── Network ─────────────────────────────────────────────────────────────────────────────

static void set_error(struct fdr_uplink *u, const char *msg)
{
	snprintf(u->status.last_error, sizeof(u->status.last_error), "%s", msg);
}

static void on_open(void *param)
{
	struct fdr_uplink *u = param;
	pthread_mutex_lock(&u->mu);
	u->connected = true;
	u->backoff_ns = 0;
	u->last_rx_ns = os_gettime_ns();
	u->last_ping_ns = u->last_rx_ns;
	u->last_sent_seq = -1; // the hub may have restarted: offer everything we still hold
	u->conn_gen++;
	u->status.last_error[0] = 0;
	u->stream_status_dirty = true; // a (re)connected hub hasn't heard it
	pthread_mutex_unlock(&u->mu);
	blog(LOG_INFO, "[faders] connected to %s", u->server);
}

static void on_text(void *param, const char *text)
{
	struct fdr_uplink *u = param;
	pthread_mutex_lock(&u->mu);
	u->last_rx_ns = os_gettime_ns();
	pthread_mutex_unlock(&u->mu);
	if (!strcmp(text, "pong"))
		return;
	// Labels are arbitrary Unicode strings: parse the whole message rather than searching
	// JSON text, so escaped quotes or words such as "quality" in a name stay plain labels.
	obs_data_t *message = obs_data_create_from_json(text);
	if (!message)
		return;
	const char *type = obs_data_get_string(message, "type");
	if (!strcmp(type, "fader-names")) {
		obs_data_t *names = obs_data_get_obj(message, "names");
		const char *keys[] = {"voice", "game", "music"};
		char labels[3][FDR_NAME_BYTES] = {{0}};
		bool valid = names != NULL;
		for (int i = 0; valid && i < STEMS; i++) {
			const char *name = obs_data_get_string(names, keys[i]);
			size_t bytes = strlen(name), points = 0;
			valid = bytes > 0 && bytes < FDR_NAME_BYTES;
			for (size_t j = 0; valid && j < bytes; j++) {
				unsigned char c = (unsigned char)name[j];
				if ((c & 0xc0) != 0x80) points++;
				if (c < 0x20 || c == 0x7f || (c == 0xc2 && j + 1 < bytes && (unsigned char)name[j + 1] < 0xa0)) valid = false;
			}
			if (points > 24) valid = false;
			if (valid) memcpy(labels[i], name, bytes + 1);
		}
		if (valid) {
			pthread_mutex_lock(&u->mu);
			bool changed = memcmp(labels, u->status.fader_names, sizeof(labels)) != 0;
			memcpy(u->status.fader_names, labels, sizeof(labels));
			pthread_mutex_unlock(&u->mu);
			if (changed && u->names_changed) u->names_changed();
		}
		obs_data_release(names);
	} else if (!strcmp(type, "update")) {
		const char *latest = obs_data_get_string(message, "latest");
		size_t n = strspn(latest, "0123456789.");
		if (n > 0 && n == strlen(latest) && n < sizeof(u->status.update_available)) {
			pthread_mutex_lock(&u->mu);
			memcpy(u->status.update_available, latest, n + 1);
			pthread_mutex_unlock(&u->mu);
			blog(LOG_INFO, "[faders] update available: %s (this is %s)", latest, FDR_VERSION_STR);
		}
	} else if (!strcmp(type, "quality")) {
		long long kbps = obs_data_get_int(message, "kbps");
		if (kbps >= MIN_BITRATE / 1000 && kbps <= MAX_BITRATE / 1000) {
			pthread_mutex_lock(&u->mu);
			u->want_bitrate = (int)kbps * 1000;
			pthread_mutex_unlock(&u->mu);
		}
	} else if (!strcmp(type, "listeners")) {
		long long n = obs_data_get_int(message, "n");
		if (n >= 0 && n <= INT_MAX) {
			int listeners = (int)n;
			pthread_mutex_lock(&u->mu);
			if ((u->listeners == 0) != (listeners == 0))
				blog(LOG_INFO, "[faders] %d listener(s): %s uplink", listeners, listeners ? "resuming" : "pausing");
			u->listeners = listeners;
			pthread_mutex_unlock(&u->mu);
		}
	}
	obs_data_release(message);
}

static void on_close(void *param, const char *reason)
{
	struct fdr_uplink *u = param;
	pthread_mutex_lock(&u->mu);
	u->connected = false;
	u->listeners = 0;
	set_error(u, reason);
	pthread_mutex_unlock(&u->mu);
	blog(LOG_WARNING, "[faders] disconnected: %s", reason);
}

// Worker thread only. Sends backlog chunks the hub hasn't seen, if anyone is listening.
static void flush(struct fdr_uplink *u)
{
	pthread_mutex_lock(&u->mu);
	bool can_send = u->ws && u->connected && u->listeners > 0;
	int64_t last_sent = u->last_sent_seq; // on_open resets it from the network thread
	uint32_t gen = u->conn_gen;
	pthread_mutex_unlock(&u->mu);
	if (!can_send)
		return;
	// Oldest → newest.
	for (uint32_t i = 0; i < BACKLOG_CHUNKS; i++) {
		struct chunk *c = &u->backlog[(u->chunk_n + i) % BACKLOG_CHUNKS];
		if (!c->data || (int64_t)c->seq <= last_sent)
			continue;
		if (!fdr_ws_send_binary(u->ws, c->data, c->len))
			break; // not open / backed up: retry next tick
		last_sent = c->seq;
		u->window_bytes += c->len;
		u->status.chunks_sent++;
	}
	pthread_mutex_lock(&u->mu);
	if (u->conn_gen == gen)
		u->last_sent_seq = last_sent;
	pthread_mutex_unlock(&u->mu);
}

static void maintain_connection(struct fdr_uplink *u, uint64_t now)
{
	pthread_mutex_lock(&u->mu);
	bool open = u->ws && u->connected;
	bool ping_due = open && now - u->last_ping_ns >= PING_EVERY_NS;
	if (open && now - u->last_rx_ns >= SILENCE_LIMIT_NS) {
		u->connected = false;
		u->listeners = 0;
		set_error(u, "no response from the server");
		blog(LOG_WARNING, "[faders] no response from the server; reconnecting");
		open = ping_due = false;
	}
	if (ping_due)
		u->last_ping_ns = now;
	bool dead = u->ws && !u->connected && u->status.last_error[0];
	pthread_mutex_unlock(&u->mu);
	if (ping_due)
		fdr_ws_send_text(u->ws, "ping");
	if (dead) {
		fdr_ws_destroy(u->ws);
		u->ws = NULL;
		// Back off 1 s → 30 s so a wrong key doesn't hammer the hub.
		u->backoff_ns = u->backoff_ns ? (u->backoff_ns * 2 > 30000000000ULL ? 30000000000ULL : u->backoff_ns * 2) : 1000000000ULL;
		u->next_connect_ns = now + u->backoff_ns;
	}
	if (!u->ws && now >= u->next_connect_ns) {
		static const fdr_ws_callbacks cb = {on_open, on_text, on_close};
		pthread_mutex_lock(&u->mu);
		u->status.last_error[0] = 0;
		pthread_mutex_unlock(&u->mu);
		u->ws = fdr_ws_connect(u->server, u->key, &cb, u);
		if (!u->ws) {
			pthread_mutex_lock(&u->mu);
			set_error(u, "invalid server URL");
			pthread_mutex_unlock(&u->mu);
			u->next_connect_ns = now + 5000000000ULL;
		}
	}
}

// ── Encoding ────────────────────────────────────────────────────────────────────────────

static void write_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void write_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }
static void write_f64(uint8_t *p, double v) { memcpy(p, &v, 8); } // little-endian hosts only (arm64/x86)

static void finish_chunk(struct fdr_uplink *u)
{
	size_t size = HEADER_BYTES;
	for (int s = 0; s < STEMS; s++) {
		size += 3;
		for (int f = 0; f < FRAMES_PER_CHUNK; f++)
			size += 2 + u->packet_len[s][f];
	}
	uint8_t *out = bmalloc(size);
	uint32_t seq = u->seq0 + u->chunk_n;
	out[0] = PROTOCOL_VERSION;
	out[1] = STEMS;
	write_u32(out + 2, seq);
	write_f64(out + 6, (double)seq * CHUNK_MS);
	write_u16(out + 14, FRAME_MS);
	size_t off = HEADER_BYTES;
	for (int s = 0; s < STEMS; s++) {
		out[off] = (uint8_t)s;
		write_u16(out + off + 1, FRAMES_PER_CHUNK);
		off += 3;
		for (int f = 0; f < FRAMES_PER_CHUNK; f++) {
			write_u16(out + off, (uint16_t)u->packet_len[s][f]);
			memcpy(out + off + 2, u->packets[s][f], u->packet_len[s][f]);
			off += 2 + u->packet_len[s][f];
		}
	}
	struct chunk *slot = &u->backlog[u->chunk_n % BACKLOG_CHUNKS];
	bfree(slot->data);
	*slot = (struct chunk){seq, out, size};
	u->chunk_n++;
	u->frames_in_chunk = 0;
}

// Encode every complete 20 ms frame that all three stems have.
static void encode_ready(struct fdr_uplink *u)
{
	float pcm[STEMS][FRAME_SAMPLES * CHANNELS];
	pthread_mutex_lock(&u->mu);
	int want = u->want_bitrate;
	pthread_mutex_unlock(&u->mu);
	if (want != u->bitrate) {
		for (int s = 0; s < STEMS; s++)
			opus_encoder_ctl(u->enc[s], OPUS_SET_BITRATE(want));
		blog(LOG_INFO, "[faders] audio quality: %d kbps per track", want / 1000);
		u->bitrate = want;
	}
	for (;;) {
		pthread_mutex_lock(&u->mu);
		bool ready = true;
		for (int s = 0; s < STEMS; s++)
			ready = ready && u->rings[s].len >= FRAME_SAMPLES * CHANNELS;
		if (ready)
			for (int s = 0; s < STEMS; s++)
				ring_pop(&u->rings[s], pcm[s], FRAME_SAMPLES * CHANNELS);
		pthread_mutex_unlock(&u->mu);
		if (!ready)
			return;
		int f = u->frames_in_chunk;
		for (int s = 0; s < STEMS; s++) {
			int n = opus_encode_float(u->enc[s], pcm[s], FRAME_SAMPLES, u->packets[s][f], MAX_PACKET);
			u->packet_len[s][f] = (int16_t)(n > 0 ? n : 0);
		}
		if (++u->frames_in_chunk == FRAMES_PER_CHUNK)
			finish_chunk(u);
	}
}

static void *worker(void *param)
{
	struct fdr_uplink *u = param;
	os_set_thread_name("faders-uplink");
	while (os_event_timedwait(u->stop_event, 10) == ETIMEDOUT) {
		uint64_t now = os_gettime_ns();
		encode_ready(u);
		maintain_connection(u, now);
		check_stream_track(u, now);
		flush(u);
		if (now - u->window_start_ns >= 5000000000ULL) {
			pthread_mutex_lock(&u->mu);
			u->status.uplink_kbps = (int)(u->window_bytes * 8 * 1000000ULL / (now - u->window_start_ns));
			pthread_mutex_unlock(&u->mu);
			u->window_bytes = 0;
			u->window_start_ns = now;
		}
	}
	return NULL;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────

fdr_uplink_t *fdr_uplink_start(const fdr_uplink_config *cfg)
{
	if (!cfg->server || !*cfg->server || !cfg->key || !*cfg->key)
		return NULL;
	struct fdr_uplink *u = bzalloc(sizeof(*u));
	u->server = bstrdup(cfg->server);
	u->key = bstrdup(cfg->key);
	u->names_changed = cfg->names_changed;
	u->last_sent_seq = -1;
	u->seq0 = (uint32_t)((uint64_t)time(NULL) * 1000 / CHUNK_MS);
	u->window_start_ns = os_gettime_ns();
	pthread_mutex_init(&u->mu, NULL);
	os_event_init(&u->stop_event, OS_EVENT_TYPE_MANUAL);

	u->bitrate = u->want_bitrate = OPUS_BITRATE;
	for (int s = 0; s < STEMS; s++) {
		int err;
		u->enc[s] = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
		opus_encoder_ctl(u->enc[s], OPUS_SET_BITRATE(OPUS_BITRATE));
		opus_encoder_ctl(u->enc[s], OPUS_SET_COMPLEXITY(8));
	}

	// Interleaved stereo float at 48 kHz: OBS's stereo mix as is (mono doubled, surround folded down).
	struct audio_convert_info conv = {.samples_per_sec = SAMPLE_RATE, .format = AUDIO_FORMAT_FLOAT, .speakers = SPEAKERS_STEREO};
	for (int s = 0; s < STEMS; s++) {
		int track = cfg->tracks[s] < 1 || cfg->tracks[s] > 6 ? s + 2 : cfg->tracks[s];
		u->taps[s] = (struct tap){u, s, (size_t)(track - 1)};
		obs_add_raw_audio_callback(u->taps[s].mix_idx, &conv, on_audio, &u->taps[s]);
	}
	u->status.running = true;
	pthread_create(&u->thread, NULL, worker, u);
	blog(LOG_INFO, "[faders] uplink started: tracks voice=%d game=%d music=%d → %s", (int)u->taps[0].mix_idx + 1, (int)u->taps[1].mix_idx + 1, (int)u->taps[2].mix_idx + 1, u->server);
	return u;
}

void fdr_uplink_stop(fdr_uplink_t *u)
{
	if (!u)
		return;
	for (int s = 0; s < STEMS; s++)
		obs_remove_raw_audio_callback(u->taps[s].mix_idx, on_audio, &u->taps[s]);
	fdr_uplink_watch_stream_track(u, 0);
	os_event_signal(u->stop_event);
	pthread_join(u->thread, NULL);
	fdr_ws_destroy(u->ws);
	for (int s = 0; s < STEMS; s++)
		opus_encoder_destroy(u->enc[s]);
	for (int i = 0; i < BACKLOG_CHUNKS; i++)
		bfree(u->backlog[i].data);
	os_event_destroy(u->stop_event);
	pthread_mutex_destroy(&u->mu);
	bfree(u->server);
	bfree(u->key);
	bfree(u);
	blog(LOG_INFO, "[faders] uplink stopped");
}

void fdr_uplink_get_status(fdr_uplink_t *u, fdr_uplink_status *out)
{
	if (!u) {
		memset(out, 0, sizeof(*out));
		return;
	}
	pthread_mutex_lock(&u->mu);
	*out = u->status;
	out->connected = u->connected;
	out->listeners = u->listeners;
	pthread_mutex_unlock(&u->mu);
}

void fdr_uplink_watch_stream_track(fdr_uplink_t *u, int track)
{
	if (!u)
		return;
	if (u->watching_stream) {
		obs_remove_raw_audio_callback(u->stream_tap.mix_idx, on_stream_audio, &u->stream_tap);
		pthread_mutex_lock(&u->mu);
		u->watching_stream = false;
		u->status.stream_track = 0;
		u->status.stream_track_silent = false;
		pthread_mutex_unlock(&u->mu);
	}
	if (track < 1 || track > 6)
		return;
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&u->mu);
	u->stream_tap = (struct tap){u, -1, (size_t)(track - 1)};
	u->watching_stream = true;
	u->watch_start_ns = now;
	u->stream_heard_ns = now;
	u->stems_heard_ns = 0;
	u->status.stream_track = track;
	u->stream_status_dirty = true;
	pthread_mutex_unlock(&u->mu);
	// Mono float; only the level is read.
	struct audio_convert_info conv = {.samples_per_sec = SAMPLE_RATE, .format = AUDIO_FORMAT_FLOAT, .speakers = SPEAKERS_MONO};
	obs_add_raw_audio_callback(u->stream_tap.mix_idx, &conv, on_stream_audio, &u->stream_tap);
	blog(LOG_INFO, "[faders] watching Track %d (streamed audio) for silence", track);
}
