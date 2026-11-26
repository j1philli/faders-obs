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
// faders-oracle-streak: the duplicate warning's three-window rule, tick by tick. The real mixer
// (src/mixer.c, compiled in) with real libobs sources, but no audio thread and no clock: the test
// writes each source's audio buffer, hands the mixer's track callback one synthetic tick at a
// time and rebuilds the source list when it chooses. One PASS/FAIL line per case; exits non-zero
// if any fails. ctest runs it.
//
// Two sources play the same audio: "twin-a" on the Voice fader, "twin-b" in Everything else, and
// the stream has both. The warning is due when the third 24-tick window in a row ends with
// r > 0.5. Rebuilding the source list without changing anything the oracle measures must not
// restart that count; a window without the correlation, or the other copy (or the source itself)
// being replaced, still must.
//
//   faders-oracle-streak
#include "../src/mixer.c"

#include <obs-hotkey.h>
#include <stdarg.h>
#include <stdio.h>

// ── The uplink, stubbed: the mixer only reports to it ───────────────────────────────────
void fdr_uplink_set_stems(fdr_uplink_t *u, const char *const *names, int n, bool rest) {}
void fdr_uplink_push_tick(fdr_uplink_t *u, uint64_t ts, const float *const *stems, int n, size_t frames, float track_peak) {}
void fdr_uplink_set_mismatch(fdr_uplink_t *u, const char *source_name) {}
void fdr_uplink_set_duplicate(fdr_uplink_t *u, const char *source_name) {}
int fdr_uplink_max_stems(fdr_uplink_t *u) { return FDR_MAX_FADERS; }
// The split fader's mixer (src/splitmix.c, compiled in) reports through these too; no split here.
void fdr_uplink_get_status(fdr_uplink_t *u, fdr_uplink_status *out) { memset(out, 0, sizeof(*out)); }
void fdr_uplink_set_layout(fdr_uplink_t *u, const struct fdr_stem_desc *stems, int n) {}
void fdr_uplink_set_split_status(fdr_uplink_t *u, int youtube_track, unsigned warnings) {}

// ── An audio input that outputs nothing itself: the test writes its mix buffer ───────────
static const char *silent_name(void *u) { return "Test input"; }
static void *silent_create(obs_data_t *settings, obs_source_t *source) { return (void *)1; }
static void silent_destroy(void *data) {}
static struct obs_source_info silent_info = {
	.id = "faders_test_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = silent_name,
	.create = silent_create,
	.destroy = silent_destroy,
};

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}

// A mixer as fdr_mixer_start makes it, without the tap, the signals or the rebuild thread: one
// fader (Voice), Track 1 streamed.
static struct fdr_mixer *new_mixer(void)
{
	struct fdr_mixer *m = bzalloc(sizeof(*m));
	pthread_mutex_init(&m->mu, NULL);
	pthread_mutex_init(&m->tr_mu, NULL);
	m->stream_track = 1;
	m->mix_idx = 0;
	m->stream_bit = 1;
	snprintf(m->names[0], sizeof(m->names[0]), "Voice");
	m->n_faders = m->n_wire = m->max_stems = 1;
	fdr_mixer_rebuild(m);
	return m;
}
static void free_mixer(struct fdr_mixer *m)
{
	free_snapshot(m->snap);
	for (int i = 0; i < m->n_dropped; i++)
		obs_weak_source_release(m->dropped[i]);
	pthread_mutex_destroy(&m->mu);
	pthread_mutex_destroy(&m->tr_mu);
	bfree(m);
}

// A source on Track 1. fader: a fader name, or NULL for Everything else. Included every tick
// (the gate's own test hook): which ticks a source is mixed in isn't what this test is about.
static obs_source_t *input(const char *name, const char *fader)
{
	obs_source_t *s = obs_source_create("faders_test_input", name, NULL, NULL);
	obs_source_set_audio_mixers(s, 1);
	obs_data_t *priv = obs_source_get_private_settings(s);
	obs_data_set_string(priv, FDR_FADER_SETTING, fader ? fader : "");
	obs_data_set_bool(priv, FDR_FORCE_SETTING, true);
	obs_data_release(priv);
	return s;
}
static void gone(obs_source_t *s)
{
	if (!s)
		return;
	obs_source_remove(s);
	obs_source_release(s);
	obs_wait_for_destroy_queue();
}

