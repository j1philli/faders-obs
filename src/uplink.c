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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define SAMPLE_RATE 48000
#define FRAME_MS 20
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000) // 960
#define CHUNK_MS 500
#define FRAMES_PER_CHUNK (CHUNK_MS / FRAME_MS) // 25
// MAX_STEMS bounds the wire format and arrays; the mixer sets the actual count.
#define MAX_STEMS FDR_MAX_STEMS
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
// A stem whose whole 500 ms chunk peaks below -60 dBFS is sent as "silent" (no Opus frames).
#define SILENT_STEM_PEAK 0.001f

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

struct fdr_uplink {
	char *server;
	char *key;

	pthread_mutex_t mu; // guards rings, ws state, status
	struct ring *rings[MAX_STEMS];
	int n_stems; // stems on the wire, as the mixer set them (0 until it does)
	char names[MAX_STEMS][FDR_STEM_NAME_MAX];
	bool rest_last; // the last stem is "Everything else"

	OpusEncoder *enc[MAX_STEMS];
	int bitrate;      // what the encoders use (worker thread only)
	int want_bitrate; // set from the hub's message (guarded by mu)
	int max_stems;    // from the hub's quality message; the mixer folds faders past it
	// The chunk being assembled: per stem, FRAMES_PER_CHUNK Opus packets.
	uint8_t packets[MAX_STEMS][FRAMES_PER_CHUNK][MAX_PACKET];
	int16_t packet_len[MAX_STEMS][FRAMES_PER_CHUNK];
	float chunk_peak[MAX_STEMS];
	int frames_in_chunk;
	uint32_t layout_gen; // bumped (locked) whenever n_stems changes; a chunk never mixes layouts
	uint32_t chunk_gen;  // worker: the layout the chunk in progress was started with
	int chunk_stems;     // worker: stems in that layout

	uint32_t seq0; // stream clock: wall-clock based so a restarted OBS continues it
	uint32_t chunk_n;
	struct chunk backlog[BACKLOG_CHUNKS];
	int64_t last_sent_seq;
	uint32_t conn_gen; // bumped on every (re)connect; stale flushes don't overwrite progress
	bool stems_dirty;  // the hub must hear the stem list (connect, or the list changed)

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

	// Stream-track check (fdr_uplink_watch_stream_track): the mixer taps the track and reports
	// its peak with every pushed tick. Times are os_gettime_ns.
	bool watching_stream;
	uint64_t watch_start_ns;
	uint64_t stream_heard_ns; // last time the streamed track had sound
	uint64_t stems_heard_ns;  // last time any Faders track had sound
	bool stream_status_dirty; // tell the hub (again): changed, or reconnected

	pthread_t thread;
	os_event_t *stop_event;
};

// "At least `ns` have passed since `since`". The audio and network threads stamp times after the
// worker took `now`, so a plain unsigned subtraction could wrap and read as "ages ago".
static inline bool elapsed(uint64_t now, uint64_t since, uint64_t ns)
{
	return now >= since && now - since >= ns;
}

static float peak_of(const float *s, size_t n)
{
	float peak = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float v = s[i] < 0 ? -s[i] : s[i];
		if (v > peak)
			peak = v;
	}
	return peak;
}

// ── Audio thread: the mixer's stems go into per-stem rings ──────────────────────────────

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

// Locked. Drops buffered audio and the chunk in progress so every stem restarts aligned.
static void reset_rings(struct fdr_uplink *u)
{
	for (int s = 0; s < MAX_STEMS; s++)
		if (u->rings[s])
			u->rings[s]->head = u->rings[s]->len = 0;
	u->layout_gen++;
}

// The mixer hands over every stem for one tick at once, already aligned.
void fdr_uplink_push_tick(fdr_uplink_t *u, uint64_t ts, const float *const *stems, int n, size_t frames, float track_peak)
{
	UNUSED_PARAMETER(ts);
	if (!u)
		return;
	pthread_mutex_lock(&u->mu);
	if (n == u->n_stems && n > 0) {
		for (int s = 0; s < n; s++)
			if (u->rings[s])
				ring_push(u->rings[s], stems[s], frames * CHANNELS);
		if (u->watching_stream) {
			if (track_peak > STREAM_SOUND_PEAK)
				u->stream_heard_ns = os_gettime_ns();
			for (int s = 0; s < n - (u->rest_last ? 1 : 0); s++)
				if (peak_of(stems[s], frames * CHANNELS) > STEM_SOUND_PEAK) {
					u->stems_heard_ns = os_gettime_ns();
					break;
				}
		}
	}
	pthread_mutex_unlock(&u->mu);
}

