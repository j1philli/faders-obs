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
// Harness for the test/panel-listing-*.c tests: Tools → Faders' "What's on each fader" rows,
// headless. Real libobs and the real plugin, no hub (the dialog is built from the settings alone,
// so nothing connects). Sources are silent stand-ins registered under real source ids
// (wasapi_input_capture: an Audio Input Capture, e.g. of a virtual cable; browser_source), so the
// plugin's own rules place them. Each test prints one PASS/FAIL line per case and exits non-zero
// if any fails.
#pragma once
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *panel_wasapi_name(void *u) { return "Audio Input Capture"; }
static const char *panel_browser_name(void *u) { return "Browser"; }
static void *panel_stub_create(obs_data_t *settings, obs_source_t *source) { return bzalloc(1); }
static void panel_stub_destroy(void *data) { bfree(data); }
static struct obs_source_info panel_wasapi_info = {
	.id = "wasapi_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = panel_wasapi_name,
	.create = panel_stub_create,
	.destroy = panel_stub_destroy,
};
// Browser sources have audio, so every one in a scene collection is an audio input to the plugin.
static struct obs_source_info panel_browser_info = {
	.id = "browser_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = panel_browser_name,
	.create = panel_stub_create,
	.destroy = panel_stub_destroy,
};

static int panel_fails;
static inline void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-56s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		panel_fails++;
}

static inline void call(const char *proc, calldata_t *cd)
{
	if (!proc_handler_call(obs_get_proc_handler(), proc, cd))
		fprintf(stderr, "proc %s failed\n", proc);
}

// An Audio Input Capture (wasapi_input_capture) of a device, e.g. a virtual cable.
static inline obs_source_t *cable(const char *name, const char *device_id)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", device_id);
	obs_source_t *src = obs_source_create("wasapi_input_capture", name, s, NULL);
	obs_data_release(s);
	return src;
}

// Put a source on a fader the way the plugin stores it ("" = Everything else).
static inline void place(obs_source_t *s, const char *fader)
{
	obs_data_t *priv = obs_source_get_private_settings(s);
	obs_data_set_string(priv, "faders_fader", fader);
	obs_data_release(priv);
}
// Where a source is stored as being: a fader name, "" (Everything else) or "(auto)".
static inline const char *placed(obs_source_t *s)
{
	static char bufs[4][64];
	static int next;
	char *out = bufs[next++ % 4];
	obs_data_t *priv = obs_source_get_private_settings(s);
	snprintf(out, sizeof(bufs[0]), "%s", obs_data_has_user_value(priv, "faders_fader") ? obs_data_get_string(priv, "faders_fader") : "(auto)");
	obs_data_release(priv);
	return out;
}

// faders_assign_track: a fader name, "" = Everything else, "none" = not used.
static inline void set_track(int t, const char *v)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "track", t);
	calldata_set_string(&cd, "fader", v);
	call("faders_assign_track", &cd);
	calldata_free(&cd);
}

// The dialog as the window builds it (faders_dialog), which is also what a redraw does:
// key<TAB>label<TAB>shown<TAB>choices per dropdown, key<TAB>text per info row.
static char panel_rows[131072];
static inline const char *dialog(void)
{
	calldata_t cd = {0};
	call("faders_dialog", &cd);
	snprintf(panel_rows, sizeof(panel_rows), "%s", calldata_string(&cd, "rows") ? calldata_string(&cd, "rows") : "");
	calldata_free(&cd);
	return panel_rows;
}
// Pick a choice in one dropdown by its visible text, like a click.
static inline bool pick(const char *key, const char *choice)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "key", key);
	calldata_set_string(&cd, "choice", choice);
	call("faders_dialog_set", &cd);
	bool ok = calldata_bool(&cd, "ok");
	calldata_free(&cd);
	return ok;
}

// Field i (0-3) of a tab-separated row line [line, end) into out; whether it exists.
static inline bool panel_field(const char *line, const char *end, int i, char *out, size_t size)
{
	const char *p = line;
	for (int k = 0; k < i; k++) {
		p = memchr(p, '\t', (size_t)(end - p));
		if (!p)
			return false;
		p++;
	}
	const char *q = memchr(p, '\t', (size_t)(end - p));
	if (!q)
		q = end;
	snprintf(out, size, "%.*s", (int)(q - p), p);
	return true;
}

