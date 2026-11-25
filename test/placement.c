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
// faders-placement <scenario>: where sources go, headless (real libobs, the real plugin, no hub).
//
// Nothing is placed for the streamer: a source with no saved placement is NEW and plays in
// Everything else, whatever its kind, name or OBS tracks. Faders start empty (Voice, Game and Music
// are only names). A move is checked against the faders as they are now, and a move to a fader
// that is gone, past the plan, or to a source that is gone changes nothing.
//
// Scenarios (one process each, so each starts from a fresh config folder):
//   model      unplaced sources are NEW in Everything else; one-step moves; "~new" as a fader name
//   legacy     old track:N settings change nothing and are erased
//   duplicate  a duplicated source keeps the original's fader (OBS's default); load vs import
//   validate   moves to locked, deleted and stale faders, and of deleted sources, are rejected
//   capacity   100, 128, 256 and 257 audio inputs, all NEW and all placed
//   sharkey2   Mr Sharkey's second panel: an input made after the mixer's limit is still named
//   refresh    an open window follows audio inputs being created, renamed and removed (coalesced)
//   cancel     OBS's Cancel / Discard after a move moves nothing
//   firstrun   Tools → Faders opens once on the first start when nothing is placed
//   firstrun-placed   ... and not when something already is
// One PASS/FAIL line per check; exits non-zero if any fails.
#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
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

// ── Stand-ins for OBS's source kinds (only the id and the flags matter to the plugin) ─────
static const char *stub_name(void *u) { return "Stand-in"; }
static void *stub_create(obs_data_t *settings, obs_source_t *source) { return bzalloc(1); }
static void stub_destroy(void *data) { bfree(data); }
#define KIND(kind_id, flags) {.id = kind_id, .type = OBS_SOURCE_TYPE_INPUT, .output_flags = (flags), .get_name = stub_name, .create = stub_create, .destroy = stub_destroy}
static struct obs_source_info kinds[] = {
	// Device captures can't be duplicated in OBS (Paste makes a reference).
	KIND("wasapi_input_capture", OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE),
	KIND("coreaudio_input_capture", OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE),
	KIND("wasapi_output_capture", OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE),
	KIND("wasapi_process_output_capture", OBS_SOURCE_AUDIO),
	KIND("browser_source", OBS_SOURCE_AUDIO),
	KIND("ffmpeg_source", OBS_SOURCE_AUDIO),
	KIND("game_capture", OBS_SOURCE_AUDIO),
	KIND("window_capture", OBS_SOURCE_AUDIO),
};

// ── Harness ─────────────────────────────────────────────────────────────────────────────
static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail);
	fflush(stdout);
	if (!ok)
		fails++;
}

// Whether the plugin has this procedure (a missing one fails the check that needs it).
static bool call(const char *proc, calldata_t *cd)
{
	bool ok = proc_handler_call(obs_get_proc_handler(), proc, cd);
	if (!ok)
		fprintf(stderr, "proc %s is missing\n", proc);
	return ok;
}

// OBS's UI task handler as the app has it: a task queued from the UI thread (this main thread)
// runs at once; one from another thread waits for pump_ui.
static pthread_t ui_thread;
static pthread_mutex_t ui_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	obs_task_t task;
	void *param;
} ui_queue[1024];
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
// Run the UI thread for `ms` (the plugin's graphics-tick work reaches it as queued tasks).
static void pump_for(int ms)
{
	uint64_t end = os_gettime_ns() + (uint64_t)ms * 1000000ULL;
	while (os_gettime_ns() < end) {
		pump_ui();
		os_sleep_ms(10);
	}
	pump_ui();
}

static char config_dir[512];
static obs_source_t *settings_source; // the plugin's private settings holder (what the window edits)

static bool find_settings(void *param, obs_source_t *s)
{
	if (strcmp(obs_source_get_id(s), "faders_uplink") != 0)
		return true;
	*(obs_source_t **)param = obs_source_get_ref(s);
	return false;
}

static char settings_path[600];
// The plugin's settings.json as written now ("" if there is none).
static const char *settings_file(void)
{
	static char *text;
	bfree(text);
	text = os_quick_read_utf8_file(settings_path);
	return text ? text : "";
}

static void configure(const char *faders, bool run)
{
	calldata_t cd = {0};
	// Nothing listens there: with run set the uplink keeps retrying and the mixer runs.
	calldata_set_string(&cd, "server", "ws://127.0.0.1:9/ingest/placement");
	calldata_set_string(&cd, "key", "fdr_test_placementFAKEKEY");
	calldata_set_string(&cd, "faders", faders);
	calldata_set_bool(&cd, "only_live", !run);
	call("faders_configure", &cd);
	calldata_free(&cd);
	calldata_t wc = {0};
	calldata_set_int(&wc, "track", 1);
	call("faders_watch_stream_track", &wc);
	calldata_free(&wc);
}

// Start libobs with a fresh config folder (or `settings_json` already saved as the plugin's
// settings, as an earlier version left them) and load the plugin.
static bool start(const char *name, const char *settings_json)
{
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	snprintf(config_dir, sizeof(config_dir), "%s/faders-placement-%s-%d-%llu", tmp, name, (int)getpid(), (unsigned long long)os_gettime_ns());
	// obs_startup's folder is the modules' config folder: the plugin keeps faders/settings.json there.
	snprintf(settings_path, sizeof(settings_path), "%s/faders/settings.json", config_dir);
	if (settings_json) {
		char dir[600];
		snprintf(dir, sizeof(dir), "%s/faders", config_dir);
		os_mkdirs(dir);
		os_quick_write_utf8_file(settings_path, settings_json, strlen(settings_json), false);
	}
	ui_thread = pthread_self();
	if (!obs_startup("en-US", config_dir, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return false;
	}
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC, .gpu_conversion = true,
	};
	obs_set_ui_task_handler(ui_task_handler);
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS || !obs_reset_audio(&ai)) {
		fprintf(stderr, "obs_reset_video/audio failed\n");
		return false;
	}
	for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++)
		obs_register_source(&kinds[i]);
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", FADERS_PLUGIN_BIN);
		return false;
	}
	obs_enum_all_sources(find_settings, &settings_source);
	return settings_source != NULL;
}

