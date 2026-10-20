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
// faders-mixcheck: proves the libobs facts the virtual mixer (PROTOCOL.md, "Virtual mixer")
// relies on, against the real libobs, headless. No plugin involved. One PASS/FAIL line per check;
// exits non-zero if any check fails. CI runs it (scripts/ci/teamcity-macos.sh).
//
// The mixer under test: a raw audio callback on track 1 (allow_clipping = true) rebuilds the
// track from per-source buffers (obs_source_get_audio_mix) gated on obs_source_active, the
// source's track bits and obs_source_audio_pending, and checks the residual track − Σ sources.
//
//   faders-mixcheck
#include <obs.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/platform.h>
#include <util/threading.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define RATE 48000
#define FRAMES AUDIO_OUTPUT_FRAMES // 1024 per tick
#define MAX_SRC 32
#define MAX_CB 4096

// ── Tone source: stereo float, 10 ms blocks, direct (wall-clock) timestamps ──────────────
struct tone {
	obs_source_t *source;
	double freq, amp, phase;
	pthread_t thread;
	volatile bool stop;
};

static void *tone_thread(void *param)
{
	struct tone *t = param;
	const size_t block = RATE / 100;
	float l[RATE / 100], r[RATE / 100];
	uint64_t t0 = os_gettime_ns();
	for (uint64_t n = 0; !t->stop; n++) {
		for (size_t i = 0; i < block; i++) {
			float v = (float)(t->amp * sin(t->phase));
			t->phase += 2.0 * M_PI * t->freq / RATE;
			l[i] = v;
			r[i] = v * 0.5f; // channels differ, so a channel swap would show
		}
		if (t->phase > 2.0 * M_PI)
			t->phase -= 2.0 * M_PI;
		struct obs_source_audio a = {
			.data = {(uint8_t *)l, (uint8_t *)r},
			.frames = (uint32_t)block,
			.speakers = SPEAKERS_STEREO,
			.format = AUDIO_FORMAT_FLOAT_PLANAR,
			.samples_per_sec = RATE,
			.timestamp = t0 + n * 10000000ULL,
		};
		obs_source_output_audio(t->source, &a);
		os_sleepto_ns(t0 + (n + 1) * 10000000ULL);
	}
	return NULL;
}

static const char *tone_name(void *u) { return "Tone"; }
static void *tone_create(obs_data_t *settings, obs_source_t *source)
{
	struct tone *t = bzalloc(sizeof(*t));
	t->source = source;
	t->freq = obs_data_get_double(settings, "freq");
	t->amp = obs_data_get_double(settings, "amp");
	pthread_create(&t->thread, NULL, tone_thread, t);
	return t;
}
static void tone_destroy(void *data)
{
	struct tone *t = data;
	t->stop = true;
	pthread_join(t->thread, NULL);
	bfree(t);
}
static struct obs_source_info tone_info = {
	.id = "faders_test_tone",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = tone_name,
	.create = tone_create,
	.destroy = tone_destroy,
};

// ── The mixer under test (audio thread) ─────────────────────────────────────────────────
struct src_entry {
	obs_source_t *src;
	bool in_a, in_b; // membership in the transition's scenes A / B (plugin: enum_active_tree)
};
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct src_entry srcs[MAX_SRC];
static int n_srcs;

// Transition model: t = (ts − start) / dur per sample, the formula libobs uses (calc_time).
static obs_source_t *tr;
static uint64_t tr_start_ns, tr_dur_ns;
static bool tr_model;

struct stats {
	uint64_t ticks, offset_ticks, pending_ticks, fade_ticks;
	float peak_unclamped, peak_clamped;
	double resid_max, resid_sq, resid_vt_sq, resid_none_sq, sum_sq, ungated_sq;
	uint64_t n;
	float src_peak[MAX_SRC];
	uint64_t first_zero_ts[MAX_SRC], first_sound_ts[MAX_SRC];
	bool was_zero[MAX_SRC];
	uint64_t cb_ns[MAX_CB];
	int n_cb;
	// per fade tick: residual energy of the per-sample model (first/last tick straddle the start/stop)
	double fade_tick_sq[64];
	int n_fade_ticks;
	// capture callback (fallback API): first muted / unmuted flags seen
	uint64_t cap_muted_os, cap_unmuted_os;
};
static struct stats st;
static int phase;

