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
// faders-uplink-timeline: a stem layout change mid-stream (split on/off, a plan change, the plan's
// allowance) must keep every stem on the same timeline. Viewers place chunk N's audio at N × 500 ms;
// if audio is dropped (or added) when the layout changes, every later chunk is off by that much and
// every viewer has to find the sync again (the e2e saw 1–3 s of "syncing" and +190–380 ms each time).
//
// The real uplink (src/uplink.c, compiled in) with no thread, no clock and no network: the test
// pushes the mixer's ticks itself (1024 frames each), runs the worker's steps after each, and
// records what would go to the hub. Every stem carries a 10 ms burst every 2 s at known sample
// positions; the test decodes each chunk with libopus (a fresh decoder per stem whenever a new stem
// list arrives, as the extension does) and checks that the bursts stay where they were: within one
// Opus frame (20 ms) of their place before the change. It also checks that nothing is skipped (seqs
// consecutive) and that each stem list reaches the hub right before the first chunk in that layout.
// One PASS/FAIL line per case; exits non-zero if any fails. ctest runs it.
//
//   faders-uplink-timeline
#include "../src/uplink.c"

#include <stdarg.h>
#include <stdio.h>

// ── The WebSocket, stubbed: everything "sent" is recorded in order ──────────────────────
struct sent {
	bool binary;
	uint8_t *data;
	size_t len;
};
static struct sent *sent;
static int n_sent, cap_sent;
static void record(bool binary, const void *data, size_t len)
{
	if (n_sent == cap_sent) {
		cap_sent = cap_sent ? cap_sent * 2 : 1024;
		sent = realloc(sent, sizeof(*sent) * (size_t)cap_sent);
	}
	sent[n_sent].binary = binary;
	sent[n_sent].data = malloc(len + 1);
	memcpy(sent[n_sent].data, data, len);
	sent[n_sent].data[len] = 0;
	sent[n_sent].len = len;
	n_sent++;
}
fdr_ws_t *fdr_ws_connect(const char *url, const char *bearer, const fdr_ws_callbacks *cb, void *param) { return NULL; }
bool fdr_ws_send_binary(fdr_ws_t *ws, const uint8_t *data, size_t len)
{
	record(true, data, len);
	return true;
}
bool fdr_ws_send_text(fdr_ws_t *ws, const char *text)
{
	record(false, text, strlen(text));
	return true;
}
void fdr_ws_destroy(fdr_ws_t *ws) {}

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}

// An uplink as fdr_uplink_start makes it, without the worker thread, "connected" with a listener.
static struct fdr_uplink *new_uplink(void)
{
	struct fdr_uplink *u = bzalloc(sizeof(*u));
	u->server = bstrdup("ws://test/ingest/timeline");
	u->key = bstrdup("fdr_test_timeline");
	u->last_sent_seq = -1;
	u->seq0 = 1000;
	pthread_mutex_init(&u->mu, NULL);
	u->bitrate = u->want_bitrate = OPUS_BITRATE;
	u->max_stems = MAX_STEMS;
	u->chunk_gen = UINT32_MAX;
	u->ws = (fdr_ws_t *)1;
	u->connected = true;
	u->listeners = 1;
	return u;
}

// ── The audio: a 10 ms 1 kHz burst every 2 s (at absolute sample 96000·k + 4800), else a quiet
// 200 Hz carrier so no chunk is silent. Every stem plays it; the test tracks where it lands. ──
#define TICK 1024
#define PERIOD 96000
#define BURST_AT 4800
#define BURST_LEN 480
static float sample_at(uint64_t pos, int stem)
{
	double t = (double)pos / SAMPLE_RATE;
	uint64_t in = pos % PERIOD;
	float carrier = (float)(0.01 * sin(2.0 * M_PI * (200.0 + 10.0 * stem) * t));
	if (in >= BURST_AT && in < BURST_AT + BURST_LEN)
		return carrier + (float)(0.5 * sin(2.0 * M_PI * 1000.0 * t));
	return carrier;
}

static uint64_t pos; // samples pushed so far
static void push(struct fdr_uplink *u, int n)
{
	static float buf[MAX_STEMS][TICK * CHANNELS];
	static const float *ptr[MAX_STEMS];
	for (int s = 0; s < n; s++) {
		for (int i = 0; i < TICK; i++) {
			float v = sample_at(pos + (uint64_t)i, s);
			buf[s][2 * i] = v;
			buf[s][2 * i + 1] = v;
		}
		ptr[s] = buf[s];
	}
	fdr_uplink_push_tick(u, 0, ptr, n, TICK, 0.5f);
	pos += TICK;
	// The worker's turn (it runs every 10 ms; a tick is 21 ms).
	encode_ready(u);
	send_stems(u);
	flush(u);
}
static void run(struct fdr_uplink *u, int n, double seconds)
{
	uint64_t end = pos + (uint64_t)(seconds * SAMPLE_RATE);
	while (pos < end)
		push(u, n);
}