static int finish(void)
{
	pump_ui();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	fflush(stdout);
	// Like the other hosts, skip teardown races: exit straight away with the result.
	_exit(fails ? 1 : 0);
}

static obs_source_t *make(const char *kind, const char *name)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", name);
	obs_source_t *src = obs_source_create(kind, name, s, NULL);
	obs_data_release(s);
	obs_source_set_audio_mixers(src, 1);
	return src;
}
static void drop(obs_source_t *s)
{
	if (!s)
		return;
	obs_source_remove(s);
	obs_source_release(s);
}

// Put a source on a fader the way the plugin stores it ("" = Everything else).
static void place(obs_source_t *s, const char *fader)
{
	obs_data_t *priv = obs_source_get_private_settings(s);
	obs_data_set_string(priv, "faders_fader", fader);
	obs_data_release(priv);
}
// The stored placement: a fader name, "" (placed in Everything else) or "(new)" (none saved).
static const char *saved(obs_source_t *s)
{
	static char bufs[8][96];
	static int next;
	char *out = bufs[next++ % 8];
	obs_data_t *priv = obs_source_get_private_settings(s);
	snprintf(out, sizeof(bufs[0]), "%s", obs_data_has_user_value(priv, "faders_fader") ? obs_data_get_string(priv, "faders_fader") : "(new)");
	obs_data_release(priv);
	return out;
}

// faders_place: a placement value ("f:<fader>", "x:rest", "x:new", "x:keep"); whether it was
// taken, and why not.
static char place_error[512];
static bool place_value(obs_source_t *s, const char *value)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "source", obs_source_get_name(s));
	calldata_set_string(&cd, "value", value);
	place_error[0] = 0;
	bool ok = call("faders_place", &cd) && calldata_bool(&cd, "ok");
	const char *e = calldata_string(&cd, "error");
	snprintf(place_error, sizeof(place_error), "%s", e ? e : "");
	calldata_free(&cd);
	return ok;
}
// faders_placement: "<saved>|<effective>|<state>", e.g. "new||new", "Voice|Voice|ok",
// "Game||deleted", "Alerts||locked", "rest||rest".
static const char *placement(obs_source_t *s)
{
	static char bufs[8][256];
	static int next;
	char *out = bufs[next++ % 8];
	calldata_t cd = {0};
	calldata_set_string(&cd, "source", obs_source_get_name(s));
	if (call("faders_placement", &cd))
		snprintf(out, sizeof(bufs[0]), "%s|%s|%s", calldata_string(&cd, "saved"), calldata_string(&cd, "effective"), calldata_string(&cd, "state"));
	else
		snprintf(out, sizeof(bufs[0]), "(no faders_placement)");
	calldata_free(&cd);
	return out;
}

static void frontend_event(enum obs_frontend_event event)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "event", event);
	call("faders_frontend_event", &cd);
	calldata_free(&cd);
}
static long count_proc(const char *proc)
{
	calldata_t cd = {0};
	long n = call(proc, &cd) ? (long)calldata_int(&cd, "n") : -1;
	calldata_free(&cd);
	return n;
}