static inline float gain_of(const struct src_entry *e, uint64_t sample_ts, float t_tick, int model)
{
	if (!tr_model || (!e->in_a && !e->in_b))
		return 1.0f;
	float t;
	if (model == 0) { // per-sample, libobs formula
		if (sample_ts <= tr_start_ns)
			t = 0.0f;
		else if (sample_ts - tr_start_ns >= tr_dur_ns)
			t = 1.0f;
		else
			t = (float)((long double)(sample_ts - tr_start_ns) / (long double)tr_dur_ns);
	} else if (model == 1) {
		t = t_tick; // obs_transition_get_time(), once per tick (video clock)
	} else {
		return 1.0f; // no model
	}
	if (e->in_a && e->in_b)
		return 1.0f;
	return e->in_a ? 1.0f - t : t;
}

static void on_clamped(void *param, size_t mix_idx, struct audio_data *data)
{
	const float *l = (const float *)data->data[0];
	float peak = 0;
	for (uint32_t i = 0; i < data->frames; i++)
		peak = fabsf(l[i]) > peak ? fabsf(l[i]) : peak;
	pthread_mutex_lock(&mu);
	if (peak > st.peak_clamped)
		st.peak_clamped = peak;
	pthread_mutex_unlock(&mu);
}

// Per source: the audio_ts seen at the previous tick. After mixing, libobs has already advanced a
// consumed source's audio_ts to the tick's end (discard_audio) and zeroed a scene's; so "was mixed
// from frame 0 of this tick" is: audio_ts == tick end now, and audio_ts == tick start at the last
// tick (it was already flowing). A source's first tick is left to the residual.
static uint64_t prev_ats[MAX_SRC];
static inline bool near(uint64_t a, uint64_t b) { return (a > b ? a - b : b - a) <= 4; }