// One tick. Each source's buffer gets its tone (100 Hz is the same audio twice; 1250 Hz is
// unrelated to it over a window), the track gets the sum of every source "in the stream".
static uint64_t ts = 1000000000ULL;
static double phase;
static void tick(struct fdr_mixer *m, obs_source_t *a, obs_source_t *b, double b_freq)
{
	static float track[CH][FRAMES];
	memset(track, 0, sizeof(track));
	obs_source_t *src[2] = {a, b};
	double freq[2] = {100, b_freq};
	for (int j = 0; j < 2; j++) {
		if (!src[j])
			continue;
		struct obs_source_audio_mix mix;
		obs_source_get_audio_mix(src[j], &mix);
		float *l = mix.output[0].data[0], *r = mix.output[0].data[1];
		for (size_t i = 0; i < FRAMES; i++) {
			double t = phase + (double)i / RATE;
			float v = (float)(0.2 * sin(2.0 * M_PI * freq[j] * t));
			l[i] = v;
			r[i] = v * 0.5f;
			track[0][i] += l[i];
			track[1][i] += r[i];
		}
	}
	phase += (double)FRAMES / RATE;
	struct audio_data d = {.data = {(uint8_t *)track[0], (uint8_t *)track[1]}, .frames = FRAMES, .timestamp = ts};
	on_track(m, 0, &d);
	ts += util_mul_div64(FRAMES, 1000000000ULL, RATE);
}

static bool warned(struct fdr_mixer *m, const char *name)
{
	pthread_mutex_lock(&m->mu);
	bool w = strcmp(m->duplicate, name) == 0;
	pthread_mutex_unlock(&m->mu);
	return w;
}

// One oracle window (24 ticks), rebuilding the source list `rebuilds` times inside it (at ticks
// 6, 12 and 18 of the window, the way sources coming and going rebuild it a moment apart).
static void window(struct fdr_mixer *m, obs_source_t *a, obs_source_t *b, double b_freq, int rebuilds)
{
	for (int t = 0; t < ORACLE_TICKS; t++) {
		if (rebuilds && t > 0 && t % (ORACLE_TICKS / (rebuilds + 1)) == 0 && t / (ORACLE_TICKS / (rebuilds + 1)) <= rebuilds)
			fdr_mixer_rebuild(m);
		tick(m, a, b, b_freq);
	}
}

// Part of a window, with the other copy playing the same audio (or absent).
static void part_window(struct fdr_mixer *m, obs_source_t *a, obs_source_t *b, int n)
{
	for (int t = 0; t < n; t++)
		tick(m, a, b, 100);
}

// Windows until the warning names `who` (1-based), or 0 if it hasn't after `max`.
static int windows_until_warned(struct fdr_mixer *m, obs_source_t *a, obs_source_t *b, const char *who, int rebuilds, int max)
{
	for (int w = 1; w <= max; w++) {
		window(m, a, b, 100, rebuilds);
		if (warned(m, who))
			return w;
	}
	return 0;
}

// Window `done + n`, or 0 when n is 0 (no warning).
static int after(int done, int n) { return n ? done + n : 0; }

// libobs and the mixer log through here: only warnings and errors (the mixer's own warning lines).
static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	if (level > LOG_WARNING)
		return;
	vprintf(fmt, args);
	printf("\n");
}

