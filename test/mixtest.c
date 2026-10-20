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
// faders-mixtest: the virtual mixer (sources mode) end to end, headless: real libobs, the real
// plugin, synthetic tone sources with distinct levels, and the plugin's own "faders_mixer_stats"
// procedure for what each fader and Everything else carried. One PASS/FAIL line per case; exits
// non-zero if any fails. CI runs it (scripts/ci/teamcity-macos.sh).
//
//   faders-mixtest <server> <key> [fold|wire]
//
// "fold": only the plan-cap case; the hub must be sending {"type":"quality","maxStems":2}
// (mock-hub --kbps 128 --max-stems 2). "wire": no checks; keeps the five sources playing for
// 25 s so test/plugin-listener.mjs can check the stems as a listener sees them.
#include <obs.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/platform.h>
#include <util/threading.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define RATE 48000
// A tone's stereo RMS for amplitude a: left = a·sin, right = a/2·sin → sqrt((1 + 1/4) / 2) / sqrt(2).
#define TONE_RMS(a) ((a) * 0.5590170f)

// ── Tone source (stereo float, 10 ms blocks) ────────────────────────────────────────────
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
			r[i] = v * 0.5f;
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

// ── Harness ─────────────────────────────────────────────────────────────────────────────
static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
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

static void call(const char *proc, calldata_t *cd)
{
	if (!proc_handler_call(obs_get_proc_handler(), proc, cd))
		fprintf(stderr, "proc %s failed\n", proc);
}
static void assign(const char *source, const char *fader)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "source", source);
	calldata_set_string(&cd, "fader", fader);
	call("faders_assign", &cd);
	calldata_free(&cd);
}

// Stats: fetch the JSON once (resets the plugin's window) and pick values out of it.
static char stats[8192];
static void fetch_stats(void)
{
	calldata_t cd = {0};
	call("faders_mixer_stats", &cd);
	snprintf(stats, sizeof(stats), "%s", calldata_string(&cd, "json") ? calldata_string(&cd, "json") : "{}");
	calldata_free(&cd);
}
static void reset_stats(void)
{
	fetch_stats();
}
static double num_after(const char *hay, const char *key)
{
	const char *p = strstr(hay, key);
	return p ? atof(p + strlen(key)) : NAN;
}
static double rest_rms(void) { return num_after(stats, "\"rest_rms\":"); }
static double rest_dbfs(void) { return num_after(stats, "\"rest_dbfs\":"); }
static double fader_rms(const char *name)
{
	char key[96];
	snprintf(key, sizeof(key), "\"name\":\"%s\",\"rms\":", name);
	return num_after(stats, key);
}
static const char *source_json(const char *name)
{
	char key[96];
	snprintf(key, sizeof(key), "\"name\":\"%s\",\"fader\":", name);
	return strstr(stats, key);
}
static int source_included(const char *name)
{
	const char *p = source_json(name);
	return p ? (int)num_after(p, "\"included\":") : -1;
}
static bool source_dropped(const char *name)
{
	const char *p = source_json(name);
	return p && strstr(p, "\"dropped\":true") && (strstr(p, "\"dropped\":true") < strchr(p + 1, '}'));
}
static const char *mismatch(void)
{
	static char buf[64];
	const char *p = strstr(stats, "\"mismatch\":\"");
	buf[0] = 0;
	if (p) {
		p += 12;
		size_t n = strcspn(p, "\"");
		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;
		memcpy(buf, p, n);
		buf[n] = 0;
	}
	return buf;
}
static int faders_on_wire(void)
{
	int n = 0;
	for (const char *p = strstr(stats, "\"faders\":["); p && (p = strstr(p + 1, "\"rms\":")) && p < strstr(stats, "\"sources\":"); n++)
		;
	return n;
}
static bool close_to(double v, double want, double tol) { return fabs(v - want) <= tol * want + 1e-4; }
static double ticks(void) { return num_after(stats, "\"ticks\":"); }

// Measure a window: settle, reset, wait, fetch.
static void window(int settle_ms, int window_ms)
{
	os_sleep_ms((uint32_t)settle_ms);
	reset_stats();
	os_sleep_ms((uint32_t)window_ms);
	fetch_stats();
}