// ── The dialog, as the window builds it (faders_dialog) ──────────────────────────────────
// key<TAB>label<TAB>shown<TAB>choices per dropdown, key<TAB>text per info row.
static char *rows;
static const char *dialog(void)
{
	calldata_t cd = {0};
	call("faders_dialog", &cd);
	bfree(rows);
	rows = bstrdup(calldata_string(&cd, "rows") ? calldata_string(&cd, "rows") : "");
	calldata_free(&cd);
	return rows;
}
static long redraws_inside_update;
static bool pick(const char *key, const char *choice)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "key", key);
	calldata_set_string(&cd, "choice", choice);
	call("faders_dialog_set", &cd);
	bool ok = calldata_bool(&cd, "ok");
	redraws_inside_update += (long)calldata_int(&cd, "redraws_during_update");
	calldata_free(&cd);
	return ok;
}
// A click on a dropdown the window drew earlier: the value goes to the plugin as it was drawn,
// whatever has changed since (the window isn't redrawn yet).
static void stale_click(const char *key, const char *value)
{
	obs_data_t *change = obs_data_create();
	obs_data_set_string(change, key, value);
	obs_source_update(settings_source, change);
	obs_data_release(change);
}
static bool field(const char *line, const char *end, int i, char *out, size_t size)
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
// The sources shown in one fader's block, "|"-joined (rows after the first have the label " ").
static const char *block(const char *fader)
{
	static struct dstr out;
	dstr_copy(&out, "");
	bool in = false;
	for (const char *line = rows; line && *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char key[64], label[256], shown[256], choices[8];
		bool row = field(line, end, 3, choices, sizeof(choices));
		field(line, end, 0, key, sizeof(key));
		field(line, end, 1, label, sizeof(label));
		field(line, end, 2, shown, sizeof(shown));
		line = end + 1;
		if (!row)
			continue;
		if (strcmp(label, " ") != 0)
			in = strncmp(label, fader, strlen(fader)) == 0 && (label[strlen(fader)] == 0 || label[strlen(fader)] == ' ');
		if (in && strncmp(key, "row:", 4) == 0)
			dstr_catf(&out, "%s%s", out.len ? "|" : "", shown);
	}
	return out.array ? out.array : "";
}
static bool in_block(const char *fader, const char *name)
{
	struct dstr have = {0}, want = {0};
	dstr_printf(&have, "|%s|", block(fader));
	dstr_printf(&want, "|%s|", name);
	bool ok = strstr(have.array, want.array) != NULL;
	dstr_free(&have);
	dstr_free(&want);
	return ok;
}
// Number of source rows under any fader.
static int fader_rows(void)
{
	int n = 0;
	for (const char *p = rows; p && (p = strstr(p, "\nrow:")); p++)
		n++;
	return n + (rows && strncmp(rows, "row:", 4) == 0);
}
// One info line (by key), or "".
static const char *info(const char *key)
{
	static struct dstr bufs[4];
	static int next;
	struct dstr *o = &bufs[next++ % 4];
#define out (*o)
	dstr_copy(&out, "");
	char want[64];
	snprintf(want, sizeof(want), "%s\t", key);
	for (const char *line = rows; line && *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		if (strncmp(line, want, strlen(want)) == 0)
			dstr_ncat(&out, line + strlen(want), (size_t)(end - line) - strlen(want));
		line = end + 1;
	}
	return out.array ? out.array : "";
#undef out
}
// The dropdown line with this key, or "".
static const char *dropdown(const char *key)
{
	return info(key);
}
// The key of the dropdown currently showing `shown` (from the last dialog()), or "".
static const char *key_showing(const char *shown)
{
	static char key[64];
	key[0] = 0;
	for (const char *line = rows; line && *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char s[256];
		if (field(line, end, 3, s, sizeof(s)) && field(line, end, 2, s, sizeof(s)) && strcmp(s, shown) == 0) {
			field(line, end, 0, key, sizeof(key));
			return key;
		}
		line = end + 1;
	}
	return key;
}
// The key of the Add row of fader `fader` (its block's "add:" dropdown), or "".
static const char *add_key(const char *fader)
{
	static char keys[4][64];
	static int next;
	char *key = keys[next++ % 4];
	key[0] = 0;
	bool in = false;
	for (const char *line = rows; line && *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		char k[64], label[256], choices[8];
		bool row = field(line, end, 3, choices, sizeof(choices));
		field(line, end, 0, k, sizeof(k));
		field(line, end, 1, label, sizeof(label));
		line = end + 1;
		if (!row)
			continue;
		if (strcmp(label, " ") != 0)
			in = strncmp(label, fader, strlen(fader)) == 0 && (label[strlen(fader)] == 0 || label[strlen(fader)] == ' ');
		if (in && strncmp(k, "add:", 4) == 0) {
			snprintf(key, sizeof(keys[0]), "%s", k);
			return key;
		}
	}
	return key;
}
// Whether the Everything else line names `text` as an item (followed by ",", "." or " (").
static bool rest_has(const char *text)
{
	const char *line = info("rest_info");
	size_t n = strlen(text);
	for (const char *p = line; (p = strstr(p, text)); p++) {
		bool starts = p == line || p[-1] == ' ';
		char after = p[n];
		if (starts && (after == ',' || after == '.' || (after == ' ' && p[n + 1] == '(')))
			return true;
	}
	return false;
}

// The mixer's view (faders_mixer_stats): the fader it plays `name` on, "" = Everything else.
static char *stats;
static void fetch_stats(void)
{
	calldata_t cd = {0};
	call("faders_rebuild", &cd);
	calldata_free(&cd);
	calldata_t sd = {0};
	call("faders_mixer_stats", &sd);
	bfree(stats);
	stats = bstrdup(calldata_string(&sd, "json") ? calldata_string(&sd, "json") : "{}");
	calldata_free(&sd);
}
static const char *mixer_fader_of(const char *name)
{
	static char out[128];
	char key[512];
	snprintf(key, sizeof(key), "{\"name\":\"%s\",\"fader\":\"", name);
	const char *p = stats ? strstr(stats, key) : NULL;
	if (!p)
		return "(not listed)";
	p += strlen(key);
	snprintf(out, sizeof(out), "%.*s", (int)strcspn(p, "\""), p);
	return out;
}

// ── Scenarios ───────────────────────────────────────────────────────────────────────────