int main(void)
{
	base_set_log_handler(log_handler, NULL);
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-oracle-streak-config", tmp);
	// Video as the other tests set it up (libobs tears sources down through it), but no
	// obs_reset_audio: no audio thread, so the source buffers are the test's alone.
	if (!obs_startup("en-US", config_dir, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC, .gpu_conversion = true,
	};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed\n");
		return 1;
	}
	obs_register_source(&silent_info);

	// ── 1: nothing changes but the source list is rebuilt (twice a window) ───────────────
	{
		obs_source_t *a = input("twin-a", "Voice"), *b = input("twin-b", NULL);
		struct fdr_mixer *m = new_mixer();
		int w = windows_until_warned(m, a, b, "twin-a", 2, 12);
		result(w == ORACLE_RUNS, "1 rebuilds that change nothing keep the duplicate streak",
		       w ? "warned after window %d (want %d: three in a row, ~1.5 s)" : "no warning after 12 windows (want after window %2$d)", w, ORACLE_RUNS);
		free_mixer(m);
		gone(a);
		gone(b);
	}

	// ── 2: a source added meanwhile is no reason to start over ──────────────────────────
	{
		obs_source_t *a = input("twin-a2", "Voice"), *b = input("twin-b2", NULL), *late = NULL;
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 0);
		late = input("late-arrival", NULL); // silent, in Everything else
		window(m, a, b, 100, 1);
		int w = after(2, windows_until_warned(m, a, b, "twin-a2", 0, 10));
		result(w == ORACLE_RUNS, "2 a source added mid-streak doesn't reset it", w ? "warned after window %d (want %d)" : "no warning after 12 windows (want after window %2$d)", w, ORACLE_RUNS);
		free_mixer(m);
		gone(late);
		gone(a);
		gone(b);
	}

	// ── 3: a window without the correlation still resets the streak ─────────────────────
	{
		obs_source_t *a = input("twin-a3", "Voice"), *b = input("twin-b3", NULL);
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 2);
		window(m, a, b, 100, 2);
		window(m, a, b, 1250, 2); // the other source plays something else: r ≈ 0 this window
		bool early = warned(m, "twin-a3");
		int w = after(3, windows_until_warned(m, a, b, "twin-a3", 2, 10));
		result(!early && w == 3 + ORACLE_RUNS, "3 a window with r <= 0.5 resets the streak",
		       w ? "warned after window %d (want %d: windows 4-6), early %d" : "no warning after 13 windows (want after window %2$d: windows 4-6), early %3$d", w, 3 + ORACLE_RUNS, early);
		free_mixer(m);
		gone(a);
		gone(b);
	}

	// ── 4: the other copy replaced: the streak starts over ──────────────────────────────
	{
		obs_source_t *a = input("twin-a4", "Voice"), *b = input("twin-b4", NULL);
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 0);
		window(m, a, b, 100, 0);
		gone(b);
		b = input("twin-b4", NULL); // same name, same audio, a different source
		fdr_mixer_rebuild(m);
		bool early = warned(m, "twin-a4");
		window(m, a, b, 100, 0);
		early = early || warned(m, "twin-a4");
		int w = after(3, windows_until_warned(m, a, b, "twin-a4", 0, 10));
		result(!early && w == 2 + ORACLE_RUNS, "4 the other copy replaced: the streak starts over", w ? "warned after window %d (want %d: windows 3-5), early %d" : "no warning after 13 windows (want after window %2$d: windows 3-5), early %3$d", w, 2 + ORACLE_RUNS, early);
		free_mixer(m);
		gone(a);
		gone(b);
	}

	// ── 5: the other copy removed mid-window: no warning ────────────────────────────────
	// Two windows in a row, then three quarters of a third with the other copy, the rest without.
	{
		obs_source_t *a = input("twin-a5", "Voice"), *b = input("twin-b5", NULL);
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 0);
		window(m, a, b, 100, 0);
		part_window(m, a, b, ORACLE_TICKS * 3 / 4);
		gone(b);
		fdr_mixer_rebuild(m);
		part_window(m, a, NULL, ORACLE_TICKS / 4);
		bool third = warned(m, "twin-a5");
		int w = windows_until_warned(m, a, NULL, "twin-a5", 1, 6);
		result(!third && w == 0, "5 the other copy removed mid-window: no warning", "warned at window 3 %d; %s", third, w ? "warned later too" : "no warning in 6 more windows");
		free_mixer(m);
		gone(a);
	}

	// ── 6: the warned source itself replaced: the streak starts over ────────────────────
	{
		obs_source_t *a = input("twin-a6", "Voice"), *b = input("twin-b6", NULL);
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 0);
		window(m, a, b, 100, 0);
		gone(a);
		a = input("twin-a6", "Voice");
		fdr_mixer_rebuild(m);
		window(m, a, b, 100, 0);
		bool early = warned(m, "twin-a6");
		int w = after(3, windows_until_warned(m, a, b, "twin-a6", 0, 10));
		result(!early && w == 2 + ORACLE_RUNS, "6 the source itself replaced: the streak starts over", w ? "warned after window %d (want %d: windows 3-5), early %d" : "no warning after 13 windows (want after window %2$d: windows 3-5), early %3$d", w, 2 + ORACLE_RUNS, early);
		free_mixer(m);
		gone(a);
		gone(b);
	}

	// ── 7: the other copy moved onto the fader mid-window: no warning ───────────────────
	{
		obs_source_t *a = input("twin-a7", "Voice"), *b = input("twin-b7", NULL);
		struct fdr_mixer *m = new_mixer();
		window(m, a, b, 100, 0);
		window(m, a, b, 100, 0);
		part_window(m, a, b, ORACLE_TICKS * 3 / 4);
		obs_data_t *priv = obs_source_get_private_settings(b);
		obs_data_set_string(priv, FDR_FADER_SETTING, "Voice");
		obs_data_release(priv);
		fdr_mixer_rebuild(m);
		part_window(m, a, b, ORACLE_TICKS / 4);
		bool third = warned(m, "twin-a7");
		int w = windows_until_warned(m, a, b, "twin-a7", 0, 6);
		result(!third && w == 0, "7 the other copy moved onto the fader: no warning", "warned at window 3 %d; %s", third, w ? "warned later too" : "no warning in 6 more windows");
		free_mixer(m);
		gone(a);
		gone(b);
	}

	obs_wait_for_destroy_queue();
	obs_shutdown();
	printf("%s: %d check(s) failed\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
