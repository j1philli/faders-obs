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
// faders-split-mix <server> <key> split|cap|nosplit: the split fader's mixing, headless: real libobs,
// the real plugin, tone sources on two OBS tracks (Track 1 streams to Twitch, Track 3 is picked for
// YouTube), and the plugin's "faders_split_stats" for what each layer carried. Run by
// test/split-mix.mjs, which runs the mock hub (Pro, Twitch and YouTube) and checks the messages
// it got; "STEP <what>" lines ask it to change the hub's hello. One PASS/FAIL line per case.
//
//   split    different music per platform, Everything else per platform, the oracle, warnings,
//            downgrade and upgrade
//   cap      the hub allows 2 faders: Voice and the split Music are both on the wire (5 layers)
//   nosplit  no split fader: today's layers and no second tap
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>

#define RATE 48000
#define TONE_RMS(a) ((a) * 0.5590170f)

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

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail);
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
static bool call(const char *proc, calldata_t *cd)
{
	bool ok = proc_handler_call(obs_get_proc_handler(), proc, cd);
	if (!ok)
		fprintf(stderr, "proc %s is missing\n", proc);
	return ok;
}
static bool place(const char *source, const char *value)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "source", source);
	calldata_set_string(&cd, "value", value);
	bool ok = call("faders_place", &cd) && calldata_bool(&cd, "ok");
	if (!ok)
		fprintf(stderr, "place %s %s: %s\n", source, value, calldata_string(&cd, "error") ? calldata_string(&cd, "error") : "");
	calldata_free(&cd);
	return ok;
}
static bool dialog_set(const char *key, const char *choice)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "key", key);
	calldata_set_string(&cd, "choice", choice);
	bool ok = call("faders_dialog_set", &cd) && calldata_bool(&cd, "ok");
	calldata_free(&cd);
	return ok;
}

// OBS's UI task handler: tasks from other threads wait for pump_ui (this thread is the UI thread).
static pthread_t ui_thread;
static pthread_mutex_t ui_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	obs_task_t task;
	void *param;
} ui_queue[256];
static int ui_queued;
static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	if (pthread_equal(pthread_self(), ui_thread)) {
		task(param);
		return;
	}
	pthread_mutex_lock(&ui_mu);
	if (ui_queued < (int)(sizeof(ui_queue) / sizeof(*ui_queue)))
		ui_queue[ui_queued++] = (typeof(ui_queue[0])){task, param};
	pthread_mutex_unlock(&ui_mu);
	(void)wait;
}
static void pump_for(int ms)
{
	uint64_t end = os_gettime_ns() + (uint64_t)ms * 1000000ULL;
	do {
		for (;;) {
			pthread_mutex_lock(&ui_mu);
			if (!ui_queued) {
				pthread_mutex_unlock(&ui_mu);
				break;
			}
			typeof(ui_queue[0]) t = ui_queue[0];
			memmove(ui_queue, ui_queue + 1, sizeof(ui_queue[0]) * (size_t)--ui_queued);
			pthread_mutex_unlock(&ui_mu);
			t.task(t.param);
		}
		os_sleep_ms(10);
	} while (os_gettime_ns() < end);
}

static char stats[16384];
static void fetch(void)
{
	calldata_t cd = {0};
	if (call("faders_split_stats", &cd))
		snprintf(stats, sizeof(stats), "%s", calldata_string(&cd, "json") ? calldata_string(&cd, "json") : "{}");
	else
		snprintf(stats, sizeof(stats), "{}");
	calldata_free(&cd);
}
static double num(const char *key)
{
	char k[96];
	snprintf(k, sizeof(k), "\"%s\":", key);
	const char *p = strstr(stats, k);
	return p ? atof(p + strlen(k)) : NAN;
}
static bool flag(const char *key)
{
	char k[96];
	snprintf(k, sizeof(k), "\"%s\":true", key);
	return strstr(stats, k) != NULL;
}
static bool warned(const char *code)
{
	char k[96];
	snprintf(k, sizeof(k), "\"%s\"", code);
	const char *w = strstr(stats, "\"warnings\":[");
	const char *e = w ? strchr(w, ']') : NULL;
	const char *hit = w ? strstr(w, k) : NULL;
	return hit && hit < e;
}
static void window(int settle_ms, int window_ms)
{
	pump_for(settle_ms);
	fetch();
	pump_for(window_ms);
	fetch();
}
// Wait (up to 15 s) until the split is on (or off), as the hub's hello makes it.
static bool wait_split(bool on)
{
	for (int i = 0; i < 150; i++) {
		fetch();
		if (flag("on") == on && num("stems") > 0)
			return true;
		pump_for(100);
	}
	return false;
}
static bool close_to(double v, double want, double tol) { return fabs(v - want) <= tol * want + 1e-4; }