// Worker thread. Tells the hub about the streamed track's silence and the mixer's mismatch
// verdict whenever either changes (or the connection is new).
static void send_status(struct fdr_uplink *u, uint64_t now)
{
	pthread_mutex_lock(&u->mu);
	bool silent = u->watching_stream && elapsed(now, u->watch_start_ns, STREAM_SILENT_NS) &&
		      elapsed(now, u->stream_heard_ns, STREAM_SILENT_NS) && !elapsed(now, u->stems_heard_ns, STREAM_SILENT_NS);
	if (silent != u->status.stream_track_silent) {
		u->status.stream_track_silent = silent;
		u->stream_status_dirty = true;
		if (silent)
			blog(LOG_WARNING, "[faders] Track %d, which OBS streams, is silent while the Faders layers have sound: viewers hear nothing and the extension can't sync. Tick Track %d for your sources in Edit → Advanced Audio Properties.", u->status.stream_track, u->status.stream_track);
		else
			blog(LOG_INFO, "[faders] Track %d is no longer flagged as silent", u->status.stream_track);
	}
	bool send = u->stream_status_dirty && u->ws && u->connected && u->watching_stream;
	if (send)
		u->stream_status_dirty = false;
	char msg[160];
	if (u->status.mix_mismatch[0])
		snprintf(msg, sizeof(msg), "{\"type\":\"status\",\"streamAudio\":\"%s\",\"track\":%d,\"mixMismatch\":\"%s\"}", silent ? "silent" : "ok", u->status.stream_track, u->status.mix_mismatch);
	else
		snprintf(msg, sizeof(msg), "{\"type\":\"status\",\"streamAudio\":\"%s\",\"track\":%d}", silent ? "silent" : "ok", u->status.stream_track);
	pthread_mutex_unlock(&u->mu);
	if (send && !fdr_ws_send_text(u->ws, msg)) {
		pthread_mutex_lock(&u->mu);
		u->stream_status_dirty = true;
		pthread_mutex_unlock(&u->mu);
	}
}