static void on_track(void *param, size_t mix_idx, struct audio_data *data)
{
	const uint64_t ts = data->timestamp;
	const uint64_t ts_end = ts + util_mul_div64(FRAMES, 1000000000ULL, RATE);
	static float acc[3][2][FRAMES]; // model 0 (per-sample), 1 (get_time per tick), 2 (none)
	static float ungated[2][FRAMES];
	static struct obs_source_audio_mix mixes[MAX_SRC];
	static bool included[MAX_SRC];
	memset(acc, 0, sizeof(acc));
	memset(ungated, 0, sizeof(ungated));
	bool any_new = false, any_pending = false;
	float t_tick = tr && tr_model ? obs_transition_get_time(tr) : 0.0f;
	bool in_fade = tr_model && ts > tr_start_ns && ts - tr_start_ns < tr_dur_ns;

	pthread_mutex_lock(&mu);
	// ── Timed: what the plugin does per tick (gating + one gain model + summing) ──────
	uint64_t t_in = os_gettime_ns();
	for (int k = 0; k < n_srcs; k++) {
		struct src_entry *e = &srcs[k];
		obs_source_t *s = e->src;
		included[k] = false;
		obs_source_get_audio_mix(s, &mixes[k]);
		uint64_t ats = obs_source_get_audio_timestamp(s);
		uint64_t was = prev_ats[k];
		prev_ats[k] = ats;
		if (!obs_source_active(s))
			continue;
		if (!(obs_source_get_audio_mixers(s) & 1))
			continue;
		if (obs_source_get_monitoring_type(s) == OBS_MONITORING_TYPE_MONITOR_ONLY)
			continue;
		if (obs_source_audio_pending(s)) {
			any_pending = true;
			continue;
		}
		// libobs computes tick times as start + frames_to_ns(total frames): they are up to a
		// few ns off from start-of-tick + frames_to_ns(1024).
		if (!near(ats, ts_end))
			continue; // not consumed through this tick
		if (!near(was, ts)) {
			any_new = true; // first tick of this source (or a resync): offset unknown
			continue;
		}
		included[k] = true;
		bool per_sample = tr_model && (e->in_a || e->in_b);
		float g = per_sample ? 0.0f : 1.0f;
		for (int ch = 0; ch < 2; ch++) {
			const float *in = mixes[k].output[0].data[ch];
			float *out = acc[0][ch];
			for (size_t i = 0; i < FRAMES; i++) {
				if (per_sample)
					g = gain_of(e, ts + util_mul_div64(i, 1000000000ULL, RATE), t_tick, 0);
				out[i] += in[i] * g;
			}
		}
	}
	uint64_t t_out = os_gettime_ns();
	static int dbg;
	if (getenv("MIXCHECK_DEBUG") && dbg++ < 6) {
		fprintf(stderr, "tick ts=%llu end=%llu:", (unsigned long long)ts, (unsigned long long)ts_end);
		for (int k = 0; k < n_srcs; k++)
			fprintf(stderr, " [%s ats-end=%lld active=%d pend=%d inc=%d]", obs_source_get_name(srcs[k].src), (long long)prev_ats[k] - (long long)ts_end, obs_source_active(srcs[k].src), obs_source_audio_pending(srcs[k].src), included[k]);
		fprintf(stderr, "\n");
	}
	// ── Untimed: alternative models and bookkeeping for the checks ──────────────────
	for (int k = 0; k < n_srcs; k++) {
		float speak = 0;
		for (int ch = 0; ch < 2; ch++)
			for (int i = 0; i < FRAMES; i++) {
				float v = fabsf(mixes[k].output[0].data[ch][i]);
				speak = v > speak ? v : speak;
				ungated[ch][i] += mixes[k].output[0].data[ch][i];
			}
		if (speak > st.src_peak[k])
			st.src_peak[k] = speak;
		bool zero = speak < 1e-6f;
		if (zero && !st.first_zero_ts[k] && !st.was_zero[k] && st.ticks > 0)
			st.first_zero_ts[k] = ts;
		if (!zero && !st.first_sound_ts[k] && st.was_zero[k])
			st.first_sound_ts[k] = ts;
		st.was_zero[k] = zero;
		if (!included[k])
			continue;
		for (int m = 1; m < 3; m++) {
			float g = gain_of(&srcs[k], ts, t_tick, m);
			for (int ch = 0; ch < 2; ch++)
				for (int i = 0; i < FRAMES; i++)
					acc[m][ch][i] += mixes[k].output[0].data[ch][i] * g;
		}
	}
	float peak = 0;
	double tick_sq = 0;
	for (int ch = 0; ch < 2; ch++) {
		const float *track = (const float *)data->data[ch];
		for (int i = 0; i < FRAMES; i++) {
			float v = fabsf(track[i]);
			peak = v > peak ? v : peak;
			double r = track[i] - acc[0][ch][i];
			double ar = fabs(r);
			if (ar > st.resid_max)
				st.resid_max = ar;
			st.resid_sq += r * r;
			tick_sq += r * r;
			st.sum_sq += (double)acc[0][ch][i] * acc[0][ch][i];
			double ru = track[i] - ungated[ch][i];
			st.ungated_sq += ru * ru;
			if (in_fade) {
				double r1 = track[i] - acc[1][ch][i], r2 = track[i] - acc[2][ch][i];
				st.resid_vt_sq += r1 * r1;
				st.resid_none_sq += r2 * r2;
			}
			st.n++;
		}
	}
	if (peak > st.peak_unclamped)
		st.peak_unclamped = peak;
	st.ticks++;
	if (in_fade) {
		st.fade_ticks++;
		if (st.n_fade_ticks < 64)
			st.fade_tick_sq[st.n_fade_ticks++] = tick_sq;
	}
	if (any_new)
		st.offset_ticks++;
	if (any_pending)
		st.pending_ticks++;
	if (st.n_cb < MAX_CB)
		st.cb_ns[st.n_cb++] = t_out - t_in;
	pthread_mutex_unlock(&mu);
}

// Fallback API under evaluation: the per-source capture callback's muted flag.
static void on_capture(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&mu);
	if (muted && !st.cap_muted_os)
		st.cap_muted_os = now;
	if (!muted && !st.cap_unmuted_os)
		st.cap_unmuted_os = now;
	pthread_mutex_unlock(&mu);
}

// ── Test harness ────────────────────────────────────────────────────────────────────────
static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}
static void skip(const char *name, const char *why)
{
	printf("SKIP  %-44s %s\n", name, why);
}