static void configure(const char *server, const char *key, const char *faders)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "server", server);
	calldata_set_string(&cd, "key", key);
	calldata_set_string(&cd, "faders", faders);
	calldata_set_bool(&cd, "only_live", false);
	call("faders_configure", &cd);
	calldata_free(&cd);
	calldata_t wc = {0};
	calldata_set_int(&wc, "track", 1);
	call("faders_watch_stream_track", &wc);
	calldata_free(&wc);
}

// Waits for the hub's hello (the plan decides who may tick it), then makes `fader` (index) split.
static bool make_split(int index, const char *fader, const char *tw, const char *yt, int track)
{
	for (int i = 0; i < 100; i++) {
		calldata_t cd = {0};
		call("faders_status", &cd);
		bool connected = calldata_string(&cd, "status") && strstr(calldata_string(&cd, "status"), "Connected");
		calldata_free(&cd);
		if (connected)
			break;
		pump_for(100);
	}
	pump_for(300);
	char key[32], value[64], choice[16];
	snprintf(key, sizeof(key), "split:%d", index);
	bool ok = dialog_set(key, "on");
	pump_for(100);
	snprintf(value, sizeof(value), "tw:%s", fader);
	ok = place(tw, value) && ok;
	snprintf(value, sizeof(value), "yt:%s", fader);
	ok = place(yt, value) && ok;
	snprintf(choice, sizeof(choice), "Track %d", track);
	ok = dialog_set("split_youtube_track", choice) && ok;
	pump_for(100);
	return ok;
}

