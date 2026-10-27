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
// Faders OBS plugin: sends voice / game / music as separate layers so viewers can mix them.
//
// UI without Qt code: a private "Faders" source holds the settings, and Tools → Faders opens
// OBS's own properties window for it. Settings persist in the plugin's config folder.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include "uplink.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("faders", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Faders: let viewers set their own voice, game and music levels.";
}

#define SOURCE_ID "faders_uplink"
#define PORTAL_URL "https://faders.tv/portal/"
#define DOWNLOAD_URL "https://faders.tv/streamers/#download"
// Production hub. The key alone identifies the channel, so streamers only paste their key. The
// server isn't shown in the settings; tests point it elsewhere through faders_configure.
#define DEFAULT_SERVER "wss://hub.faders.tv/ingest"

static obs_source_t *settings_source; // private, never shown in a scene
static fdr_uplink_t *uplink;
static pthread_mutex_t uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static bool streaming;

static char *config_file(void)
{
	return obs_module_config_path("settings.json");
}

static void names_changed(void);
static void update_available(const char *version);

static void save_settings(obs_data_t *settings)
{
	char *path = config_file();
	obs_data_save_json_safe(settings, path, "tmp", "bak");
	bfree(path);
}

// ── Starting / stopping ─────────────────────────────────────────────────────────────────

static bool should_run(obs_data_t *s)
{
	if (!obs_data_get_bool(s, "enabled"))
		return false;
	if (!*obs_data_get_string(s, "server") || !*obs_data_get_string(s, "key"))
		return false;
	return streaming || !obs_data_get_bool(s, "only_live");
}

// The track (1-6) the stream's audio encoder mixes from; 0 when not streaming (or headless).
static int stream_audio_track(void)
{
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return 0;
	obs_encoder_t *enc = obs_output_get_audio_encoder(out, 0);
	int track = enc ? (int)obs_encoder_get_mixer_index(enc) + 1 : 0;
	obs_output_release(out);
	return track;
}

static void apply(obs_data_t *s)
{
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_stop(uplink);
	uplink = NULL;
	if (s && should_run(s)) {
		fdr_uplink_config cfg = {
			.server = obs_data_get_string(s, "server"),
			.key = obs_data_get_string(s, "key"),
			.names_changed = names_changed,
			.update_available = update_available,
			.tracks = {(int)obs_data_get_int(s, "track_voice"), (int)obs_data_get_int(s, "track_game"),
				   (int)obs_data_get_int(s, "track_music")},
		};
		uplink = fdr_uplink_start(&cfg);
		if (streaming)
			fdr_uplink_watch_stream_track(uplink, stream_audio_track());
	}
	pthread_mutex_unlock(&uplink_mu);
}

static void apply_current(void)
{
	obs_data_t *s = settings_source ? obs_source_get_settings(settings_source) : NULL;
	apply(s);
	obs_data_release(s);
}

static void get_status(fdr_uplink_status *st)
{
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_get_status(uplink, st);
	pthread_mutex_unlock(&uplink_mu);
}

// Runs on OBS's UI thread. Updating the labels must not apply/restart the audio uplink.
static void refresh_names(void *unused)
{
	UNUSED_PARAMETER(unused);
	if (!settings_source) return;
	fdr_uplink_status st;
	get_status(&st);
	if (!st.fader_names[0][0]) return;
	obs_data_t *settings = obs_source_get_settings(settings_source);
	const char *keys[] = {"name_voice", "name_game", "name_music"};
	for (int i = 0; i < 3; i++) obs_data_set_string(settings, keys[i], st.fader_names[i]);
	save_settings(settings);
	obs_data_release(settings);
	obs_source_update_properties(settings_source);
}

static void names_changed(void)
{
	obs_queue_task(OBS_TASK_UI, refresh_names, NULL, false);
}

// The hub only says a release is newer when the plugin connects, which by default is only while
// live. Keeping the version in the settings lets the notice outlive the stream: Tools → Faders
// shows it and OBS's next start opens that window once.
static void remember_update(void *param)
{
	char *version = param;
	if (settings_source) {
		obs_data_t *settings = obs_source_get_settings(settings_source);
		if (strcmp(obs_data_get_string(settings, "update_available"), version) != 0) {
			obs_data_set_string(settings, "update_available", version);
			save_settings(settings);
		}
		obs_data_release(settings);
		obs_source_update_properties(settings_source);
	}
	bfree(version);
}

static void update_available(const char *version)
{
	obs_queue_task(OBS_TASK_UI, remember_update, bstrdup(version), false);
}