static void begin(int p, bool keep_zero_state)
{
	pthread_mutex_lock(&mu);
	bool was_zero[MAX_SRC];
	memcpy(was_zero, st.was_zero, sizeof(was_zero));
	memset(&st, 0, sizeof(st));
	if (keep_zero_state)
		memcpy(st.was_zero, was_zero, sizeof(was_zero));
	phase = p;
	pthread_mutex_unlock(&mu);
}
static struct stats snapshot(void)
{
	pthread_mutex_lock(&mu);
	struct stats s = st;
	pthread_mutex_unlock(&mu);
	return s;
}
static double dbfs(double sq, uint64_t n)
{
	return n ? 20.0 * log10(sqrt(sq / (double)n) + 1e-12) : -999;
}

static int add_src(obs_source_t *s, bool in_a, bool in_b)
{
	pthread_mutex_lock(&mu);
	int k = n_srcs++;
	srcs[k] = (struct src_entry){s, in_a, in_b};
	pthread_mutex_unlock(&mu);
	return k;
}
static obs_source_t *tone(const char *name, double freq, double amp, uint32_t mixers)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_double(s, "freq", freq);
	obs_data_set_double(s, "amp", amp);
	obs_source_t *src = obs_source_create("faders_test_tone", name, s, NULL);
	obs_data_release(s);
	obs_source_set_audio_mixers(src, mixers);
	return src;
}
static obs_scene_t *scene_with(const char *name, obs_source_t *child)
{
	obs_scene_t *sc = obs_scene_create(name);
	obs_scene_add(sc, child);
	return sc;
}

struct find_ptt { obs_source_t *src; obs_hotkey_id id; };
static bool find_ptt_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct find_ptt *f = data;
	if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE && obs_weak_source_references_source(obs_hotkey_get_registerer(key), f->src) &&
	    strcmp(obs_hotkey_get_name(key), "libobs.push-to-talk") == 0) {
		f->id = id;
		return false;
	}
	return true;
}

static double pct(uint64_t *v, int n, double p)
{
	if (!n)
		return 0;
	// insertion sort is fine for a few thousand values once
	for (int i = 1; i < n; i++) {
		uint64_t x = v[i];
		int j = i - 1;
		while (j >= 0 && v[j] > x) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = x;
	}
	int idx = (int)(p * (n - 1));
	return (double)v[idx] / 1000.0;
}