struct find_ptt {
	obs_source_t *src;
	obs_hotkey_id id;
};
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

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <server> <key> [fold]\n", argv[0]);
		return 1;
	}
	bool fold = argc > 3 && strcmp(argv[3], "fold") == 0;
	bool wire = argc > 3 && strcmp(argv[3], "wire") == 0;
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-mixtest-config", tmp);
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
	bool have_transitions = false;
	if (FADERS_OBS_TRANSITIONS_BIN[0]) {
		obs_module_t *tmod = NULL;
		have_transitions = obs_open_module(&tmod, FADERS_OBS_TRANSITIONS_BIN, FADERS_OBS_TRANSITIONS_DATA) == MODULE_SUCCESS && obs_init_module(tmod);
	}

	// Sources: distinct amplitudes identify them in RMS sums. All on track 1, all active.
	const double A_VOICE = 0.5, A_GAME = 0.4, A_MUSIC = 0.3, A_DISCORD = 0.2, A_EXTRA = 0.25;
	obs_source_t *voice = tone("voice", 440, A_VOICE, 1), *game = tone("game", 660, A_GAME, 1), *music = tone("music", 880, A_MUSIC, 1),
		     *discord = tone("discord", 1100, A_DISCORD, 1), *extra = tone("extra", 1320, A_EXTRA, 1);
	obs_set_output_source(1, voice);
	obs_set_output_source(2, game);
	obs_set_output_source(3, music);
	obs_set_output_source(4, discord);
	obs_set_output_source(5, extra);

	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", FADERS_PLUGIN_BIN);
		return 1;
	}
	// Sources mode with four faders; the streamed track is track 1 (no streaming output here).
	{
		calldata_t cd = {0};
		calldata_set_string(&cd, "server", argv[1]);
		calldata_set_string(&cd, "key", argv[2]);
		calldata_set_string(&cd, "faders", "Voice,Game,Music,Discord");
		calldata_set_bool(&cd, "only_live", false);
		call("faders_configure_sources", &cd);
		calldata_free(&cd);
		calldata_t wc = {0};
		calldata_set_int(&wc, "track", 1);
		call("faders_watch_stream_track", &wc);
		calldata_free(&wc);
		calldata_t td = {0};
		calldata_set_int(&td, "ms", 300);
		call("faders_transition_duration", &td);
		calldata_free(&td);
	}
	assign("voice", "Voice");
	assign("game", "Game");
	assign("music", "Music");
	assign("discord", "Discord");
	// "extra" stays automatic: a test tone has no rule, so it is Everything else.

	const double R_VOICE = TONE_RMS(A_VOICE), R_GAME = TONE_RMS(A_GAME), R_MUSIC = TONE_RMS(A_MUSIC), R_DISCORD = TONE_RMS(A_DISCORD), R_EXTRA = TONE_RMS(A_EXTRA);

	if (wire) {
		for (int t = 0; t < 25; t++) {
			os_sleep_ms(1000);
			if (t % 5 == 4) {
				calldata_t st = {0};
				call("faders_status", &st);
				printf("[%2ds] %s\n", t + 1, calldata_string(&st, "status"));
				fflush(stdout);
				calldata_free(&st);
			}
		}
		goto teardown;
	}
	if (fold) {
		// ── Plan cap: the hub allows 2 faders; Music and Discord fold into Everything else ──
		window(3000, 1000);
		double rest_want = sqrt(R_MUSIC * R_MUSIC + R_DISCORD * R_DISCORD + R_EXTRA * R_EXTRA);
		result(faders_on_wire() == 2 && close_to(fader_rms("Voice"), R_VOICE, 0.05) && close_to(fader_rms("Game"), R_GAME, 0.05) && close_to(rest_rms(), rest_want, 0.05),
		       "fader folded by plan cap (maxStems 2)", "faders on wire %d; Voice %.4f (want %.4f), Game %.4f, rest %.4f (want %.4f: music+discord+extra); mismatch \"%s\"",
		       faders_on_wire(), fader_rms("Voice"), R_VOICE, fader_rms("Game"), rest_rms(), rest_want, mismatch());
		goto teardown;
	}

	// ── 1: 5 sources, 4 faders, 1 unassigned → each fader exact, rest == the unassigned one ──
	window(2500, 1000);
	result(ticks() > 30 && close_to(fader_rms("Voice"), R_VOICE, 0.03) && close_to(fader_rms("Game"), R_GAME, 0.03) && close_to(fader_rms("Music"), R_MUSIC, 0.03) &&
		       close_to(fader_rms("Discord"), R_DISCORD, 0.03) && close_to(rest_rms(), R_EXTRA, 0.03) && mismatch()[0] == 0,
	       "1 five sources, four faders, one unassigned", "Voice %.4f/%.4f Game %.4f/%.4f Music %.4f/%.4f Discord %.4f/%.4f rest %.4f/%.4f (got/want), %g ticks, mismatch \"%s\"",
	       fader_rms("Voice"), R_VOICE, fader_rms("Game"), R_GAME, fader_rms("Music"), R_MUSIC, fader_rms("Discord"), R_DISCORD, rest_rms(), R_EXTRA, ticks(), mismatch());

	// ── 2: mute, and push-to-talk with a 200 ms release delay ───────────────────────────
	obs_source_set_muted(voice, true);
	window(150, 400);
	double muted_voice = fader_rms("Voice"), muted_rest = rest_rms();
	obs_source_set_muted(voice, false);
	window(300, 300);
	result(muted_voice < 1e-4 && close_to(muted_rest, R_EXTRA, 0.03) && close_to(fader_rms("Voice"), R_VOICE, 0.03), "2 mute zeroes the fader, nothing leaks to rest",
	       "muted: Voice %.5f rest %.4f; unmuted: Voice %.4f", muted_voice, muted_rest, fader_rms("Voice"));
	struct find_ptt f = {game, OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(find_ptt_cb, &f);
	if (f.id == OBS_INVALID_HOTKEY_ID) {
		printf("SKIP  %-46s no push-to-talk hotkey for the source\n", "2b push-to-talk release delay");
	} else {
		obs_hotkey_enable_callback_rerouting(true);
		obs_source_set_push_to_talk_delay(game, 200);
		obs_source_enable_push_to_talk(game, true);
		os_sleep_ms(300);
		obs_hotkey_trigger_routed_callback(f.id, true);
		os_sleep_ms(400);
		obs_hotkey_trigger_routed_callback(f.id, false);
		// 0–150 ms after release: still talking (delay); 300–700 ms: silent.
		window(0, 150);
		double early = fader_rms("Game");
		window(150, 400);
		double late = fader_rms("Game"), late_rest = rest_rms();
		result(early > R_GAME * 0.8 && late < 1e-4 && close_to(late_rest, R_EXTRA, 0.03), "2b push-to-talk release delay (200 ms)", "Game 0-150 ms after release %.4f (want ~%.4f), 300-700 ms %.5f; rest %.4f", early, R_GAME,
		       late, late_rest);
		obs_source_enable_push_to_talk(game, false);
		obs_hotkey_enable_callback_rerouting(false);
		os_sleep_ms(300);
	}

	// ── 3: a source hidden in the live scene ────────────────────────────────────────────
	obs_source_t *hid = tone("hidden", 1540, 0.15, 1);
	assign("hidden", "Game");
	obs_scene_t *live = obs_scene_create("live");
	obs_sceneitem_t *hid_item = obs_scene_add(live, hid);
	obs_set_output_source(6, obs_scene_get_source(live));
	window(1500, 600);
	double shown = fader_rms("Game");
	obs_sceneitem_set_visible(hid_item, false);
	window(600, 600);
	result(close_to(shown, sqrt(R_GAME * R_GAME + TONE_RMS(0.15) * TONE_RMS(0.15)), 0.03) && close_to(fader_rms("Game"), R_GAME, 0.03) && close_to(rest_rms(), R_EXTRA, 0.03),
	       "3 source hidden in the live scene is excluded", "Game shown %.4f (want %.4f), hidden %.4f (want %.4f), rest %.4f", shown, sqrt(R_GAME * R_GAME + TONE_RMS(0.15) * TONE_RMS(0.15)), fader_rms("Game"), R_GAME,
	       rest_rms());

	// ── 4: a source only in a preview scene (studio mode) ───────────────────────────────
	obs_source_t *prev = tone("preview-only", 1760, 0.15, 1);
	assign("preview-only", "Music");
	obs_scene_t *preview = obs_scene_create("preview");
	obs_scene_add(preview, prev);
	obs_source_inc_showing(obs_scene_get_source(preview)); // what the frontend does for the preview
	window(1000, 600);
	result(!obs_source_active(prev) && obs_source_showing(prev) && close_to(fader_rms("Music"), R_MUSIC, 0.03) && close_to(rest_rms(), R_EXTRA, 0.03),
	       "4 preview-only source is excluded", "active=%d showing=%d Music %.4f (want %.4f) rest %.4f", obs_source_active(prev), obs_source_showing(prev), fader_rms("Music"), R_MUSIC, rest_rms());
	obs_source_dec_showing(obs_scene_get_source(preview));

	// ── 5: Monitor Only ─────────────────────────────────────────────────────────────────
	obs_source_set_monitoring_type(discord, OBS_MONITORING_TYPE_MONITOR_ONLY);
	window(800, 600);
	result(fader_rms("Discord") < 1e-4 && close_to(rest_rms(), R_EXTRA, 0.03), "5 Monitor Only source is excluded and absent from rest", "Discord %.5f rest %.4f", fader_rms("Discord"), rest_rms());
	obs_source_set_monitoring_type(discord, OBS_MONITORING_TYPE_NONE);

	// ── 6: off the streamed track ───────────────────────────────────────────────────────
	obs_source_set_audio_mixers(game, 2);
	window(800, 600);
	result(fader_rms("Game") < 1e-4 && close_to(rest_rms(), R_EXTRA, 0.03) && mismatch()[0] == 0, "6 source off the streamed track is excluded", "Game %.5f rest %.4f mismatch \"%s\"", fader_rms("Game"), rest_rms(),
	       mismatch());
	obs_source_set_audio_mixers(game, 1);
	os_sleep_ms(500);

	// From here the residual should be ~0: put the extra tone in a fader.
	assign("extra", "Discord");
	os_sleep_ms(800);

	// ── 7: fade transition 300 ms ───────────────────────────────────────────────────────
	obs_source_t *tr = NULL;
	obs_scene_t *sa = NULL, *sb = NULL;
	if (!have_transitions) {
		printf("SKIP  %-46s obs-transitions module not available\n", "7 fade transition");
	} else {
		obs_source_t *ta = tone("scene-a-tone", 990, 0.3, 1), *tb = tone("scene-b-tone", 1210, 0.3, 1);
		assign("scene-a-tone", "Voice");
		assign("scene-b-tone", "Voice");
		sa = obs_scene_create("A");
		sb = obs_scene_create("B");
		obs_scene_add(sa, ta);
		obs_scene_add(sb, tb);
		tr = obs_source_create("fade_transition", "fade", NULL, NULL);
		obs_set_output_source(0, tr);
		obs_transition_set(tr, obs_scene_get_source(sa));
		os_sleep_ms(1200);
		reset_stats();
		obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 300, obs_scene_get_source(sb));
		os_sleep_ms(700);
		fetch_stats();
		double during = rest_dbfs();
		window(500, 600);
		result(during < -40.0 && rest_dbfs() < -60.0 && !obs_source_active(ta) && obs_source_active(tb), "7 fade 300 ms: residual with the per-sample model",
		       "rest during the fade %.1f dBFS (want < -40), after %.1f dBFS; A active=%d B active=%d; mismatch \"%s\"", during, rest_dbfs(), obs_source_active(ta), obs_source_active(tb), mismatch());
		// Leave the program on an empty scene so A/B's tones don't count in later cases.
		obs_scene_t *empty = obs_scene_create("empty");
		obs_transition_set(tr, obs_scene_get_source(empty));
		obs_scene_release(empty);
		os_sleep_ms(300);
		obs_source_release(ta);
		obs_source_release(tb);
	}

	// ── 8: sync offset +200 ms ──────────────────────────────────────────────────────────
	obs_source_set_sync_offset(voice, 200000000LL);
	window(1500, 800);
	result(rest_dbfs() < -60.0 && close_to(fader_rms("Voice"), R_VOICE, 0.03), "8 sync offset +200 ms stays aligned", "rest %.1f dBFS, Voice %.4f (want %.4f)", rest_dbfs(), fader_rms("Voice"), R_VOICE);
	obs_source_set_sync_offset(voice, 0);
	os_sleep_ms(800);

	// ── 9: the same source twice in the live tree is counted once ───────────────────────
	obs_source_t *dup = tone("dup", 2000, 0.2, 1);
	assign("dup", "Music");
	obs_scene_t *inner = obs_scene_create("inner"), *outer = obs_scene_create("outer");
	obs_scene_add(inner, dup);
	obs_scene_add(outer, dup);
	obs_scene_add(outer, obs_scene_get_source(inner));
	if (tr) {
		obs_transition_set(tr, obs_scene_get_source(outer));
	} else {
		obs_set_output_source(0, obs_scene_get_source(outer));
	}
	window(1500, 800);
	double dup_want = sqrt(R_MUSIC * R_MUSIC + TONE_RMS(0.2) * TONE_RMS(0.2));
	result(close_to(fader_rms("Music"), dup_want, 0.03) && rest_dbfs() < -60.0, "9 duplicated source counted once", "Music %.4f (want %.4f: music + dup once), rest %.1f dBFS", fader_rms("Music"), dup_want, rest_dbfs());

	// ── 10: CPU with 12 more sources ────────────────────────────────────────────────────
	obs_scene_t *many = obs_scene_create("many");
	obs_source_t *extra12[12];
	for (int i = 0; i < 12; i++) {
		char name[32];
		snprintf(name, sizeof(name), "many-%d", i);
		extra12[i] = tone(name, 200 + 97 * i, 0.05, 1);
		obs_scene_add(many, extra12[i]);
		assign(name, i % 2 ? "Game" : "Music");
	}
	if (tr)
		obs_transition_set(tr, obs_scene_get_source(many));
	else
		obs_set_output_source(0, obs_scene_get_source(many));
	window(1500, 2000);
	double p99 = num_after(stats, "\"p99\":"), p50 = num_after(stats, "\"p50\":"), mx = num_after(stats, "\"max\":");
	// Budget per 21 ms tick with two dozen sources (gating, per-sample gains, summing, residual,
	// oracle and the test's own level sums): p50 under 100 µs, p99 under 500 µs (2.4 % of the
	// audio thread; the tail is scheduling noise on a shared CI machine).
	result(p50 < 100.0 && p99 < 500.0 && rest_dbfs() < -60.0, "10 CPU: two dozen sources, per-tick work", "p50 %.0f µs, p99 %.0f µs, max %.0f µs; rest %.1f dBFS", p50, p99, mx, rest_dbfs());

	// ── 12: mismatch injection: a source included although it isn't in the stream ─────
	// On track 1 but in no scene/output: its buffers carry audio every tick (libobs ticks
	// inactive sources) while the streamed track has none of it. The test hook bypasses the
	// active-gate, as a gating bug would.
	obs_source_t *inj = tone("injected", 1500, 0.3, 1);
	{
		obs_data_t *priv = obs_source_get_private_settings(inj);
		obs_data_set_bool(priv, "faders_test_force", true);
		obs_data_release(priv);
	}
	assign("injected", "Voice");
	reset_stats();
	uint64_t t0 = os_gettime_ns();
	double drop_ms = -1;
	for (int i = 0; i < 30 && drop_ms < 0; i++) {
		os_sleep_ms(100);
		fetch_stats();
		if (source_dropped("injected"))
			drop_ms = (double)(os_gettime_ns() - t0) / 1e6;
	}
	window(600, 600);
	result(drop_ms >= 0 && drop_ms <= 2500 && strcmp(mismatch(), "injected") == 0 && rest_dbfs() < -60.0 && close_to(fader_rms("Voice"), R_VOICE, 0.03),
	       "12 mismatch: wrongly included source is dropped", "dropped after %.0f ms (want ≤ 2000); mismatch \"%s\"; rest after %.1f dBFS; Voice %.4f", drop_ms, mismatch(), rest_dbfs(), fader_rms("Voice"));

teardown:
	for (int c = 0; c < 8; c++)
		obs_set_output_source(c, NULL);
	if (!fold && !wire) {
		if (tr) {
			obs_transition_clear(tr);
			obs_source_release(tr);
		}
		obs_scene_t *scenes[] = {live, preview, sa, sb, inner, outer, many};
		for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); i++) {
			if (!scenes[i])
				continue;
			obs_source_remove(obs_scene_get_source(scenes[i]));
			obs_scene_release(scenes[i]);
		}
		obs_source_release(hid);
		obs_source_release(prev);
		obs_source_release(dup);
		obs_source_release(inj);
		for (int i = 0; i < 12; i++)
			obs_source_release(extra12[i]);
	}
	obs_source_release(voice);
	obs_source_release(game);
	obs_source_release(music);
	obs_source_release(discord);
	obs_source_release(extra);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	printf("%s: %d check(s) failed\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