static void scenario_model(void)
{
	configure("Voice,Game,Music", true);
	// Kinds the old rule table guessed for: a mic (Voice), Spotify (Music), Discord (Voice), an
	// alert box (Alerts), a game capture (Game), desktop audio (Game).
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_data_t *w = obs_data_create();
	obs_data_set_string(w, "window", "Spotify Premium:Chrome_WidgetWin_0:Spotify.exe");
	obs_source_update(spotify, w);
	obs_data_release(w);
	obs_source_t *discord = make("wasapi_process_output_capture", "Discord");
	obs_source_t *alerts = make("browser_source", "Stream alerts");
	obs_source_t *game = make("game_capture", "Game Capture");
	obs_source_t *desktop = make("wasapi_output_capture", "Desktop Audio");
	obs_source_t *all[] = {mic, spotify, discord, alerts, game, desktop};
	const int n = 6;

	dialog();
	fetch_stats();
	struct dstr why = {0};
	bool ok = true;
	for (int i = 0; i < n; i++) {
		const char *name = obs_source_get_name(all[i]);
		struct dstr item = {0};
		dstr_printf(&item, "%s (new)", name);
		bool one = strcmp(saved(all[i]), "(new)") == 0 && rest_has(item.array) && strcmp(mixer_fader_of(name), "") == 0 && strcmp(placement(all[i]), "new||new") == 0;
		if (!one)
			dstr_catf(&why, "[\"%s\": saved %s, placement %s, listed as NEW in Everything else %d, mixer \"%s\"] ", name, saved(all[i]), placement(all[i]), rest_has(item.array), mixer_fader_of(name));
		ok = ok && one;
		dstr_free(&item);
	}
	result(ok && fader_rows() == 0, "model 1 unplaced sources are NEW and play in Everything else", "%s%d source row(s) on faders (want 0); Everything else: %s", why.array ? why.array : "", fader_rows(), info("rest_info"));
	dstr_free(&why);

	// Faders start empty, but keep their names.
	result(strlen(block("Voice")) == 0 && strlen(block("Game")) == 0 && strlen(block("Music")) == 0 && *add_key("Voice") && *add_key("Game") && *add_key("Music"),
	       "model 2 Voice, Game and Music start empty", "Voice [%s] Game [%s] Music [%s]; Add rows %s %s %s", block("Voice"), block("Game"), block("Music"), add_key("Voice"), add_key("Game"), add_key("Music"));
	result(strstr(info("new_info"), "6 sources aren't placed yet") != NULL && !strstr(rows, "Auto") && !strstr(rows, "auto") && !strstr(rows, "OBS Track"),
	       "model 3 the window counts NEW sources and offers no tracks", "new_info \"%s\"", info("new_info"));

	// One step: pick it in a fader's Add list.
	redraws_inside_update = 0;
	bool picked = pick(add_key("Voice"), "Mic");
	pump_for(300);
	dialog();
	fetch_stats();
	bool on_voice = picked && strcmp(saved(mic), "Voice") == 0 && in_block("Voice", "Mic") && !rest_has("Mic (new)") && strcmp(mixer_fader_of("Mic"), "Voice") == 0 &&
			strcmp(placement(mic), "Voice|Voice|ok") == 0 && strstr(info("new_info"), "5 sources aren't placed yet");
	result(on_voice && redraws_inside_update == 0, "model 4 a pick in Voice's Add list moves Mic there", "picked %d, saved \"%s\", placement %s, Voice [%s], mixer \"%s\", redraws during the update %ld (want 0), new_info \"%s\"", picked,
	       saved(mic), placement(mic), block("Voice"), mixer_fader_of("Mic"), redraws_inside_update, info("new_info"));
	// Taking it off puts it in Everything else, placed there (no longer NEW).
	bool removed = pick(key_showing("Mic"), "Remove from Voice");
	pump_for(300);
	dialog();
	result(removed && strcmp(saved(mic), "") == 0 && rest_has("Mic") && !rest_has("Mic (new)") && strcmp(placement(mic), "rest||rest") == 0,
	       "model 5 Remove puts it in Everything else, no longer NEW", "removed %d, saved \"%s\", placement %s, Everything else: %s", removed, saved(mic), placement(mic), info("rest_info"));

	// Fader names that look like the values the plugin uses internally are ordinary names.
	configure("~new,x:rest,Music", true);
	dialog();
	bool p1 = pick(add_key("~new"), "Spotify");
	dialog();
	bool p2 = place_value(discord, "f:x:rest");
	dialog();
	fetch_stats();
	result(p1 && p2 && strcmp(saved(spotify), "~new") == 0 && in_block("~new", "Spotify") && strcmp(mixer_fader_of("Spotify"), "~new") == 0 && strcmp(saved(discord), "x:rest") == 0 &&
		       in_block("x:rest", "Discord") && strcmp(mixer_fader_of("Discord"), "x:rest") == 0,
	       "model 6 faders named \"~new\" and \"x:rest\" are faders", "Spotify: picked %d saved \"%s\" mixer \"%s\" [%s]; Discord: placed %d (%s) saved \"%s\" mixer \"%s\" [%s]", p1, saved(spotify),
	       mixer_fader_of("Spotify"), block("~new"), p2, place_error, saved(discord), mixer_fader_of("Discord"), block("x:rest"));
	// The values themselves.
	bool v_rest = place_value(spotify, "x:rest") && strcmp(saved(spotify), "") == 0;
	bool v_new = place_value(spotify, "x:new") && strcmp(saved(spotify), "(new)") == 0;
	bool v_keep = place_value(discord, "x:keep") && strcmp(saved(discord), "x:rest") == 0;
	bool v_raw = !place_value(spotify, "~new") && strcmp(saved(spotify), "(new)") == 0;
	bool v_ee = !place_value(spotify, "f:Everything else") && strcmp(saved(spotify), "(new)") == 0;
	bool v_empty = !place_value(spotify, "f:") && !place_value(spotify, "") && strcmp(saved(spotify), "(new)") == 0;
	result(v_rest && v_new && v_keep && v_raw && v_ee && v_empty, "model 7 placement values: x:rest, x:new, x:keep; bad ones refused",
	       "x:rest %d, x:new %d, x:keep %d, bare \"~new\" refused %d, f:Everything else refused %d, empty refused %d", v_rest, v_new, v_keep, v_raw, v_ee, v_empty);

	for (int i = 0; i < n; i++)
		drop(all[i]);
}