static void layout(struct fdr_uplink *u, const char *const *names, const enum fdr_platform *platforms, int n)
{
	struct fdr_stem_desc d[MAX_STEMS];
	for (int i = 0; i < n; i++) {
		bool rest = !strcmp(names[i], "Everything else");
		d[i] = (struct fdr_stem_desc){names[i], rest, platforms && platforms[i] != FDR_PLATFORM_ALL && !rest ? "f1" : NULL, platforms ? platforms[i] : FDR_PLATFORM_ALL};
	}
	fdr_uplink_set_layout(u, d, n);
}

// ── Decoding what the hub got ───────────────────────────────────────────────────────────
struct burst {
	uint32_t seq;  // chunk it starts in
	int64_t onset; // absolute sample (chunk seq's start = (seq - seq0) · 24000)
	char stem[32];
	int layout;    // which stem list was current (0, 1, …)
};
static struct burst bursts[4096];
static int n_bursts;
static char problems[2048];

static void check_hub_view(uint32_t seq0, const char *stem_a, const char *stem_b)
{
	OpusDecoder *dec[MAX_STEMS] = {0};
	char names[MAX_STEMS][32];
	int n_names = 0, layouts = -1;
	int64_t last_seq = -1;
	bool armed[MAX_STEMS];
	int64_t quiet_since[MAX_STEMS];
	static float pcm[FRAME_SAMPLES * CHANNELS];
	n_bursts = 0;
	problems[0] = 0;
	for (int m = 0; m < n_sent; m++) {
		struct sent *x = &sent[m];
		if (!x->binary) {
			if (!strstr((char *)x->data, "\"type\":\"stems\""))
				continue;
			// A new stem list: names by id; fresh decoders, as the extension makes per layout.
			layouts++;
			n_names = 0;
			for (const char *p = (char *)x->data; (p = strstr(p, "\"name\":\"")) && n_names < MAX_STEMS; n_names++) {
				p += 8;
				size_t len = strcspn(p, "\"");
				snprintf(names[n_names], sizeof(names[0]), "%.*s", (int)len, p);
				const char *plat = strstr(p, "\"platform\":\"");
				const char *next = strstr(p, "{\"id\"");
				if (plat && (!next || plat < next))
					snprintf(names[n_names] + strlen(names[n_names]), sizeof(names[0]) - strlen(names[n_names]), "@%.*s", (int)strcspn(plat + 12, "\""), plat + 12);
			}
			for (int s = 0; s < MAX_STEMS; s++) {
				if (dec[s])
					opus_decoder_destroy(dec[s]);
				int err;
				dec[s] = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
				armed[s] = false;
				quiet_since[s] = 0;
			}
			continue;
		}
		const uint8_t *d = x->data;
		int n = d[1];
		uint32_t seq = (uint32_t)d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24;
		if (last_seq >= 0 && seq != last_seq + 1)
			snprintf(problems + strlen(problems), sizeof(problems) - strlen(problems), "[seq %lld → %u] ", (long long)last_seq, seq);
		last_seq = seq;
		if (layouts < 0 || n != n_names)
			snprintf(problems + strlen(problems), sizeof(problems) - strlen(problems), "[chunk %u has %d stems, the list before it %d] ", seq, n, layouts < 0 ? 0 : n_names);
		size_t off = HEADER_BYTES;
		for (int s = 0; s < n && off + 3 <= x->len; s++) {
			int id = d[off];
			int frames = d[off + 1] | d[off + 2] << 8;
			off += 3;
			for (int f = 0; f < FRAMES_PER_CHUNK; f++) {
				int got = 0;
				if (f < frames) {
					int len = d[off] | d[off + 1] << 8;
					got = opus_decode_float(dec[id], d + off + 2, len, pcm, FRAME_SAMPLES, 0);
					off += 2 + (size_t)len;
				}
				if (got <= 0)
					memset(pcm, 0, sizeof(pcm));
				int64_t base = (int64_t)(seq - seq0) * (SAMPLE_RATE * CHUNK_MS / 1000) + f * FRAME_SAMPLES;
				for (int i = 0; i < FRAME_SAMPLES; i++) {
					float v = fabsf(pcm[2 * i]);
					int64_t at = base + i;
					// A burst starts where the level crosses 0.2 after ≥ 0.5 s of the quiet carrier.
					if (v < 0.05f) {
						if (!quiet_since[id])
							quiet_since[id] = at;
						armed[id] = armed[id] || at - quiet_since[id] > SAMPLE_RATE / 2;
					} else {
						quiet_since[id] = 0;
					}
					if (armed[id] && v > 0.2f && n_bursts < 4096) {
						bursts[n_bursts++] = (struct burst){seq, at, "", layouts};
						snprintf(bursts[n_bursts - 1].stem, sizeof(bursts[0].stem), "%s", id < n_names ? names[id] : "?");
						armed[id] = false;
					}
				}
			}
		}
	}
	for (int s = 0; s < MAX_STEMS; s++)
		if (dec[s])
			opus_decoder_destroy(dec[s]);
	(void)stem_a;
	(void)stem_b;
}