static void run_split(const char *server, const char *key)
{
	const double A_VOICE = 0.5, A_TW = 0.3, A_YT = 0.25, A_EXTRA = 0.2;
	// Voice is on both tracks (shared); each platform's music is only on its own track.
	obs_source_t *voice = tone("voice", 440, A_VOICE, 1 | 4), *tw = tone("tw-music", 880, A_TW, 1), *yt = tone("yt-music", 1320, A_YT, 4);
	obs_set_output_source(1, voice);
	obs_set_output_source(2, tw);
	obs_set_output_source(3, yt);
	configure(server, key, "Voice,Music");
	place("voice", "f:Voice");
	bool made = make_split(1, "Music", "tw-music", "yt-music", 3);
	bool on = wait_split(true);
	result(made && on && num("stems") == 5 && num("youtube_track") == 3 && num("taps") == 2, "split 1 on: 5 layers, a second tap on Track 3", "made %d, on %d: %s", made, on, stats);
	printf("STEP check-stems-on\n");
	fflush(stdout);

	const double R_VOICE = TONE_RMS(A_VOICE), R_TW = TONE_RMS(A_TW), R_YT = TONE_RMS(A_YT), R_EXTRA = TONE_RMS(A_EXTRA);
	window(2500, 1500);
	result(num("ticks") > 30 && close_to(num("shared_rms"), R_VOICE, 0.03) && close_to(num("f_twitch_rms"), R_TW, 0.03) && close_to(num("f_youtube_rms"), R_YT, 0.03),
	       "split 2 Music · Twitch and Music · YouTube carry their own music", "Voice %.4f/%.4f F@twitch %.4f/%.4f F@youtube %.4f/%.4f (got/want), %g ticks", num("shared_rms"), R_VOICE,
	       num("f_twitch_rms"), R_TW, num("f_youtube_rms"), R_YT, num("ticks"));
	result(num("ee_twitch_rms") < 1e-4 && num("ee_youtube_rms") < 1e-4, "split 3 everything placed: both Everything elses silent", "EE@twitch %.6f EE@youtube %.6f", num("ee_twitch_rms"),
	       num("ee_youtube_rms"));
	result(num("leak_twitch_ee_youtube_music") < 0.01 && num("leak_youtube_ee_twitch_music") < 0.01, "split 4 EE@twitch has no YouTube music; EE@youtube no Twitch music",
	       "YouTube music in EE@twitch %.5f, Twitch music in EE@youtube %.5f (gain)", num("leak_twitch_ee_youtube_music"), num("leak_youtube_ee_twitch_music"));
	result(num("oracle_err_twitch") < 1e-5 && num("oracle_err_youtube") < 1e-5, "split 5 each platform's oracle is exact (shared + F@p + EE@p = track p)", "max |error| Twitch %.2e YouTube %.2e",
	       num("oracle_err_twitch"), num("oracle_err_youtube"));
	result(!warned("twitch-source-not-on-track") && !warned("youtube-source-not-on-track") && !warned("twitch-source-on-youtube-track") && !warned("youtube-source-on-twitch-track"),
	       "split 6 no warnings while each source is on its track", "%s", strstr(stats, "\"warnings\"") ? strstr(stats, "\"warnings\"") : stats);

	// Something unplaced on both tracks: in both Everything elses, and still no cross-platform music.
	obs_source_t *extra = tone("extra", 660, A_EXTRA, 1 | 4);
	obs_set_output_source(4, extra);
	window(2500, 1500);
	result(close_to(num("ee_twitch_rms"), R_EXTRA, 0.03) && close_to(num("ee_youtube_rms"), R_EXTRA, 0.03) && num("leak_twitch_ee_youtube_music") < 0.01 && num("leak_youtube_ee_twitch_music") < 0.01 &&
		       num("oracle_err_twitch") < 1e-5 && num("oracle_err_youtube") < 1e-5,
	       "split 7 an unplaced source is in each platform's Everything else", "EE@twitch %.4f EE@youtube %.4f (want %.4f), leaks %.5f %.5f, oracle %.1e %.1e", num("ee_twitch_rms"), num("ee_youtube_rms"),
	       R_EXTRA, num("leak_twitch_ee_youtube_music"), num("leak_youtube_ee_twitch_music"), num("oracle_err_twitch"), num("oracle_err_youtube"));
	obs_set_output_source(4, NULL);
	obs_source_release(extra);

	// Doubling: the Twitch music on the YouTube track too.
	obs_source_set_audio_mixers(tw, 1 | 4);
	window(1500, 1500);
	result(warned("twitch-source-on-youtube-track") && num("oracle_err_youtube") < 1e-5, "split 8 warning: Twitch source on the YouTube track", "%s; Twitch music in EE@youtube %.3f",
	       strstr(stats, "\"warnings\"") ? strstr(stats, "\"warnings\"") : stats, num("leak_youtube_ee_twitch_music"));
	printf("STEP check-status-doubling\n");
	fflush(stdout);
	pump_for(1500);
	obs_source_set_audio_mixers(tw, 1);

	// Wrong track: the YouTube music on Track 1 only. It isn't on YouTube's track, and it is
	// on Twitch's: it plays in Twitch's Everything else (as Twitch airs it), not in F@youtube.
	obs_source_set_audio_mixers(yt, 1);
	window(1500, 1500);
	result(warned("youtube-source-not-on-track") && warned("youtube-source-on-twitch-track") && num("f_youtube_rms") < 1e-4 && close_to(num("ee_twitch_rms"), R_YT, 0.03),
	       "split 9 warnings: YouTube source on the wrong track", "%s; F@youtube %.4f, EE@twitch %.4f (want %.4f)", strstr(stats, "\"warnings\"") ? strstr(stats, "\"warnings\"") : stats,
	       num("f_youtube_rms"), num("ee_twitch_rms"), R_YT);
	obs_source_set_audio_mixers(yt, 4);
	// The per-platform oracle: on Track 3 but in no output, so YouTube doesn't air it, and forced in
	// by the test hook (as a gating bug would): it is dropped and YouTube's warning says so.
	obs_set_output_source(3, NULL);
	obs_data_t *priv = obs_source_get_private_settings(yt);
	obs_data_set_bool(priv, "faders_test_force", true);
	obs_data_release(priv);
	{
		calldata_t cd = {0};
		call("faders_rebuild", &cd);
		calldata_free(&cd);
	}
	bool dropped = false;
	for (int i = 0; i < 60 && !dropped; i++) {
		pump_for(100);
		fetch();
		dropped = flag("youtube_dropped");
	}
	pump_for(700);
	fetch();
	result(dropped && warned("youtube-source-not-on-track"), "split 10 the YouTube oracle drops a source YouTube doesn't air", "dropped %d; %s", dropped, stats);
	priv = obs_source_get_private_settings(yt);
	obs_data_erase(priv, "faders_test_force");
	obs_data_release(priv);
	obs_set_output_source(3, yt);

	// Downgrade: the hub says Free. The split turns off (today's layers), picks kept.
	printf("STEP hello-free\n");
	fflush(stdout);
	bool off = wait_split(false);
	window(1500, 1000);
	result(off && num("stems") == 3 && num("taps") == 1, "split 11 Free: off, today's 3 layers, one tap", "%s", stats);
	printf("STEP check-stems-off\n");
	fflush(stdout);
	pump_for(1500);
	// Pro again: back on with the same picks.
	printf("STEP hello-pro\n");
	fflush(stdout);
	bool back = wait_split(true);
	pump_for(2500); // turning on again starts the YouTube oracle over; let it settle
	window(500, 1500);
	result(back && num("stems") == 5 && close_to(num("f_youtube_rms"), R_YT, 0.05) && close_to(num("f_twitch_rms"), R_TW, 0.03), "split 12 Pro again: on with the same picks",
	       "back %d, F@youtube %.4f F@twitch %.4f; %s", back, num("f_youtube_rms"), num("f_twitch_rms"), stats);
	printf("STEP done\n");
	fflush(stdout);
	pump_for(500);
	for (int ch = 1; ch <= 3; ch++)
		obs_set_output_source(ch, NULL);
	obs_source_release(voice);
	obs_source_release(tw);
	obs_source_release(yt);
}