// The saved release, or NULL once this build is that version or newer (the user has updated).
static const char *pending_update(obs_data_t *settings)
{
	const char *latest = settings ? obs_data_get_string(settings, "update_available") : "";
	int a[3], b[3];
	if (sscanf(latest, "%d.%d.%d", &a[0], &a[1], &a[2]) != 3 || sscanf(FDR_VERSION_STR, "%d.%d.%d", &b[0], &b[1], &b[2]) != 3)
		return NULL;
	for (int i = 0; i < 3; i++)
		if (a[i] != b[i])
			return a[i] > b[i] ? latest : NULL;
	return NULL;
}

static void status_text(struct dstr *out)
{
	fdr_uplink_status st;
	get_status(&st);
	if (!st.running)
		dstr_copy(out, streaming ? "Not running. Check your key." : "Idle. Starts when you go live.");
	else if (!st.connected)
		dstr_printf(out, "Connecting… %s", st.last_error[0] ? st.last_error : "");
	else if (st.stream_track_silent)
		dstr_printf(out, "Connected, but Track %d (what OBS streams) is silent: viewers hear nothing.", st.stream_track);
	else if (st.listeners == 0)
		dstr_copy(out, "Connected. Paused until a viewer turns Faders on.");
	else
		dstr_printf(out, "Connected. %d listener%s, sending %d kbps.", st.listeners, st.listeners == 1 ? "" : "s", st.uplink_kbps);
}

// ── Settings source (properties window) ─────────────────────────────────────────────────

static const char *src_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Faders";
}

static void *src_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void src_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static void src_defaults(obs_data_t *s)
{
	obs_data_set_default_bool(s, "enabled", true);
	obs_data_set_default_string(s, "server", DEFAULT_SERVER);
	obs_data_set_default_bool(s, "only_live", true);
	obs_data_set_default_string(s, "name_voice", "Voice");
	obs_data_set_default_string(s, "name_game", "Game");
	obs_data_set_default_string(s, "name_music", "Music");
	obs_data_set_default_int(s, "track_voice", 2);
	obs_data_set_default_int(s, "track_game", 3);
	obs_data_set_default_int(s, "track_music", 4);
}

static void add_track_list(obs_properties_t *props, const char *name, const char *label)
{
	obs_property_t *p = obs_properties_add_list(props, name, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int t = 1; t <= 6; t++) {
		char text[16];
		snprintf(text, sizeof(text), "Track %d", t);
		obs_property_list_add_int(p, text, t);
	}
}

static obs_properties_t *src_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	struct dstr status = {0};
	status_text(&status);
	obs_property_t *st = obs_properties_add_text(props, "status_info", status.array, OBS_TEXT_INFO);
	obs_property_text_set_info_type(st, OBS_TEXT_INFO_NORMAL);
	dstr_free(&status);

	fdr_uplink_status now_st;
	get_status(&now_st);
	if (now_st.stream_track_silent) {
		struct dstr warn = {0};
		dstr_printf(&warn,
			    "Twitch is getting no audio: Track %d, the one OBS streams, is silent. In Edit → Advanced "
			    "Audio Properties, tick Track %d for your mic, game and music as well as their Faders track. "
			    "<a href=\"https://faders.tv/docs/obs-tracks/\">How to set up tracks</a>",
			    now_st.stream_track, now_st.stream_track);
		obs_property_t *w = obs_properties_add_text(props, "stream_silent", warn.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_ERROR);
		obs_property_text_set_info_word_wrap(w, true);
		dstr_free(&warn);
	}

	// The hub says when a newer release is out; the link always goes to faders.tv.
	obs_data_t *settings = settings_source ? obs_source_get_settings(settings_source) : NULL;
	const char *update = pending_update(settings);
	if (update) {
		struct dstr label = {0};
		dstr_printf(&label, "Update available: Faders %s (you have %s). <a href=\"%s\">Download it</a>", update, FDR_VERSION_STR, DOWNLOAD_URL);
		obs_property_t *upd = obs_properties_add_text(props, "update", label.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(upd, OBS_TEXT_INFO_WARNING);
		dstr_free(&label);
	}

	obs_properties_add_bool(props, "enabled", "Send Faders layers");
	obs_properties_add_text(props, "key", "Key", OBS_TEXT_PASSWORD);
	// An info row renders as a QLabel with external links enabled: a real hyperlink that opens the
	// browser directly (a URL button asks "Open this URL?" first).
	obs_properties_add_text(props, "portal", "<a href=\"" PORTAL_URL "\">Open the Faders portal to get key</a>", OBS_TEXT_INFO);

	obs_properties_t *tracks = obs_properties_create();
	const char *track_keys[] = {"track_voice", "track_game", "track_music"};
	const char *name_keys[] = {"name_voice", "name_game", "name_music"};
	const char *defaults[] = {"Voice", "Game", "Music"};
	for (int i = 0; i < 3; i++) {
		const char *label = settings ? obs_data_get_string(settings, name_keys[i]) : "";
		// OBS renders property descriptions using QLabel's automatic rich-text detection.
		// Always wrap escaped text so names containing tags or ampersands remain literal.
		struct dstr escaped = {0}, description = {0};
		dstr_copy(&escaped, *label ? label : defaults[i]);
		dstr_replace(&escaped, "&", "&amp;");
		dstr_replace(&escaped, "<", "&lt;");
		dstr_replace(&escaped, ">", "&gt;");
		dstr_printf(&description, "<span>%s</span>", escaped.array);
		add_track_list(tracks, track_keys[i], description.array);
		dstr_free(&description);
		dstr_free(&escaped);
	}
	obs_data_release(settings);
	obs_properties_add_text(tracks, "rename", "<a href=\"" PORTAL_URL "\">Rename tracks in the portal</a> (syncs here and to viewers)", OBS_TEXT_INFO);
	obs_properties_add_group(props, "tracks", "Audio tracks (assign sources in Edit → Advanced Audio Properties)", OBS_GROUP_NORMAL, tracks);

	obs_properties_add_bool(props, "only_live", "Only send while streaming");
	return props;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	save_settings(settings);
	apply(settings);
}