// Worker thread. The stem list goes out on every (re)connect and whenever it changes, before
// any chunk with that layout (the hub forwards text in order).
static void send_stems(struct fdr_uplink *u)
{
	pthread_mutex_lock(&u->mu);
	bool send = u->stems_dirty && u->ws && u->connected;
	int n = u->n_stems;
	pthread_mutex_unlock(&u->mu);
	if (!send)
		return;
	struct dstr msg = {0};
	dstr_copy(&msg, "{\"type\":\"stems\",\"stems\":[");
	for (int s = 0; s < n; s++) {
		if (u->rest_last && s == n - 1) {
			dstr_catf(&msg, "%s{\"id\":%d,\"name\":\"Everything else\",\"rest\":true}", s ? "," : "", s);
			continue;
		}
		const char *name = u->names[s];
		dstr_catf(&msg, "%s{\"id\":%d,\"name\":\"", s ? "," : "", s);
		for (const char *c = name; *c; c++) { // JSON string: escape quotes and backslashes, drop controls
			if (*c == '"' || *c == '\\')
				dstr_catf(&msg, "\\%c", *c);
			else if ((unsigned char)*c >= 0x20)
				dstr_ncat(&msg, c, 1);
		}
		dstr_cat(&msg, "\"}");
	}
	dstr_cat(&msg, "]}");
	bool ok = fdr_ws_send_text(u->ws, msg.array);
	dstr_free(&msg);
	if (ok) {
		pthread_mutex_lock(&u->mu);
		u->stems_dirty = false;
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
	u->stems_dirty = true;
	pthread_mutex_unlock(&u->mu);
	blog(LOG_INFO, "[faders] connected to %s", u->server);
}

static void on_text(void *param, const char *text)
{
	struct fdr_uplink *u = param;
	pthread_mutex_lock(&u->mu);
	u->last_rx_ns = os_gettime_ns();
	pthread_mutex_unlock(&u->mu);
	// The hub sends {"type":"listeners","n":N}, {"type":"update","latest":"X.Y.Z"},
	// {"type":"quality","kbps":N,"maxStems":M} and "pong". Tiny, fixed shapes: no JSON parser.
	const char *latest = strstr(text, "\"latest\":\"");
	if (strstr(text, "\"update\"") && latest) {
		latest += 10;
		size_t n = strspn(latest, "0123456789.");
		pthread_mutex_lock(&u->mu);
		if (n > 0 && n < sizeof(u->status.update_available)) {
			memcpy(u->status.update_available, latest, n);
			u->status.update_available[n] = 0;
		}
	}
	const char *kbps = strstr(text, "\"kbps\":");
	if (strstr(text, "\"quality\"") && kbps) {
		int bps = atoi(kbps + 7) * 1000;
		const char *max_stems = strstr(text, "\"maxStems\":");
		pthread_mutex_lock(&u->mu);
		if (bps >= MIN_BITRATE && bps <= MAX_BITRATE)
			u->want_bitrate = bps;
		if (max_stems) {
			int m = atoi(max_stems + 11);
			u->max_stems = m < 1 ? 1 : m > MAX_STEMS ? MAX_STEMS : m;
			u->status.max_stems = u->max_stems;
		}
		pthread_mutex_unlock(&u->mu);
		return;
	}
	if (!strstr(text, "\"listeners\""))
		return;
	const char *n = strstr(text, "\"n\":");
	if (!n)
		return;
	int listeners = atoi(n + 4);
	pthread_mutex_lock(&u->mu);
	if ((u->listeners == 0) != (listeners == 0))
		blog(LOG_INFO, "[faders] %d listener(s): %s uplink", listeners, listeners ? "resuming" : "pausing");
	u->listeners = listeners;
	pthread_mutex_unlock(&u->mu);
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
	bool can_send = u->ws && u->connected && u->listeners > 0 && !u->stems_dirty;
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
	bool ping_due = open && elapsed(now, u->last_ping_ns, PING_EVERY_NS);
	if (open && elapsed(now, u->last_rx_ns, SILENCE_LIMIT_NS)) {
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
	int n_stems = u->chunk_stems;
	size_t size = HEADER_BYTES;
	for (int s = 0; s < n_stems; s++) {
		size += 3;
		if (u->chunk_peak[s] < SILENT_STEM_PEAK)
			continue; // silent stem: no frames
		for (int f = 0; f < FRAMES_PER_CHUNK; f++)
			size += 2 + u->packet_len[s][f];
	}
	uint8_t *out = bmalloc(size);
	uint32_t seq = u->seq0 + u->chunk_n;
	out[0] = PROTOCOL_VERSION;
	out[1] = (uint8_t)n_stems;
	write_u32(out + 2, seq);
	write_f64(out + 6, (double)seq * CHUNK_MS);
	write_u16(out + 14, FRAME_MS);
	size_t off = HEADER_BYTES;
	for (int s = 0; s < n_stems; s++) {
		out[off] = (uint8_t)s;
		bool silent = u->chunk_peak[s] < SILENT_STEM_PEAK;
		write_u16(out + off + 1, silent ? 0 : FRAMES_PER_CHUNK);
		off += 3;
		if (silent)
			continue;
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
	memset(u->chunk_peak, 0, sizeof(u->chunk_peak));
}

// Encode every complete 20 ms frame that all stems have.
static void encode_ready(struct fdr_uplink *u)
{
	static float pcm[MAX_STEMS][FRAME_SAMPLES * CHANNELS];
	pthread_mutex_lock(&u->mu);
	int want = u->want_bitrate;
	pthread_mutex_unlock(&u->mu);
	if (want != u->bitrate) {
		for (int s = 0; s < MAX_STEMS; s++)
			if (u->enc[s])
				opus_encoder_ctl(u->enc[s], OPUS_SET_BITRATE(want));
		blog(LOG_INFO, "[faders] audio quality: %d kbps per layer", want / 1000);
		u->bitrate = want;
	}
	for (;;) {
		pthread_mutex_lock(&u->mu);
		int n_stems = u->n_stems;
		uint32_t gen = u->layout_gen;
		bool ready = n_stems > 0;
		for (int s = 0; s < n_stems; s++)
			ready = ready && u->rings[s] && u->rings[s]->len >= FRAME_SAMPLES * CHANNELS;
		if (ready)
			for (int s = 0; s < n_stems; s++)
				ring_pop(u->rings[s], pcm[s], FRAME_SAMPLES * CHANNELS);
		pthread_mutex_unlock(&u->mu);
		if (!ready)
			return;
		if (u->chunk_gen != gen) {
			// The stem list changed under a chunk in progress: drop it, a chunk has one layout.
			u->chunk_gen = gen;
			u->chunk_stems = n_stems;
			u->frames_in_chunk = 0;
			memset(u->chunk_peak, 0, sizeof(u->chunk_peak));
		}
		int f = u->frames_in_chunk;
		for (int s = 0; s < n_stems; s++) {
			float peak = peak_of(pcm[s], FRAME_SAMPLES * CHANNELS);
			if (peak > u->chunk_peak[s])
				u->chunk_peak[s] = peak;
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
		send_stems(u);
		send_status(u, now);
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

static OpusEncoder *make_encoder(int bitrate)
{
	int err;
	OpusEncoder *e = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
	opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(8));
	return e;
}

fdr_uplink_t *fdr_uplink_start(const fdr_uplink_config *cfg)
{
	if (!cfg->server || !*cfg->server || !cfg->key || !*cfg->key)
		return NULL;
	struct fdr_uplink *u = bzalloc(sizeof(*u));
	u->server = bstrdup(cfg->server);
	u->key = bstrdup(cfg->key);
	u->names_changed = cfg->names_changed;
	u->update_available = cfg->update_available;
	u->last_sent_seq = -1;
	u->seq0 = (uint32_t)((uint64_t)time(NULL) * 1000 / CHUNK_MS);
	u->window_start_ns = os_gettime_ns();
	pthread_mutex_init(&u->mu, NULL);
	os_event_init(&u->stop_event, OS_EVENT_TYPE_MANUAL);

	u->bitrate = u->want_bitrate = OPUS_BITRATE;
	u->max_stems = MAX_STEMS;
	u->chunk_gen = UINT32_MAX; // the first frame adopts the current layout
	u->n_stems = 0;            // stems arrive with fdr_uplink_set_stems; nothing is encoded before that
	u->status.running = true;
	pthread_create(&u->thread, NULL, worker, u);
	blog(LOG_INFO, "[faders] uplink started → %s", u->server);
	return u;
}

void fdr_uplink_set_stems(fdr_uplink_t *u, const char *const *names, int n, bool rest)
{
	if (!u)
		return;
	if (n < 1)
		n = 1;
	if (n > MAX_STEMS)
		n = MAX_STEMS;
	// Rings and encoders for new indices are made before the count grows, so the worker never
	// sees an index it has no encoder for. They stay for the life of the uplink.
	struct ring *new_rings[MAX_STEMS] = {0};
	OpusEncoder *new_enc[MAX_STEMS] = {0};
	for (int s = 0; s < n; s++) {
		if (!u->rings[s])
			new_rings[s] = bzalloc(sizeof(struct ring));
		if (!u->enc[s])
			new_enc[s] = make_encoder(u->bitrate);
	}
	pthread_mutex_lock(&u->mu);
	for (int s = 0; s < n; s++) {
		if (new_rings[s])
			u->rings[s] = new_rings[s];
		if (new_enc[s])
			u->enc[s] = new_enc[s];
		snprintf(u->names[s], sizeof(u->names[s]), "%s", names[s] ? names[s] : "");
	}
	bool changed = n != u->n_stems || rest != u->rest_last;
	for (int s = 0; s < n && !changed; s++)
		changed = strcmp(u->names[s], names[s] ? names[s] : "") != 0;
	u->n_stems = n;
	u->rest_last = rest;
	u->status.stems = n;
	u->stems_dirty = true;
	reset_rings(u); // a chunk has one layout: restart aligned
	pthread_mutex_unlock(&u->mu);
	if (changed)
		blog(LOG_INFO, "[faders] stems: %d layer(s)%s", n, rest ? " incl. Everything else" : "");
}

void fdr_uplink_set_mismatch(fdr_uplink_t *u, const char *source_name)
{
	if (!u)
		return;
	pthread_mutex_lock(&u->mu);
	const char *now = source_name ? source_name : "";
	if (strcmp(u->status.mix_mismatch, now) != 0) {
		snprintf(u->status.mix_mismatch, sizeof(u->status.mix_mismatch), "%s", now);
		u->stream_status_dirty = true;
	}
	pthread_mutex_unlock(&u->mu);
}

int fdr_uplink_max_stems(fdr_uplink_t *u)
{
	if (!u)
		return MAX_STEMS;
	pthread_mutex_lock(&u->mu);
	int m = u->max_stems;
	pthread_mutex_unlock(&u->mu);
	return m;
}

void fdr_uplink_stop(fdr_uplink_t *u)
{
	if (!u)
		return;
	fdr_uplink_watch_stream_track(u, 0);
	os_event_signal(u->stop_event);
	pthread_join(u->thread, NULL);
	fdr_ws_destroy(u->ws);
	for (int s = 0; s < MAX_STEMS; s++) {
		if (u->enc[s])
			opus_encoder_destroy(u->enc[s]);
		bfree(u->rings[s]);
	}
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
	out->stems = u->n_stems;
	pthread_mutex_unlock(&u->mu);
}

void fdr_uplink_watch_stream_track(fdr_uplink_t *u, int track)
{
	if (!u)
		return;
	if (u->watching_stream) {
		pthread_mutex_lock(&u->mu);
		u->watching_stream = false;
		u->status.stream_track = 0;
		u->status.stream_track_silent = false;
		u->status.mix_mismatch[0] = 0;
		pthread_mutex_unlock(&u->mu);
	}
	if (track < 1 || track > 6)
		return;
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&u->mu);
	u->watching_stream = true;
	u->watch_start_ns = now;
	u->stream_heard_ns = now;
	u->stems_heard_ns = 0;
	u->status.stream_track = track;
	u->stream_status_dirty = true;
	pthread_mutex_unlock(&u->mu);
	blog(LOG_INFO, "[faders] streamed track is Track %d: silence check on, Everything else measured against it", track);
}