int main(int argc, char **argv)
{
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-mixcheck-config", tmp);
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
	struct obs_audio_info ai = {.samples_per_sec = RATE, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai)) {
		fprintf(stderr, "obs_reset_audio failed\n");
		return 1;
	}
	obs_register_source(&tone_info);
	printf("libobs %s (API %u.%u)\n", obs_get_version_string(), LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER);

	obs_scene_t *scene_a = NULL, *scene_b = NULL;
	bool have_transitions = false;
	if (FADERS_OBS_TRANSITIONS_BIN[0]) {
		obs_module_t *mod = NULL;
		have_transitions = obs_open_module(&mod, FADERS_OBS_TRANSITIONS_BIN, FADERS_OBS_TRANSITIONS_DATA) == MODULE_SUCCESS && obs_init_module(mod);
	}

	// Track 1, unclamped and clamped, float planar 48 kHz stereo: the mixer's own buffers, no resampling.
	struct audio_convert_info conv = {.samples_per_sec = RATE, .format = AUDIO_FORMAT_FLOAT_PLANAR, .speakers = SPEAKERS_STEREO, .allow_clipping = true};
	struct audio_convert_info conv_clamped = conv;
	conv_clamped.allow_clipping = false;
	obs_add_raw_audio_callback(0, &conv, on_track, NULL);
	obs_add_raw_audio_callback(0, &conv_clamped, on_clamped, NULL);

	// ── 1 + 2: two loud sources on track 1 ───────────────────────────────────────────
	obs_source_t *s1 = tone("s1", 440, 0.8, 1), *s2 = tone("s2", 660, 0.8, 1);
	int k1 = add_src(s1, false, false), k2 = add_src(s2, false, false);
	obs_set_output_source(1, s1);
	obs_set_output_source(2, s2);
	os_sleep_ms(1500);
	begin(1, false);
	os_sleep_ms(1000);
	struct stats s = snapshot();
	result(s.peak_unclamped > 1.05f && s.peak_clamped <= 1.0001f, "1 allow_clipping gives the unclamped track",
	       "unclamped peak %.3f, clamped peak %.3f over %llu ticks", s.peak_unclamped, s.peak_clamped, (unsigned long long)s.ticks);
	result(s.ticks > 30 && s.resid_max < 1e-5, "2 track == sum of obs_source_get_audio_mix",
	       "max |residual| %.2e, rms %.1f dBFS, ticks %llu (first-tick exclusions %llu, pending %llu)", s.resid_max, dbfs(s.resid_sq, s.n),
	       (unsigned long long)s.ticks, (unsigned long long)s.offset_ticks, (unsigned long long)s.pending_ticks);

	// 2b: a fader at 50 % and a source on track 1 only vs track 2 only: buffers carry volume and track bits.
	obs_source_set_volume(s2, 0.5f);
	obs_source_t *s2b = tone("s2b-track2", 770, 0.6, 2);
	int k2b = add_src(s2b, false, false);
	obs_set_output_source(3, s2b);
	os_sleep_ms(800);
	begin(2, false);
	os_sleep_ms(1000);
	s = snapshot();
	result(s.ticks > 30 && s.resid_max < 1e-5 && s.src_peak[k2] < 0.45f && s.src_peak[k2] > 0.35f && s.src_peak[k2b] < 1e-6f,
	       "2b volume applied in buffers; off-track zeroed", "s2 peak %.3f (vol 0.5 of 0.8), s2b (track 2 only) track-1 buffer peak %.3f, residual %.2e",
	       s.src_peak[k2], s.src_peak[k2b], s.resid_max);
	obs_source_set_volume(s2, 1.0f);

	// ── 3: an inactive source (no output channel) still ticks ────────────────────────
	obs_source_t *s3 = tone("s3-inactive", 990, 0.7, 1);
	int k3 = add_src(s3, false, false);
	os_sleep_ms(800);
	begin(3, false);
	os_sleep_ms(1000);
	s = snapshot();
	result(!obs_source_active(s3) && s.src_peak[k3] > 0.5f && s.resid_max < 1e-5 && dbfs(s.ungated_sq, s.n) > -20,
	       "3 inactive source ticks but is gated", "active=%d buffer peak %.3f residual %.2e (ungated residual %.1f dBFS)",
	       obs_source_active(s3), s.src_peak[k3], s.resid_max, dbfs(s.ungated_sq, s.n));

	// ── 4: preview-only scene (studio mode) vs a scene in program ────────────────────
	obs_source_t *s4 = tone("s4-preview", 1100, 0.7, 1);
	int k4 = add_src(s4, false, false);
	obs_scene_t *preview = scene_with("preview", s4);
	obs_source_inc_showing(obs_scene_get_source(preview)); // what the frontend does for the preview
	obs_source_t *s5 = tone("s5-in-scene", 1210, 0.7, 1);
	int k5 = add_src(s5, false, false);
	obs_scene_t *program = scene_with("program", s5);
	obs_set_output_source(4, obs_scene_get_source(program));
	os_sleep_ms(800);
	begin(4, false);
	os_sleep_ms(1000);
	s = snapshot();
	result(!obs_source_active(s4) && obs_source_showing(s4) && s.src_peak[k4] > 0.5f && obs_source_active(s5) && s.src_peak[k5] > 0.5f &&
		       s.resid_max < 1e-5,
	       "4 preview scene: showing, not active; program scene nested", "s4 active=%d showing=%d peak %.3f; s5 active=%d peak %.3f; residual %.2e",
	       obs_source_active(s4), obs_source_showing(s4), s.src_peak[k4], obs_source_active(s5), s.src_peak[k5], s.resid_max);
	obs_source_dec_showing(obs_scene_get_source(preview));

	// ── 5: fade transition 300 ms between scene A and scene B ────────────────────────
	if (!have_transitions) {
		skip("5 fade transition residual", "obs-transitions module not available");
	} else {
		obs_source_t *s6 = tone("s6-sceneA", 880, 0.6, 1), *s7 = tone("s7-sceneB", 1320, 0.6, 1);
		int k6 = add_src(s6, true, false), k7 = add_src(s7, false, true);
		obs_scene_t *a = scene_a = scene_with("A", s6), *b = scene_b = scene_with("B", s7);
		tr = obs_source_create("fade_transition", "fade", NULL, NULL);
		obs_set_output_source(0, tr);
		obs_transition_set(tr, obs_scene_get_source(a));
		os_sleep_ms(1000);
		begin(5, false);
		pthread_mutex_lock(&mu);
		tr_dur_ns = 300000000ULL;
		tr_start_ns = os_gettime_ns();
		tr_model = true;
		pthread_mutex_unlock(&mu);
		obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 300, obs_scene_get_source(b));
		os_sleep_ms(700);
		s = snapshot();
		double vt = dbfs(s.resid_vt_sq, s.fade_ticks * 2 * FRAMES), none = dbfs(s.resid_none_sq, s.fade_ticks * 2 * FRAMES);
		// The tick in which obs_transition_start() runs is mixed with a half-applied state (B
		// activated vs. start time set): up to one tick of error, which the residual stem
		// carries. Judge the model on the ticks fully inside the fade.
		double all_sq = 0, inner_sq = 0, worst = 0;
		for (int i = 0; i < s.n_fade_ticks; i++) {
			all_sq += s.fade_tick_sq[i];
			if (i > 0 && i < s.n_fade_ticks - 1)
				inner_sq += s.fade_tick_sq[i];
			if (s.fade_tick_sq[i] > worst)
				worst = s.fade_tick_sq[i];
		}
		double model = dbfs(all_sq, s.fade_ticks * 2 * FRAMES), inner = dbfs(inner_sq, (s.n_fade_ticks > 2 ? s.n_fade_ticks - 2 : 1) * 2 * FRAMES);
		result(s.fade_ticks >= 10 && inner < -40.0, "5 fade: residual with per-sample model",
		       "fade ticks %llu; residual rms: per-sample model %.1f dBFS (inner ticks %.1f, worst tick %.1f), get_time() per tick %.1f dBFS, no model %.1f dBFS; s7 peak %.3f",
		       (unsigned long long)s.fade_ticks, model, inner, dbfs(worst, 2 * FRAMES), vt, none, s.src_peak[k7]);
		// After the fade: A is no longer active, B is; still exact.
		os_sleep_ms(500);
		begin(6, false);
		os_sleep_ms(800);
		s = snapshot();
		result(!obs_source_active(s6) && obs_source_active(s7) && s.resid_max < 1e-5, "5b after the fade: A inactive, exact again",
		       "s6 active=%d s7 active=%d residual %.2e (s6 buffer peak %.3f)", obs_source_active(s6), obs_source_active(s7), s.resid_max, s.src_peak[k6]);
		pthread_mutex_lock(&mu);
		tr_model = false;
		pthread_mutex_unlock(&mu);
		skip("6 stinger", "needs a media file + obs-ffmpeg; by code reading (transition-stinger.c) the stinger's audio is a private media source mixed in stinger_audio_render → lands in the residual");
	}

	// ── 7: mute and push-to-talk line up with the buffers ────────────────────────────
	obs_source_add_audio_capture_callback(s1, on_capture, NULL);
	begin(7, true);
	os_sleep_ms(200);
	uint64_t m_os = os_gettime_ns();
	obs_source_set_muted(s1, true);
	os_sleep_ms(500);
	s = snapshot();
	double zero_ms = s.first_zero_ts[k1] ? ((double)s.first_zero_ts[k1] - (double)m_os) / 1e6 : -1;
	double cap_ms = s.cap_muted_os ? ((double)s.cap_muted_os - (double)m_os) / 1e6 : -1;
	result(s.first_zero_ts[k1] && zero_ms > -25 && zero_ms < 80 && s.resid_max < 1e-5, "7 mute zeroes the buffer at the mute time",
	       "buffer all-zero tick ts %.1f ms after set_muted; capture callback muted flag %.1f ms after; residual %.2e", zero_ms, cap_ms, s.resid_max);
	begin(8, true);
	uint64_t u_os = os_gettime_ns();
	obs_source_set_muted(s1, false);
	os_sleep_ms(500);
	s = snapshot();
	double sound_ms = s.first_sound_ts[k1] ? ((double)s.first_sound_ts[k1] - (double)u_os) / 1e6 : -1;
	result(s.first_sound_ts[k1] && sound_ms > -25 && sound_ms < 80, "7b unmute", "sound back %.1f ms after set_muted(false); capture unmuted flag %.1f ms", sound_ms,
	       s.cap_unmuted_os ? ((double)s.cap_unmuted_os - (double)u_os) / 1e6 : -1);
	obs_source_remove_audio_capture_callback(s1, on_capture, NULL);

	struct find_ptt f = {s2, OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(find_ptt_cb, &f);
	if (f.id == OBS_INVALID_HOTKEY_ID) {
		skip("7c push-to-talk release delay", "no push-to-talk hotkey registered for the source");
	} else {
		obs_hotkey_enable_callback_rerouting(true);
		obs_source_set_push_to_talk_delay(s2, 200);
		obs_source_enable_push_to_talk(s2, true); // not pressed → muted
		os_sleep_ms(400);
		obs_hotkey_trigger_routed_callback(f.id, true);
		os_sleep_ms(400);
		begin(9, true);
		os_sleep_ms(100);
		uint64_t r_os = os_gettime_ns();
		obs_hotkey_trigger_routed_callback(f.id, false);
		os_sleep_ms(600);
		s = snapshot();
		double ptt_ms = s.first_zero_ts[k2] ? ((double)s.first_zero_ts[k2] - (double)r_os) / 1e6 : -1;
		result(s.first_zero_ts[k2] && ptt_ms > 170 && ptt_ms < 280 && s.resid_max < 1e-5, "7c push-to-talk release delay (200 ms)",
		       "buffer went silent %.1f ms after release; residual %.2e", ptt_ms, s.resid_max);
		obs_source_enable_push_to_talk(s2, false);
		obs_hotkey_enable_callback_rerouting(false);
		os_sleep_ms(300);
	}

	// ── 8: sync offset +200 ms ───────────────────────────────────────────────────────
	obs_source_set_sync_offset(s1, 200000000LL);
	os_sleep_ms(1200);
	begin(10, false);
	os_sleep_ms(1000);
	s = snapshot();
	result(s.resid_max < 1e-5 && s.src_peak[k1] > 0.5f, "8 sync offset +200 ms stays aligned", "residual %.2e, s1 peak %.3f, first-tick exclusions %llu", s.resid_max,
	       s.src_peak[k1], (unsigned long long)s.offset_ticks);
	obs_source_set_sync_offset(s1, 0);
	os_sleep_ms(800);

	// ── 9: CPU with 12 sources ───────────────────────────────────────────────────────
	obs_scene_t *many = obs_scene_create("many");
	obs_source_t *extra[12];
	for (int i = 0; i < 12; i++) {
		char name[32];
		snprintf(name, sizeof(name), "many-%d", i);
		extra[i] = tone(name, 200 + 97 * i, 0.05, 1);
		obs_scene_add(many, extra[i]);
		add_src(extra[i], false, false);
	}
	obs_set_output_source(5, obs_scene_get_source(many));
	os_sleep_ms(1000);
	begin(11, false);
	os_sleep_ms(2000);
	s = snapshot();
	double p99 = pct(s.cb_ns, s.n_cb, 0.99), p50 = pct(s.cb_ns, s.n_cb, 0.5), mx = pct(s.cb_ns, s.n_cb, 1.0);
	result(s.n_cb > 50 && p99 < 200.0 && s.resid_max < 1e-5, "9 CPU: gating + per-sample gains + summing",
	       "p50 %.0f µs, p99 %.0f µs, max %.0f µs over %d ticks, %d sources; residual %.2e", p50, p99, mx, s.n_cb, n_srcs, s.resid_max);

	// ── teardown ─────────────────────────────────────────────────────────────────────
	obs_remove_raw_audio_callback(0, on_track, NULL);
	obs_remove_raw_audio_callback(0, on_clamped, NULL);
	for (int c = 0; c < 6; c++)
		obs_set_output_source(c, NULL);
	pthread_mutex_lock(&mu);
	int n = n_srcs;
	n_srcs = 0;
	pthread_mutex_unlock(&mu);
	if (tr) {
		obs_transition_clear(tr);
		obs_source_release(tr);
	}
	// The main canvas holds a reference to every scene (OBS 32 canvases): remove, then release.
	obs_scene_t *scenes[] = {scene_a, scene_b, preview, program, many};
	for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); i++) {
		if (!scenes[i])
			continue;
		obs_source_remove(obs_scene_get_source(scenes[i]));
		obs_scene_release(scenes[i]);
	}
	for (int k = 0; k < n; k++)
		obs_source_release(srcs[k].src);
	obs_wait_for_destroy_queue(); // sources are destroyed on a task thread; the tone threads must stop first
	obs_shutdown();
	printf("%s: %d check(s) failed\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