static void scenario_legacy(void)
{
	// 0.4.0 settings: Track 2 → Voice, Track 3 → Music, Track 4 → Everything else.
	start("legacy",
	      "{\"enabled\":true,\"server\":\"ws://127.0.0.1:9/ingest/placement\",\"key\":\"fdr_test_placementFAKEKEY\",\"only_live\":false,"
	      "\"faders\":[{\"value\":\"Voice\"},{\"value\":\"Game\"},{\"value\":\"Music\"}],"
	      "\"track:2\":\"Voice\",\"track:3\":\"Music\",\"track:4\":\"~rest\",\"track:5\":\"~none\"}");
	obs_data_t *s = obs_source_get_settings(settings_source);
	bool erased_on_load = true;
	for (int t = 1; t <= 6; t++) {
		char key[16];
		snprintf(key, sizeof(key), "track:%d", t);
		erased_on_load = erased_on_load && !obs_data_has_user_value(s, key) && !obs_data_has_default_value(s, key);
	}
	obs_data_release(s);
	// A source ticked for every track (OBS ticks all six for a new source) and one ticked for 3.
	obs_source_t *desktop = make("wasapi_output_capture", "Desktop Audio");
	obs_source_set_audio_mixers(desktop, 0x3f);
	obs_source_t *media = make("ffmpeg_source", "Intro video");
	obs_source_set_audio_mixers(media, 1 | 4);
	configure("Voice,Game,Music", true);
	dialog();
	fetch_stats();
	result(strcmp(mixer_fader_of("Desktop Audio"), "") == 0 && strcmp(mixer_fader_of("Intro video"), "") == 0 && rest_has("Desktop Audio (new)") && rest_has("Intro video (new)") && fader_rows() == 0 &&
		       !strstr(rows, "OBS Track"),
	       "legacy 1 old track settings place nothing", "mixer: Desktop Audio \"%s\", Intro video \"%s\"; %d row(s) on faders; Everything else: %s", mixer_fader_of("Desktop Audio"), mixer_fader_of("Intro video"),
	       fader_rows(), info("rest_info"));
	const char *file = settings_file();
	result(erased_on_load && !strstr(file, "track:"), "legacy 2 old track:N keys are erased", "erased in memory on load %d; settings.json %s", erased_on_load, strstr(file, "track:") ? "still has track:N" : "has none");
	// The procedure that set them is gone.
	calldata_t cd = {0};
	calldata_set_int(&cd, "track", 3);
	calldata_set_string(&cd, "fader", "Music");
	bool has_proc = proc_handler_call(obs_get_proc_handler(), "faders_assign_track", &cd);
	calldata_free(&cd);
	result(!has_proc, "legacy 3 faders_assign_track is gone", "%s", has_proc ? "still registered" : "not registered");
	drop(desktop);
	drop(media);
}

static void scenario_duplicate(void)
{
	configure("Voice,Game,Music", true);
	obs_source_t *clip = make("ffmpeg_source", "Clip");
	place(clip, "Music");
	// Edit → Copy, then Paste (Duplicate): OBS copies the private settings after creating it.
	obs_source_t *copy = obs_source_duplicate(clip, "Clip 2", false);
	dialog();
	fetch_stats();
	result(copy && copy != clip && strcmp(saved(copy), "Music") == 0 && in_block("Music", "Clip 2") && strcmp(mixer_fader_of("Clip 2"), "Music") == 0,
	       "duplicate 1 a duplicated source keeps the original's fader", "copy saved \"%s\", Music [%s], mixer \"%s\"", copy ? saved(copy) : "-", block("Music"), copy ? mixer_fader_of("Clip 2") : "-");
	// Paste (Reference) of a device capture is the same source.
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	place(mic, "Voice");
	obs_source_t *ref = obs_source_duplicate(mic, "Mic 2", false);
	result(ref == mic && strcmp(saved(mic), "Voice") == 0, "duplicate 2 a device capture pastes as a reference", "same source %d, saved \"%s\"", ref == mic, saved(mic));
	obs_source_release(ref);

	// Saved and loaded with the scene collection: the placement comes back.
	obs_data_t *data = obs_save_source(clip);
	drop(clip);
	obs_wait_for_destroy_queue();
	obs_source_t *loaded = obs_load_source(data);
	obs_data_release(data);
	// Imported from another scene collection: a source placed there keeps it, one never placed
	// there is NEW here too (whatever its kind).
	obs_data_t *imp = obs_data_create_from_json("{\"id\":\"ffmpeg_source\",\"name\":\"Imported placed\",\"settings\":{},\"private_settings\":{\"faders_fader\":\"Game\"}}");
	obs_source_t *imported = obs_load_source(imp);
	obs_data_release(imp);
	obs_data_t *imp2 = obs_data_create_from_json("{\"id\":\"ffmpeg_source\",\"name\":\"Imported new\",\"settings\":{}}");
	obs_source_t *imported_new = obs_load_source(imp2);
	obs_data_release(imp2);
	// A new source that only shares a name with a placed one is NEW.
	obs_source_t *again = make("ffmpeg_source", "Clip 3");
	dialog();
	fetch_stats();
	result(loaded && strcmp(saved(loaded), "Music") == 0 && in_block("Music", "Clip") && imported && strcmp(saved(imported), "Game") == 0 && in_block("Game", "Imported placed") && imported_new &&
		       strcmp(saved(imported_new), "(new)") == 0 && rest_has("Imported new (new)") && strcmp(mixer_fader_of("Imported new"), "") == 0 && strcmp(saved(again), "(new)") == 0 && rest_has("Clip 3 (new)"),
	       "duplicate 3 load keeps it; an import is NEW unless placed there", "loaded \"%s\"; imported placed \"%s\"; imported unplaced \"%s\" (mixer \"%s\"); new source \"%s\"; Everything else: %s", loaded ? saved(loaded) : "-",
	       imported ? saved(imported) : "-", imported_new ? saved(imported_new) : "-", mixer_fader_of("Imported new"), saved(again), info("rest_info"));
	drop(loaded);
	drop(imported);
	drop(imported_new);
	drop(again);
	drop(copy);
	drop(mic);
}