static void run_cap(const char *server, const char *key)
{
	obs_source_t *voice = tone("voice", 440, 0.5, 1 | 4), *tw = tone("tw-music", 880, 0.3, 1), *yt = tone("yt-music", 1320, 0.25, 4), *game = tone("game", 660, 0.2, 1 | 4);
	obs_set_output_source(1, voice);
	obs_set_output_source(2, tw);
	obs_set_output_source(3, yt);
	obs_set_output_source(4, game);
	configure(server, key, "Voice,Music,Game");
	place("voice", "f:Voice");
	place("game", "f:Game");
	bool made = make_split(1, "Music", "tw-music", "yt-music", 3);
	bool on = wait_split(true);
	window(2000, 1000);
	result(made && on && num("stems") == 5 && close_to(num("ee_twitch_rms"), TONE_RMS(0.2), 0.05), "cap 1 maxStems 2: Voice + split Music on the wire, Game folded",
	       "made %d on %d; %s", made, on, stats);
	printf("STEP done\n");
	fflush(stdout);
	pump_for(500);
	for (int ch = 1; ch <= 4; ch++)
		obs_set_output_source(ch, NULL);
	obs_source_release(voice);
	obs_source_release(tw);
	obs_source_release(yt);
	obs_source_release(game);
}

static void run_nosplit(const char *server, const char *key)
{
	obs_source_t *voice = tone("voice", 440, 0.5, 1), *game = tone("game", 660, 0.4, 1), *music = tone("music", 880, 0.3, 1);
	obs_set_output_source(1, voice);
	obs_set_output_source(2, game);
	obs_set_output_source(3, music);
	configure(server, key, "Voice,Game,Music");
	place("voice", "f:Voice");
	place("game", "f:Game");
	place("music", "f:Music");
	window(3000, 1000);
	result(!flag("on") && num("taps") == 1 && num("stems") == 4, "nosplit 1 no split: today's 4 layers, one tap", "%s", stats);
	printf("STEP check-stems-nosplit\n");
	fflush(stdout);
	pump_for(1500);
	printf("STEP done\n");
	fflush(stdout);
	pump_for(500);
	for (int ch = 1; ch <= 3; ch++)
		obs_set_output_source(ch, NULL);
	obs_source_release(voice);
	obs_source_release(game);
	obs_source_release(music);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <server> <key> split|cap|nosplit\n", argv[0]);
		return 2;
	}
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-split-mix-%s-%d", tmp, argv[3], (int)getpid());
	ui_thread = pthread_self();
	if (!obs_startup("en-US", config_dir, NULL))
		return 1;
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC, .gpu_conversion = true,
	};
	obs_set_ui_task_handler(ui_task_handler);
	struct obs_audio_info ai = {.samples_per_sec = RATE, .speakers = SPEAKERS_STEREO};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS || !obs_reset_audio(&ai))
		return 1;
	obs_register_source(&tone_info);
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", FADERS_PLUGIN_BIN);
		return 1;
	}
	if (!strcmp(argv[3], "split"))
		run_split(argv[1], argv[2]);
	else if (!strcmp(argv[3], "cap"))
		run_cap(argv[1], argv[2]);
	else
		run_nosplit(argv[1], argv[2]);
	obs_wait_for_destroy_queue();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	fflush(stdout);
	_exit(fails ? 1 : 0);
}
