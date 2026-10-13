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
// Production hub. The key alone identifies the channel, so streamers only paste their key; the
// server stays editable (Advanced) for testing against another hub.
#define DEFAULT_SERVER "wss://hub.faders.tv/ingest"

static obs_source_t *settings_source; // private, never shown in a scene
static fdr_uplink_t *uplink;
static pthread_mutex_t uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static bool streaming;

static char *config_file(void)
{
	return obs_module_config_path("settings.json");
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

static void apply(obs_data_t *s)
{
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_stop(uplink);
	uplink = NULL;
	if (s && should_run(s)) {
		fdr_uplink_config cfg = {
			.server = obs_data_get_string(s, "server"),
			.key = obs_data_get_string(s, "key"),
			.tracks = {(int)obs_data_get_int(s, "track_voice"), (int)obs_data_get_int(s, "track_game"),
				   (int)obs_data_get_int(s, "track_music")},
		};
		uplink = fdr_uplink_start(&cfg);
	}
	pthread_mutex_unlock(&uplink_mu);
}

static void apply_current(void)
{
	obs_data_t *s = settings_source ? obs_source_get_settings(settings_source) : NULL;
	apply(s);
	obs_data_release(s);
}

static void status_text(struct dstr *out)
{
	fdr_uplink_status st;
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_get_status(uplink, &st);
	pthread_mutex_unlock(&uplink_mu);
	if (!st.running)
		dstr_copy(out, streaming ? "Not running. Check the server and key." : "Idle. Starts when you go live.");
	else if (!st.connected)
		dstr_printf(out, "Connecting… %s", st.last_error[0] ? st.last_error : "");
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

	obs_properties_add_bool(props, "enabled", "Send Faders layers");
	obs_properties_add_text(props, "key", "Key", OBS_TEXT_PASSWORD);
	obs_property_t *portal = obs_properties_add_button(props, "portal", "Get your key from the Faders portal", NULL);
	obs_property_button_set_type(portal, OBS_BUTTON_URL);
	obs_property_button_set_url(portal, PORTAL_URL);

	obs_properties_t *tracks = obs_properties_create();
	add_track_list(tracks, "track_voice", "Voice");
	add_track_list(tracks, "track_game", "Game");
	add_track_list(tracks, "track_music", "Music");
	obs_properties_add_group(props, "tracks", "Audio tracks (assign sources in Edit → Advanced Audio Properties)", OBS_GROUP_NORMAL, tracks);

	obs_properties_add_bool(props, "only_live", "Only send while streaming");

	obs_properties_t *advanced = obs_properties_create();
	obs_properties_add_text(advanced, "server", "Server", OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "advanced", "Advanced", OBS_GROUP_NORMAL, advanced);
	return props;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	char *path = config_file();
	obs_data_save_json_safe(settings, path, "tmp", "bak");
	bfree(path);
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

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		apply_current();
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
	proc_handler_add(ph, "void faders_configure(in string server, in string key, in int voice, in int game, in int music, in bool only_live)", proc_configure, NULL);
	proc_handler_add(ph, "void faders_status(out string status)", proc_status, NULL);

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
