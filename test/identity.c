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
// faders-identity: how the dialog and the mixer identify a source, headless (real libobs, the
// real plugin, no hub needed). Sources are registered under OBS's own kind ids (Audio Input
// Capture on Windows and macOS, Audio Output Capture, Application Audio Capture, Video Capture
// Device) and named the ways streamers name them: renamed, two that differ only in case, a
// duplicate, unicode, very long, two capturing the same device. For each, every source must be
// listed in the dialog exactly once, under the fader the mixer actually plays it on.
// One PASS/FAIL line per case; exits non-zero if any fails.
//
//   faders-identity
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
#include <stdarg.h>
#include <unistd.h>

// ── Stand-ins for OBS's audio capture kinds (they only need the id and the audio flag) ────
static const char *fake_name(void *u) { return "Fake capture"; }
static void *fake_create(obs_data_t *settings, obs_source_t *source) { return source; }
static void fake_destroy(void *data) {}
#define FAKE(kind) {.id = kind, .type = OBS_SOURCE_TYPE_INPUT, .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE, \
		     .get_name = fake_name, .create = fake_create, .destroy = fake_destroy}
static struct obs_source_info kinds[] = {
	FAKE("wasapi_input_capture"),  FAKE("wasapi_output_capture"), FAKE("wasapi_process_output_capture"),
	FAKE("coreaudio_input_capture"), FAKE("dshow_input"),
};

// ── Harness ─────────────────────────────────────────────────────────────────────────────
static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}

static void call(const char *proc, calldata_t *cd)
{
	if (!proc_handler_call(obs_get_proc_handler(), proc, cd))
		fprintf(stderr, "proc %s failed\n", proc);
}

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
static void pump_ui(void)
{
	for (;;) {
		pthread_mutex_lock(&ui_mu);
		if (!ui_queued) {
			pthread_mutex_unlock(&ui_mu);
			return;
		}
		typeof(ui_queue[0]) t = ui_queue[0];
		memmove(ui_queue, ui_queue + 1, sizeof(ui_queue[0]) * (size_t)--ui_queued);
		pthread_mutex_unlock(&ui_mu);
		t.task(t.param);
	}
}

static obs_source_t *capture(const char *kind, const char *name, const char *device)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", device);
	obs_source_t *src = obs_source_create(kind, name, s, NULL);
	obs_data_release(s);
	obs_source_set_audio_mixers(src, 1);
	return src;
}

// Put a source on a fader the way the plugin stores it ("" = Everything else).
static void place(obs_source_t *src, const char *fader)
{
	obs_data_t *priv = obs_source_get_private_settings(src);
	obs_data_set_string(priv, "faders_fader", fader);
	obs_data_release(priv);
}

static void configure(const char *faders)
{
	calldata_t cd = {0};
	// Nothing listens there: the uplink keeps retrying and the mixer runs, which is all we need.
	calldata_set_string(&cd, "server", "ws://127.0.0.1:9/ingest/identity");
	calldata_set_string(&cd, "key", "identity-test");
	calldata_set_string(&cd, "faders", faders);
	calldata_set_bool(&cd, "only_live", false);
	call("faders_configure", &cd);
	calldata_free(&cd);
	calldata_t wc = {0};
	calldata_set_int(&wc, "track", 1);
	call("faders_watch_stream_track", &wc);
	calldata_free(&wc);
}

static void rebuild(void)
{
	calldata_t cd = {0};
	call("faders_rebuild", &cd);
	calldata_free(&cd);
	pump_ui();
}

static char rows[65536];
static const char *dialog(void)
{
	calldata_t cd = {0};
	call("faders_dialog", &cd);
	snprintf(rows, sizeof(rows), "%s", calldata_string(&cd, "rows") ? calldata_string(&cd, "rows") : "");
	calldata_free(&cd);
	return rows;
}

static char stats[65536];
static void fetch_stats(void)
{
	calldata_t cd = {0};
	call("faders_mixer_stats", &cd);
	snprintf(stats, sizeof(stats), "%s", calldata_string(&cd, "json") ? calldata_string(&cd, "json") : "{}");
	calldata_free(&cd);
}

