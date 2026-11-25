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
// faders-debug-report: the debug report through the real plugin, headless (real libobs, no hub):
// faders_debug_report(names) builds it from what the plugin knows. Names on (the default) lists
// sources and faders; names off leaves none; the plugin's own log lines are in it; the plugin key
// never is, and if it would be the report is refused (fail closed). The JSON block is valid JSON.
// Nothing is uploaded: the plugin has no code that sends this report anywhere.
// One PASS/FAIL line per check; exits non-zero if any fails.
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAKE_KEY "fdr_live_FAKEheadlessKEY0123456789abcd"

static const char *stub_name(void *u) { return "Stand-in"; }
static void *stub_create(obs_data_t *settings, obs_source_t *source) { return bzalloc(1); }
static void stub_destroy(void *data) { bfree(data); }
#define KIND(kind_id) {.id = kind_id, .type = OBS_SOURCE_TYPE_INPUT, .output_flags = OBS_SOURCE_AUDIO, .get_name = stub_name, .create = stub_create, .destroy = stub_destroy}
static struct obs_source_info kinds[] = {
	KIND("wasapi_input_capture"),
	KIND("wasapi_process_output_capture"),
	// A source kind whose id carries the key: ids are copied into the report as they are, so
	// this is the one way the key could reach it, and the report must refuse to be built.
	KIND("leaky_" FAKE_KEY),
};

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}
static bool call(const char *proc, calldata_t *cd)
{
	bool ok = proc_handler_call(obs_get_proc_handler(), proc, cd);
	if (!ok)
		fprintf(stderr, "proc %s is missing\n", proc);
	return ok;
}

static bool report_ok;
static char *report, *report_error;
static void build(bool names)
{
	calldata_t cd = {0};
	calldata_set_bool(&cd, "names", names);
	bool have = call("faders_debug_report", &cd);
	report_ok = have && calldata_bool(&cd, "ok");
	bfree(report);
	bfree(report_error);
	report = bstrdup(have && calldata_string(&cd, "report") ? calldata_string(&cd, "report") : "");
	report_error = bstrdup(have && calldata_string(&cd, "error") ? calldata_string(&cd, "error") : "");
	calldata_free(&cd);
}
static bool has(const char *needle)
{
	return report && strstr(report, needle) != NULL;
}
// The ```json block of the report, parsed by libobs (NULL if missing or not valid JSON).
static obs_data_t *json_block(void)
{
	const char *a = report ? strstr(report, "```json\n") : NULL;
	const char *b = a ? strstr(a + 8, "\n```") : NULL;
	if (!a || !b)
		return NULL;
	char *json = bstrdup_n(a + 8, (size_t)(b - a - 8));
	obs_data_t *d = obs_data_create_from_json(json);
	bfree(json);
	return d;
}

static obs_source_t *make(const char *kind, const char *name)
{
	obs_source_t *s = obs_source_create(kind, name, NULL, NULL);
	obs_source_set_audio_mixers(s, 1);
	return s;
}

int main(void)
{
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-debug-report-%d-%llu", tmp, (int)getpid(), (unsigned long long)os_gettime_ns());
	if (!obs_startup("en-US", config_dir, NULL))
		return 1;
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC, .gpu_conversion = true,
	};
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS || !obs_reset_audio(&ai))
		return 1;
	for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++)
		obs_register_source(&kinds[i]);
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod))
		return 1;
	calldata_t cd = {0};
	calldata_set_string(&cd, "server", "ws://127.0.0.1:9/ingest/debug");
	calldata_set_string(&cd, "key", FAKE_KEY);
	calldata_set_string(&cd, "faders", "Voice,Game,Music");
	calldata_set_bool(&cd, "only_live", false);
	call("faders_configure", &cd);
	calldata_free(&cd);

	obs_source_t *mic = make("wasapi_input_capture", "Podcast Mic");
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify Premium");
	calldata_t ac = {0};
	calldata_set_string(&ac, "source", "Podcast Mic");
	calldata_set_string(&ac, "fader", "Voice");
	call("faders_assign", &ac);
	calldata_free(&ac);

	// Names on (the default).
	build(true);
	if (getenv("FADERS_DUMP_REPORT"))
		printf("%s\n", report);
	obs_data_t *json = json_block();
	result(report_ok && has("## Faders debug info") && has("Nothing was uploaded") && has("**Voice**: Podcast Mic") && has("Spotify Premium (new)") && has("\"Podcast Mic\" → Voice") &&
		       has("loaded (version ") && !has(FAKE_KEY) && json && obs_data_get_int(json, "schema") >= 1,
	       "names on: sources, faders and the plugin's log; no key; valid JSON", "ok %d, %zu bytes, JSON %s; error \"%s\"", report_ok, strlen(report), json ? "valid" : "INVALID", report_error);
	obs_data_release(json);

	// Names off.
	build(false);
	json = json_block();
	result(report_ok && !has("Podcast Mic") && !has("Spotify") && !has("Voice") && has("Source 1") && has("Fader 1") && json, "names off: no source or fader name anywhere", "ok %d, Podcast Mic %d, Spotify %d, Voice %d",
	       report_ok, has("Podcast Mic"), has("Spotify"), has("Voice"));
	obs_data_release(json);

	// Fail closed.
	obs_source_t *leaky = make("leaky_" FAKE_KEY, "Leaky");
	build(true);
	result(!report_ok && !*report && *report_error, "fails closed when the key would be in it", "ok %d, %zu bytes of report, error \"%s\"", report_ok, strlen(report), report_error);
	obs_source_remove(leaky);
	obs_source_release(leaky);
	obs_source_remove(mic);
	obs_source_release(mic);
	obs_source_remove(spotify);
	obs_source_release(spotify);

	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	fflush(stdout);
	_exit(fails ? 1 : 0);
}