// Where a stem's bursts land relative to where they were played (samples; the codec's delay
// included): the last one of stem `was` before the change at sample `at`, the first one of stem
// `is` after it (the list may rename a stem: a split stem says its platform).
static bool offsets(const char *was, const char *is, int64_t at, int64_t *before, int64_t *after)
{
	bool b = false, a = false;
	for (int i = 0; i < n_bursts; i++) {
		bool old = bursts[i].onset < at && !strcmp(bursts[i].stem, was);
		bool now = bursts[i].onset >= at && !strcmp(bursts[i].stem, is);
		if (!old && !now)
			continue;
		int64_t played = (bursts[i].onset - BURST_AT + PERIOD / 2) / PERIOD * PERIOD + BURST_AT;
		int64_t off = bursts[i].onset - played;
		if (old) {
			*before = off;
			b = true;
		} else if (!a) {
			*after = off;
			a = true;
		}
	}
	return a && b;
}

int main(void)
{
	if (!obs_startup("en-US", NULL, NULL))
		return 1;
	struct fdr_uplink *u = new_uplink();
	const char *const plain[] = {"Voice", "Music", "Everything else"};
	const char *const split[] = {"Voice", "Music", "Music", "Everything else", "Everything else"};
	const enum fdr_platform split_p[] = {FDR_PLATFORM_ALL, FDR_PLATFORM_TWITCH, FDR_PLATFORM_YOUTUBE, FDR_PLATFORM_TWITCH, FDR_PLATFORM_YOUTUBE};
	const char *const folded[] = {"Voice", "Everything else"};

	int64_t at[4];
	layout(u, plain, NULL, 3);
	run(u, 3, 6.3); // ends mid-chunk (6.3 s is 12.6 chunks)
	at[1] = (int64_t)pos;
	layout(u, split, split_p, 5); // split on
	run(u, 5, 6.17);
	at[2] = (int64_t)pos;
	layout(u, plain, NULL, 3); // split off (a downgrade)
	run(u, 3, 6.41);
	at[3] = (int64_t)pos;
	layout(u, folded, NULL, 2); // the plan's allowance: Music folds into Everything else
	run(u, 2, 0.05);
	layout(u, plain, NULL, 3); // and back, 50 ms later (a hello and a quality message together)
	run(u, 3, 6.0);

	check_hub_view(1000, "Voice", "Everything else");
	result(!problems[0], "the hub gets every chunk once, each after its stem list", "%s", problems[0] ? problems : "consecutive seqs, every chunk matches the list before it");
	int changes = 0;
	for (int i = 0; i < n_bursts; i++)
		changes = bursts[i].layout > changes ? bursts[i].layout : changes;
	// Per change: the stem's name in the list before and after it (a split stem says its platform).
	const struct {
		int change;
		const char *what, *was, *is;
	} cases[] = {
		{1, "split on (3 → 5 stems)", "Voice", "Voice"},
		{1, "split on (3 → 5 stems)", "Music", "Music@twitch"},
		{1, "split on (3 → 5 stems)", "Everything else", "Everything else@twitch"},
		{2, "split off (5 → 3)", "Voice", "Voice"},
		{2, "split off (5 → 3)", "Everything else@twitch", "Everything else"},
		// The allowance step lasts 50 ms (no burst of its own): measured across both changes.
		// A layout that lasts less than a chunk never reaches the wire (its audio fills the chunk
		// in progress through the layout around it).
		{3, "allowance 3 → 2 and back 50 ms later", "Voice", "Voice"},
		{3, "allowance 3 → 2 and back 50 ms later", "Everything else", "Everything else"},
	};
	for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
		int64_t before = 0, after = 0;
		bool have = offsets(cases[c].was, cases[c].is, at[cases[c].change], &before, &after);
		char name[128];
		snprintf(name, sizeof(name), "%s: %s keeps its timeline", cases[c].what, cases[c].is);
		result(have && llabs(after - before) < FRAME_SAMPLES, name, "burst offset %lld → %lld samples (%+.1f ms; want within one 20 ms frame)%s", (long long)before, (long long)after,
		       (after - before) * 1000.0 / SAMPLE_RATE, have ? "" : ", no burst found on both sides");
	}
	result(changes == 2, "the hub gets each layout that lasted (split on, split off)", "%d stem list(s) after the first (want 2: the 50 ms one never reaches the wire)", changes);
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