// Every fader the dialog lists `label` under (a row showing it), joined with ","; "" if none.
// Rows after a fader's first have the label " ", so the fader is the last non-blank one.
static void dialog_faders_of(const char *label, char *out, size_t size)
{
	char fader[128] = "";
	out[0] = 0;
	for (const char *line = rows; *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char buf[2048];
		snprintf(buf, sizeof(buf), "%.*s", (int)(end - line), line);
		line = end + 1;
		char *f[4] = {buf, NULL, NULL, NULL};
		for (int i = 1; i < 4 && f[i - 1]; i++) {
			char *tab = strchr(f[i - 1], '\t');
			if (tab) {
				*tab = 0;
				f[i] = tab + 1;
			}
		}
		if (!f[1] || !f[2])
			continue; // an info row
		if (strcmp(f[1], " ") != 0)
			snprintf(fader, sizeof(fader), "%s", f[1]);
		if (strncmp(f[0], "row:", 4) == 0 && strcmp(f[2], label) == 0) {
			if (*out)
				strncat(out, ",", size - strlen(out) - 1);
			strncat(out, fader, size - strlen(out) - 1);
		}
	}
}

// Whether "Everything else: …" names `label`.
static bool dialog_rest_has(const char *label)
{
	const char *p = strstr(rows, "rest_info\t");
	if (!p)
		return false;
	const char *end = strchr(p, '\n');
	const char *hit = strstr(p, label);
	return hit && (!end || hit < end);
}

// The fader the mixer plays `name` on ("" = Everything else), or NULL if it doesn't know it.
static const char *mixer_fader_of(const char *name)
{
	static char out[128];
	char key[512];
	snprintf(key, sizeof(key), "{\"name\":\"%s\",\"fader\":\"", name);
	const char *p = strstr(stats, key);
	if (!p)
		return NULL;
	p += strlen(key);
	size_t n = strcspn(p, "\"");
	snprintf(out, sizeof(out), "%.*s", (int)n, p);
	return out;
}

// One source: listed exactly once, under `want` (or in Everything else when want is ""), and the
// mixer agrees.
static bool check_source(obs_source_t *src, const char *want, struct dstr *why)
{
	const char *label = obs_source_get_name(src);
	char where[512];
	dialog_faders_of(label, where, sizeof(where));
	bool rest = dialog_rest_has(label);
	const char *mix = mixer_fader_of(label);
	bool ok = *want ? strcmp(where, want) == 0 && !rest : !*where && rest;
	ok = ok && mix && strcmp(mix, want) == 0;
	if (!ok)
		dstr_catf(why, "[\"%s\" (%s): dialog rows under \"%s\"%s; mixer: %s%s%s; want %s] ", label, obs_source_get_id(src), where,
			  rest ? " + Everything else" : "", mix ? "\"" : "", mix ? mix : "not listed", mix ? "\"" : "", *want ? want : "Everything else");
	return ok;
}

static bool valid_utf8(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		size_t n = *p < 0x80 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
		if (!n)
			return false;
		for (size_t i = 1; i < n; i++)
			if ((p[i] & 0xc0) != 0x80)
				return false;
		p += n;
	}
	return true;
}

// Every label the dialog shows (row descriptions, shown choices, and the choices) is UTF-8.
static bool dialog_utf8(void)
{
	return valid_utf8(rows);
}

static void case_check(const char *name, obs_source_t **srcs, const char **want, int n)
{
	rebuild();
	dialog();
	fetch_stats();
	if (getenv("IDENTITY_DUMP"))
		printf("--- %s\n%s--- mixer %s\n", name, rows, stats);
	struct dstr why = {0};
	bool ok = true;
	for (int i = 0; i < n; i++)
		ok = check_source(srcs[i], want[i], &why) && ok;
	result(ok, name, "%s", ok ? "listed once each, where the mixer plays it" : why.array);
	dstr_free(&why);
}

static void remove_all(obs_source_t **srcs, int n)
{
	for (int i = 0; i < n; i++) {
		obs_source_remove(srcs[i]);
		obs_source_release(srcs[i]);
		srcs[i] = NULL;
	}
	rebuild();
}