static void scenario_validate(void)
{
	// The key's plan (remembered from its last connection): 2 faders. Music and Alerts are locked.
	start("validate",
	      "{\"enabled\":true,\"server\":\"ws://127.0.0.1:9/ingest/placement\",\"key\":\"fdr_test_placementFAKEKEY\",\"only_live\":true,"
	      "\"account\":\"some_channel\",\"account_key\":\"AKEKEY\",\"account_plan\":\"free\",\"account_faders\":2,"
	      "\"faders\":[{\"value\":\"Voice\"},{\"value\":\"Game\"},{\"value\":\"Music\"},{\"value\":\"Alerts\"}]}");
	configure("Voice,Game,Music,Alerts", false);
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *bell = make("browser_source", "Bell");
	place(bell, "Alerts"); // placed before the plan shrank: kept, plays in Everything else
	dialog();

	// Locked: past the plan's 2 faders.
	bool by_value = place_value(spotify, "f:Alerts");
	char err_locked[512];
	snprintf(err_locked, sizeof(err_locked), "%s", place_error);
	const char *add_alerts = dropdown(add_key("Alerts"));
	bool offered = strstr(add_alerts, "Spotify") != NULL;
	bool by_pick = *add_key("Alerts") && pick(add_key("Alerts"), "Spotify");
	result(!by_value && *err_locked && !offered && !by_pick && strcmp(saved(spotify), "(new)") == 0 && strcmp(placement(bell), "Alerts||locked") == 0,
	       "validate 1 a locked fader takes nothing new", "f:Alerts taken %d (\"%s\"); Alerts' Add list offers Spotify %d, pick taken %d; Spotify saved \"%s\"; Bell %s", by_value, err_locked, offered, by_pick,
	       saved(spotify), placement(bell));
	// A click on Alerts' Add list as it was drawn before it locked (or a stale window).
	dialog();
	char key_alerts[64];
	snprintf(key_alerts, sizeof(key_alerts), "%s", add_key("Alerts"));
	struct dstr v = {0};
	dstr_printf(&v, "s:%s", obs_source_get_uuid(spotify));
	if (*key_alerts)
		stale_click(key_alerts, v.array);
	pump_for(300);
	dialog();
	result(strcmp(saved(spotify), "(new)") == 0 && strstr(info("move_feedback"), "Spotify") && strstr(info("move_feedback"), "Alerts"),
	       "validate 2 a click on a locked fader's list is refused, and said so", "Spotify saved \"%s\"; feedback \"%s\"", saved(spotify), info("move_feedback"));

	// Deleted: Game goes. What was on it keeps its saved fader but plays in Everything else.
	place_value(spotify, "f:Game");
	bool was_on_game = strcmp(saved(spotify), "Game") == 0;
	dialog(); // the window as drawn with Game still there
	char key_game[64];
	snprintf(key_game, sizeof(key_game), "%s", add_key("Game"));
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	configure("Voice,Music,Alerts", false);
	bool deleted_state = strcmp(placement(spotify), "Game||deleted") == 0;
	bool refused = !place_value(mic, "f:Game") && strcmp(saved(mic), "(new)") == 0;
	char err_missing[512];
	snprintf(err_missing, sizeof(err_missing), "%s", place_error);
	// Stale: the old window's Game row is clicked after Game went (the redraw hasn't come yet).
	struct dstr mv = {0};
	dstr_printf(&mv, "s:%s", obs_source_get_uuid(mic));
	if (*key_game)
		stale_click(key_game, mv.array);
	bool stale_refused = strcmp(saved(mic), "(new)") == 0;
	pump_for(300);
	dialog();
	result(was_on_game && deleted_state && refused && *err_missing && stale_refused && rest_has("Spotify") && strstr(info("rest_info"), "Game") && strstr(info("move_feedback"), "Mic"),
	       "validate 3 deleted and stale faders are refused; saved name kept", "Spotify on Game %d, then %s; f:Game for Mic refused %d (\"%s\"); stale Game click refused %d (Mic \"%s\"); Everything else: %s; feedback \"%s\"",
	       was_on_game, placement(spotify), refused, err_missing, stale_refused, saved(mic), info("rest_info"), info("move_feedback"));

	// A source deleted while the window still lists it.
	obs_source_t *gone = make("ffmpeg_source", "Gone clip");
	dialog();
	struct dstr gv = {0};
	dstr_printf(&gv, "s:%s", obs_source_get_uuid(gone));
	char key_voice[64];
	snprintf(key_voice, sizeof(key_voice), "%s", add_key("Voice"));
	drop(gone);
	obs_wait_for_destroy_queue();
	if (*key_voice)
		stale_click(key_voice, gv.array);
	pump_for(300);
	dialog();
	result(*key_voice && strlen(block("Voice")) == 0 && strstr(info("move_feedback"), "no longer"), "validate 4 a deleted source is refused, and said so", "Voice [%s]; feedback \"%s\"", block("Voice"),
	       info("move_feedback"));

	// Ten picks in a row: the last one counts.
	configure("Voice,Game,Music,Alerts", false);
	const char *order[] = {"Voice", "Game", "Voice", "Game", "Voice", "Game", "Voice", "Game", "Voice", "Game"};
	for (int i = 0; i < 10; i++) {
		dialog();
		const char *k = key_showing("Mic");
		if (*k)
			pick(k, "Remove from");
		dialog();
		pick(add_key(order[i]), "Mic");
	}
	dialog();
	result(strcmp(saved(mic), "Game") == 0 && in_block("Game", "Mic") && !in_block("Voice", "Mic"), "validate 5 ten moves in a row: the last one counts", "saved \"%s\"; Voice [%s] Game [%s]", saved(mic), block("Voice"),
	       block("Game"));
	dstr_free(&v);
	dstr_free(&mv);
	dstr_free(&gv);
	drop(spotify);
	drop(bell);
	drop(mic);
}