// The source/track names shown in one fader's block (its rows up to the next fader's first row),
// "|"-joined, from the last dialog().
static inline const char *block(const char *fader)
{
	static char bufs[4][8192];
	static int next;
	char *out = bufs[next++ % 4];
	out[0] = 0;
	bool in = false;
	for (const char *line = panel_rows; *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char key[64], label[128], shown[256], choices[8];
		bool row = panel_field(line, end, 3, choices, sizeof(choices));
		panel_field(line, end, 0, key, sizeof(key));
		panel_field(line, end, 1, label, sizeof(label));
		panel_field(line, end, 2, shown, sizeof(shown));
		line = end + 1;
		if (!row)
			continue; // an info row
		if (strcmp(label, " ") != 0)
			in = strcmp(label, fader) == 0;
		if (in && strncmp(key, "row:", 4) == 0)
			snprintf(out + strlen(out), sizeof(bufs[0]) - strlen(out), "%s%s", *out ? "|" : "", shown);
	}
	return out;
}
static inline bool in_block(const char *fader, const char *name)
{
	char want[256], have[8200];
	snprintf(want, sizeof(want), "|%s|", name);
	snprintf(have, sizeof(have), "|%s|", block(fader));
	return strstr(have, want) != NULL;
}
// The key of the dropdown currently showing `shown` (from the last dialog()), or "".
static inline const char *key_showing(const char *shown)
{
	static char key[64];
	key[0] = 0;
	for (const char *line = panel_rows; *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char s[256];
		if (panel_field(line, end, 3, s, sizeof(s)) && panel_field(line, end, 2, s, sizeof(s)) && strcmp(s, shown) == 0) {
			panel_field(line, end, 0, key, sizeof(key));
			return key;
		}
		line = end + 1;
	}
	return key;
}
// The "Everything else: …" line.
static inline const char *everything_else(void)
{
	static char out[8192];
	const char *p = strstr(panel_rows, "rest_info\t");
	const char *e = p ? strchr(p, '\n') : NULL;
	snprintf(out, sizeof(out), "%.*s", p && e ? (int)(e - p) : 0, p ? p : "");
	return out;
}

static inline bool panel_find_settings(void *param, obs_source_t *s)
{
	if (strcmp(obs_source_get_id(s), "faders_uplink") != 0)
		return true;
	*(obs_source_t **)param = obs_source_get_ref(s);
	return false;
}

// Start libobs, load the plugin with these fader names (offline: only while live) and return its
// private settings source (what OBS's properties window edits), or NULL on failure.
static inline obs_source_t *panel_start(const char *faders)
{
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-panel-listing-%llu", tmp, (unsigned long long)os_gettime_ns());
	if (!obs_startup("en-US", config_dir, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return NULL;
	}
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC, .gpu_conversion = true,
	};
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS || !obs_reset_audio(&ai)) {
		fprintf(stderr, "obs_reset_video/audio failed\n");
		return NULL;
	}
	obs_register_source(&panel_wasapi_info);
	obs_register_source(&panel_browser_info);
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", FADERS_PLUGIN_BIN);
		return NULL;
	}
	calldata_t cd = {0};
	calldata_set_string(&cd, "server", "ws://127.0.0.1:9/ingest/none");
	calldata_set_string(&cd, "key", "panel-test");
	calldata_set_string(&cd, "faders", faders);
	calldata_set_bool(&cd, "only_live", true);
	call("faders_configure", &cd);
	calldata_free(&cd);
	obs_source_t *settings_source = NULL;
	obs_enum_all_sources(panel_find_settings, &settings_source);
	return settings_source;
}

static inline int panel_end(obs_source_t *settings_source)
{
	obs_source_release(settings_source);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	printf("%s: %d check(s) failed\n", panel_fails ? "FAIL" : "PASS", panel_fails);
	return panel_fails ? 1 : 0;
}