int main(int argc, char **argv)
{
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-identity-config", tmp);
	ui_thread = pthread_self();
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
	obs_set_ui_task_handler(ui_task_handler);
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed\n");
		return 1;
	}
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai)) {
		fprintf(stderr, "obs_reset_audio failed\n");
		return 1;
	}
	for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++)
		obs_register_source(&kinds[i]);
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", FADERS_PLUGIN_BIN);
		return 1;
	}
	configure("Voice,Game,Music");

	// ── 1: Mr Sharkey's setup: a mic and a virtual cable both put on Voice ──────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", "Music", "{vb-cable-output}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("1 two virtual-cable inputs placed on Voice", s, want, 2);
		remove_all(s, 2);
	}
	// ── 2: the same, never placed: the rule table puts Audio Input Captures on Voice ───────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", "Music", "{vb-cable-output}")};
		const char *want[] = {"Voice", "Voice"};
		case_check("2 two virtual-cable inputs on Voice by default", s, want, 2);
		remove_all(s, 2);
	}
	// ── 3: renamed after it was placed (assignments follow the source, not its name) ──────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", "Audio Input Capture 2", "{vb-cable-output}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		rebuild();
		dialog();
		obs_source_set_name(s[1], "Music (VB-Cable)");
		const char *want[] = {"Voice", "Voice"};
		case_check("3 a virtual cable renamed after placing it", s, want, 2);
		remove_all(s, 2);
	}
	// ── 4: names that differ only in case ─────────────────────────────────────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "music", "{cable-a}"), capture("wasapi_input_capture", "Music", "{cable-b}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("4 two inputs named Music and music", s, want, 2);
		remove_all(s, 2);
	}
	// ── 5: a duplicate name (OBS renames the second one) ────────────────────────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Music", "{cable-a}"), capture("wasapi_input_capture", "Music", "{cable-b}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("5 two inputs created with the same name", s, want, 2);
		remove_all(s, 2);
	}
	// ── 6: both capture the same device ─────────────────────────────────────────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{vb-cable-output}"), capture("wasapi_input_capture", "Music", "{vb-cable-output}")};
		place(s[0], "Voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("6 two inputs on the same virtual-cable device", s, want, 2);
		remove_all(s, 2);
	}
	// ── 7: unicode and markup-like characters ──────────────────────────────────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "🎤 Mic", "{mic}"), capture("wasapi_input_capture", "Música <VB-Cable> & Spotify 🎵", "{cable}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("7 unicode and <&> in source names", s, want, 2);
		remove_all(s, 2);
	}
	// ── 8: the other audio kinds a virtual cable can be captured with ────────────────────────
	{
		obs_source_t *s[] = {capture("coreaudio_input_capture", "Mic (mac)", "BlackHole2ch_UID"), capture("dshow_input", "Cable via Video Capture Device", "{cable}"),
				     capture("wasapi_output_capture", "CABLE Input capture", "{cable-input}"), capture("wasapi_process_output_capture", "Spotify capture", "")};
		for (int i = 0; i < 4; i++)
			place(s[i], "Voice");
		const char *want[] = {"Voice", "Voice", "Voice", "Voice"};
		case_check("8 every audio capture kind placed on Voice", s, want, 4);
		remove_all(s, 4);
	}
	// ── 9: the stored fader name differs in case from the list ──────────────────────────────
	{
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", "Music", "{cable}")};
		place(s[0], "Voice");
		place(s[1], "voice");
		const char *want[] = {"Voice", "Voice"};
		case_check("9 stored as \"voice\", listed as Voice", s, want, 2);
		remove_all(s, 2);
	}
	// ── 10: a long name, cut where the dialog cuts it ─────────────────────────────────────
	// 92 ASCII bytes then "é" (2 bytes) repeated: a cut at 95 bytes lands inside a character.
	{
		char longname[256];
		snprintf(longname, sizeof(longname), "%s", "Music from Voicemeeter Aux B2 through the VB-Audio Virtual Cable for the stream long name");
		while (strlen(longname) < 92)
			strcat(longname, "x");
		longname[92] = 0;
		strcat(longname, "éééééééééééé");
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", longname, "{cable}")};
		place(s[0], "Voice");
		place(s[1], "Voice");
		rebuild();
		dialog();
		// A long name is shortened in the dialog; the row must still be there, as valid text.
		char where[512];
		char prefix[64];
		snprintf(prefix, sizeof(prefix), "%.60s", longname);
		int rows_under_voice = 0;
		for (const char *p = rows; (p = strstr(p, prefix)); p++)
			rows_under_voice++;
		bool utf8 = dialog_utf8();
		dialog_faders_of("Mic", where, sizeof(where));
		result(rows_under_voice >= 1 && utf8 && strcmp(where, "Voice") == 0, "10 a 115-byte unicode source name", "row %s; dialog text %s",
		       rows_under_voice ? "shown" : "MISSING", utf8 ? "valid UTF-8" : "NOT valid UTF-8 (the name is cut inside a character)");
		remove_all(s, 2);
	}
	// ── 11: a unicode fader name ────────────────────────────────────────────────────────────
	{
		configure("Vöice 🎙,Game,Music");
		obs_source_t *s[] = {capture("wasapi_input_capture", "Mic", "{mic}"), capture("wasapi_input_capture", "Music", "{cable}")};
		place(s[0], "Vöice 🎙");
		place(s[1], "Vöice 🎙");
		const char *want[] = {"Vöice 🎙", "Vöice 🎙"};
		case_check("11 a unicode fader name", s, want, 2);
		remove_all(s, 2);
		configure("Voice,Game,Music");
	}

	pump_ui();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	fflush(stdout);
	// Like the other hosts, skip teardown races: exit straight away with the result.
	_exit(fails ? 1 : 0);
}