static void scenario_capacity(void)
{
	configure("Voice,Game,Music", false);
	static obs_source_t *src[257];
	int made = 0;
	const int sizes[] = {100, 128, 256, 257};
	const char *faders[] = {"Voice", "Game", "Music"};
	for (int k = 0; k < 4; k++) {
		int n = sizes[k];
		for (; made < n; made++) {
			char name[32];
			snprintf(name, sizeof(name), "Input %03d", made + 1);
			src[made] = make("wasapi_input_capture", name);
		}
		// All NEW.
		for (int i = 0; i < n; i++) {
			obs_data_t *priv = obs_source_get_private_settings(src[i]);
			obs_data_erase(priv, "faders_fader");
			obs_data_release(priv);
		}
		dialog();
		int listed_new = 0;
		for (int i = 0; i < n && i < 256; i++) {
			struct dstr item = {0};
			dstr_printf(&item, "%s (new)", obs_source_get_name(src[i]));
			listed_new += rest_has(item.array);
			dstr_free(&item);
		}
		bool past = n <= 256 || (rest_has(obs_source_get_name(src[256])) && strstr(info("rest_info"), "1 source past"));
		char count[64];
		snprintf(count, sizeof(count), "%d sources aren't placed yet", n < 256 ? n : 256);
		char name[64];
		snprintf(name, sizeof(name), "capacity %d all NEW", n);
		result(listed_new == (n < 256 ? n : 256) && past && fader_rows() == 0 && strstr(info("new_info"), count), name, "%d listed as NEW (want %d), past the limit named %d, %d row(s) on faders; new_info \"%s\"", listed_new,
		       n < 256 ? n : 256, past, fader_rows(), info("new_info"));
		// All placed, round robin.
		for (int i = 0; i < n; i++)
			place(src[i], faders[i % 3]);
		dialog();
		int rows_ok = 0;
		for (int i = 0; i < n && i < 256; i++)
			rows_ok += in_block(faders[i % 3], obs_source_get_name(src[i]));
		past = n <= 256 || rest_has(obs_source_get_name(src[256]));
		snprintf(name, sizeof(name), "capacity %d all placed", n);
		result(rows_ok == (n < 256 ? n : 256) && past && !*info("new_info"), name, "%d on their fader's rows (want %d), past the limit named in Everything else %d; Everything else: %.200s", rows_ok, n < 256 ? n : 256,
		       past, info("rest_info"));
	}
	for (int i = 0; i < made; i++)
		drop(src[i]);
}

// Mr Sharkey's second panel (#20): his sources, then many browser sources, then the virtual cable
// "6. C - VC - MUSIC" created last. Every audio input must be on some list.
static void scenario_sharkey2(void)
{
	configure("Voice,Game,Music", false);
	obs_source_t *his[16];
	int n = 0;
	his[n++] = make("wasapi_input_capture", "Lav Mic");
	his[n++] = make("wasapi_input_capture", "1.2 Traveler");
	his[n++] = make("wasapi_output_capture", "1.5 Audio");
	his[n++] = make("game_capture", "6. Game Capture");
	his[n++] = make("wasapi_process_output_capture", "9. Spotify");
	his[n++] = make("ffmpeg_source", "Music-Lofi");
	place(his[0], "Voice");
	place(his[1], "Voice");
	place(his[3], "Game");
	place(his[4], "Music");
	static obs_source_t *fill[300];
	int nf = 0;
	// The cable as input 129 (past the old 128 rows), then as input 257 (past the mixer).
	const int positions[] = {129, 257};
	for (int k = 0; k < 2; k++) {
		for (; n + nf < positions[k] - 1; nf++) {
			char name[32];
			snprintf(name, sizeof(name), "Overlay %d", nf + 1);
			fill[nf] = make("browser_source", name);
		}
		obs_source_t *c = make("wasapi_input_capture", "6. C - VC - MUSIC");
		place(c, "Music");
		dialog();
		bool listed = k == 0 ? in_block("Music", "6. C - VC - MUSIC") : rest_has("6. C - VC - MUSIC");
		bool his_ok = in_block("Voice", "Lav Mic") && in_block("Voice", "1.2 Traveler") && in_block("Game", "6. Game Capture") && in_block("Music", "9. Spotify") && rest_has("1.5 Audio (new)") &&
			      rest_has("Music-Lofi (new)");
		char name[64];
		snprintf(name, sizeof(name), "sharkey2 the cable as audio input %d is listed", positions[k]);
		result(listed && his_ok, name, "cable %s; his sources %s; Voice [%s] Music [%.120s]", listed ? (k == 0 ? "under Music" : "named in Everything else") : "ON NO LIST", his_ok ? "where he put them" : "WRONG",
		       block("Voice"), block("Music"));
		drop(c);
	}
	for (int i = 0; i < nf; i++)
		drop(fill[i]);
	for (int i = 0; i < n; i++)
		drop(his[i]);
}

static long redraws(void)
{
	return count_proc("faders_dialog_redraws");
}

static void scenario_refresh(void)
{
	configure("Voice,Game,Music", false);
	pump_for(600);
	long r0 = redraws();
	obs_source_t *a = make("wasapi_input_capture", "New mic");
	pump_for(700);
	long r_create = redraws() - r0;
	long r1 = redraws();
	obs_source_set_name(a, "Renamed mic");
	pump_for(700);
	long r_rename = redraws() - r1;
	long r2 = redraws();
	drop(a);
	obs_wait_for_destroy_queue();
	pump_for(700);
	long r_remove = redraws() - r2;
	// Not an audio input: no redraw.
	long r3 = redraws();
	obs_scene_t *scene = obs_scene_create("Just a scene");
	pump_for(700);
	long r_scene = redraws() - r3;
	result(r_create >= 1 && r_rename >= 1 && r_remove >= 1 && r_scene == 0, "refresh 1 audio inputs created, renamed, removed redraw the window", "redraws: create %ld, rename %ld, remove %ld (want ≥1 each); a scene %ld (want 0)",
	       r_create, r_rename, r_remove, r_scene);

	// 50 at once: a redraw at most every 250 ms, not 50.
	static obs_source_t *many[50];
	long r4 = redraws();
	uint64_t t0 = os_gettime_ns();
	for (int i = 0; i < 50; i++) {
		char name[32];
		snprintf(name, sizeof(name), "Burst %d", i);
		many[i] = make("browser_source", name);
	}
	pump_for(800);
	double secs = (double)(os_gettime_ns() - t0) / 1e9;
	long r_burst = redraws() - r4;
	long allowed = 1 + (long)(secs / 0.25);
	result(r_burst >= 1 && r_burst <= allowed && r_burst < 10, "refresh 2 50 new inputs are coalesced", "%ld redraw(s) over %.2f s (want 1..%ld)", r_burst, secs, allowed);

	// A scene collection switch: nothing while it loads, then one redraw.
	long r5 = redraws();
	frontend_event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);
	for (int i = 0; i < 50; i++)
		drop(many[i]);
	obs_wait_for_destroy_queue();
	obs_source_t *loaded[20];
	for (int i = 0; i < 20; i++) {
		char name[32];
		snprintf(name, sizeof(name), "Loaded %d", i);
		loaded[i] = make("ffmpeg_source", name);
	}
	pump_for(700);
	long r_during = redraws() - r5;
	frontend_event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
	pump_for(700);
	long r_after = redraws() - r5 - r_during;
	result(r_during == 0 && r_after >= 1 && r_after <= 2, "refresh 3 a collection switch redraws once, after it", "during the switch %ld (want 0), after %ld (want 1-2)", r_during, r_after);
	for (int i = 0; i < 20; i++)
		drop(loaded[i]);
	obs_scene_release(scene);
}