static struct obs_source_info settings_source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	// Disabled: never listed under "Add source". It only exists as our private settings holder.
	.output_flags = OBS_SOURCE_CAP_DISABLED,
	.get_name = src_get_name,
	.create = src_create,
	.destroy = src_destroy,
	.get_defaults = src_defaults,
	.get_properties = src_properties,
	.update = src_update,
};

// ── Frontend glue ───────────────────────────────────────────────────────────────────────

static void open_settings(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_frontend_open_source_properties(settings_source);
}

// Once per release: open Tools → Faders at startup so the update notice is seen without a stream.
static void announce_update(void)
{
	obs_data_t *settings = obs_source_get_settings(settings_source);
	const char *update = pending_update(settings);
	if (update && strcmp(obs_data_get_string(settings, "update_announced"), update) != 0) {
		obs_data_set_string(settings, "update_announced", update);
		save_settings(settings);
		obs_queue_task(OBS_TASK_UI, open_settings, NULL, false);
	}
	obs_data_release(settings);
}

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		apply_current();
		announce_update();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		streaming = true;
		apply_current();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		streaming = false;
		apply_current();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		apply(NULL);
		break;
	default:
		break;
	}
}

// ── Scripting / automation procedures (also used by the headless test) ──────────────────
// faders_configure(server, key, voice, game, music, only_live) · faders_status(out status)

static void proc_watch_stream_track(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_watch_stream_track(uplink, (int)calldata_int(cd, "track"));
	pthread_mutex_unlock(&uplink_mu);
}

static void proc_configure(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "enabled", true);
	obs_data_set_string(s, "server", calldata_string(cd, "server"));
	obs_data_set_string(s, "key", calldata_string(cd, "key"));
	obs_data_set_int(s, "track_voice", calldata_int(cd, "voice"));
	obs_data_set_int(s, "track_game", calldata_int(cd, "game"));
	obs_data_set_int(s, "track_music", calldata_int(cd, "music"));
	obs_data_set_bool(s, "only_live", calldata_bool(cd, "only_live"));
	obs_source_update(settings_source, s); // persists and applies via src_update
	obs_data_release(s);
}

static void proc_status(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	struct dstr text = {0};
	status_text(&text);
	calldata_set_string(cd, "status", text.array);
	fdr_uplink_status st;
	get_status(&st);
	calldata_set_string(cd, "voice", st.fader_names[0]);
	calldata_set_string(cd, "game", st.fader_names[1]);
	calldata_set_string(cd, "music", st.fader_names[2]);
	dstr_free(&text);
}

bool obs_module_load(void)
{
	obs_register_source(&settings_source_info);

	char *path = config_file();
	obs_data_t *saved = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	settings_source = obs_source_create_private(SOURCE_ID, "Faders", saved);
	obs_data_release(saved);

	proc_handler_t *ph = obs_get_proc_handler();
	// Headless tests have no streaming output: point the streamed-track check at a track directly.
	proc_handler_add(ph, "void faders_watch_stream_track(in int track)", proc_watch_stream_track, NULL);
	proc_handler_add(ph, "void faders_configure(in string server, in string key, in int voice, in int game, in int music, in bool only_live)", proc_configure, NULL);
	proc_handler_add(ph, "void faders_status(out string status, out string voice, out string game, out string music)", proc_status, NULL);

	// No-ops when there's no OBS frontend (e.g. the headless test host).
	obs_frontend_add_tools_menu_item("Faders", open_settings, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	blog(LOG_INFO, "[faders] loaded (version %s)", FDR_VERSION_STR);
	return true;
}

void obs_module_unload(void)
{
	apply(NULL);
	obs_source_release(settings_source);
	settings_source = NULL;
}