static void scenario_cancel(void)
{
	configure("Voice,Game,Music", false);
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	obs_source_t *other = make("wasapi_input_capture", "Discord cable");
	obs_source_t *music = make("wasapi_input_capture", "Music (VB-Cable)");
	obs_source_t *loose = make("ffmpeg_source", "Loose clip");
	place(mic, "Voice");
	place(music, "Voice");
	place(other, "Game");

	dialog(); // the window as last drawn
	obs_data_t *at_open = obs_data_create();
	obs_data_t *cur = obs_source_get_settings(settings_source);
	obs_data_apply(at_open, cur); // OBS's copy, taken as the window opens
	dialog();
	// Take the mic off Voice (the cable's row moves up into its place), and put the loose clip on Game.
	bool removed = pick(key_showing("Mic"), "Remove from Voice");
	dialog();
	bool added = pick(add_key("Game"), "Loose clip");
	dialog();
	// Cancel (and Discard, and Edit → Undo): clear, then update with the copy.
	obs_data_clear(cur);
	obs_source_update(settings_source, at_open);
	obs_data_release(cur);
	obs_data_release(at_open);
	pump_for(300);
	dialog();
	result(removed && added && strcmp(saved(music), "Voice") == 0 && strcmp(saved(other), "Game") == 0 && strcmp(saved(mic), "") == 0 && strcmp(saved(loose), "Game") == 0 && in_block("Voice", "Music (VB-Cable)") &&
		       in_block("Game", "Discord cable") && in_block("Game", "Loose clip"),
	       "cancel 1 Cancel after moves moves nothing", "stored after Cancel: Mic \"%s\" (want \"\"), Music \"%s\", Discord cable \"%s\", Loose clip \"%s\" (want Game); Voice [%s] Game [%s]", saved(mic), saved(music),
	       saved(other), saved(loose), block("Voice"), block("Game"));
	const char *file = settings_file();
	result(!strstr(file, "\"row:") && !strstr(file, "\"add:"), "cancel 2 the window's dropdowns never reach settings.json", "%s", strstr(file, "\"row:") || strstr(file, "\"add:") ? file : "none there");
	drop(mic);
	drop(other);
	drop(music);
	drop(loose);
}

static void scenario_firstrun(bool placed_already)
{
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	obs_source_t *game = make("game_capture", "Game Capture");
	if (placed_already)
		place(mic, "Voice");
	long before = count_proc("faders_dialog_opens");
	frontend_event(OBS_FRONTEND_EVENT_FINISHED_LOADING);
	pump_for(300);
	long first = count_proc("faders_dialog_opens") - before;
	frontend_event(OBS_FRONTEND_EVENT_FINISHED_LOADING); // the next start
	pump_for(300);
	long second = count_proc("faders_dialog_opens") - before - first;
	const char *file = settings_file();
	if (!placed_already)
		result(before == 0 && first == 1 && second == 0, "firstrun 1 opens once on the first start, nothing placed", "opened %ld then %ld time(s) (want 1, 0); settings.json %s", first, second,
		       strstr(file, "first_run") ? "remembers it" : "doesn't remember it");
	else
		result(before == 0 && first == 0 && second == 0, "firstrun 2 doesn't open when something is placed", "opened %ld then %ld time(s) (want 0, 0)", first, second);
	drop(mic);
	drop(game);
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	bool own_start = !strcmp(which, "legacy") || !strcmp(which, "validate");
	if (!own_start && !start(which, NULL))
		return 1;
	if (!strcmp(which, "model"))
		scenario_model();
	else if (!strcmp(which, "legacy"))
		scenario_legacy();
	else if (!strcmp(which, "duplicate"))
		scenario_duplicate();
	else if (!strcmp(which, "validate"))
		scenario_validate();
	else if (!strcmp(which, "capacity"))
		scenario_capacity();
	else if (!strcmp(which, "sharkey2"))
		scenario_sharkey2();
	else if (!strcmp(which, "refresh"))
		scenario_refresh();
	else if (!strcmp(which, "cancel"))
		scenario_cancel();
	else if (!strcmp(which, "firstrun"))
		scenario_firstrun(false);
	else if (!strcmp(which, "firstrun-placed"))
		scenario_firstrun(true);
	else {
		fprintf(stderr, "usage: %s model|legacy|duplicate|validate|capacity|sharkey2|refresh|cancel|firstrun|firstrun-placed\n", argv[0]);
		return 2;
	}
	return finish();
}
