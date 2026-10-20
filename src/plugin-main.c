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
// Faders OBS plugin: sends your audio as separate layers (faders) so viewers can mix them.
//
// Two mixer modes. Sources (default since 0.4.0): each fader is a set of OBS sources, mixed by
// the plugin from per-source audio (mixer.c), plus "Everything else"; no OBS tracks involved.
// Tracks (legacy): faders are OBS mixer tracks 2/3/4 (uplink.c taps them).
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
#include "mixer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("faders", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Faders: let viewers set their own levels for your voice, game, music and more.";
}

#define SOURCE_ID "faders_uplink"
#define PORTAL_URL "https://faders.tv/portal/"
#define DOWNLOAD_URL "https://faders.tv/streamers/#download"
#define DOCS_URL "https://faders.tv/docs/faders-setup/"
// Production hub. The key alone identifies the channel, so streamers only paste their key. The
// server isn't shown in the settings; tests point it elsewhere through faders_configure.
#define DEFAULT_SERVER "wss://hub.faders.tv/ingest"
// Settings keys (settings.json in the plugin's config folder).
#define S_MODE "mixer_mode" // "sources" | "tracks"
#define S_FADERS "faders"   // editable list: [{"value":"Voice"},…]
#define S_MIGRATED "migrated_from_tracks"
#define SRC_PREFIX "src:" // per-source dropdowns in the dialog: "src:<uuid>" → fader name / "~rest" / "~auto"
#define CHOICE_REST "~rest"
#define CHOICE_AUTO "~auto"

static obs_source_t *settings_source; // private, never shown in a scene
static fdr_uplink_t *uplink;
static fdr_mixer_t *mixer;
static pthread_mutex_t uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static bool streaming;
static bool frontend_loaded;
static int forced_stream_track; // headless tests: faders_watch_stream_track
static int forced_transition_ms; // headless tests: faders_transition_duration
static int max_stems_seen = FDR_MAX_FADERS;
static bool sources_fallback; // sources mode asked for but unavailable (non-stereo output)

static char *config_file(void)
{
	return obs_module_config_path("settings.json");
}

static bool sources_mode(obs_data_t *s)
{
	return strcmp(obs_data_get_string(s, S_MODE), "tracks") != 0;
}

// ── Fader names from the settings' editable list ───────────────────────────────────────

static int fader_names(obs_data_t *s, char names[][FDR_STEM_NAME_MAX], int max)
{
	obs_data_array_t *arr = obs_data_get_array(s, S_FADERS);
	int n = 0;
	size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count && n < max; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *v = obs_data_get_string(item, "value");
		// Trim, cap the length, skip blanks and duplicates (names identify faders).
		while (*v == ' ')
			v++;
		char tmp[FDR_STEM_NAME_MAX];
		snprintf(tmp, sizeof(tmp), "%s", v);
		for (size_t k = strlen(tmp); k > 0 && tmp[k - 1] == ' '; k--)
			tmp[k - 1] = 0;
		bool dup = false;
		for (int j = 0; j < n; j++)
			dup = dup || astrcmpi(names[j], tmp) == 0;
		if (*tmp && !dup && astrcmpi(tmp, "Everything else") != 0)
			snprintf(names[n++], FDR_STEM_NAME_MAX, "%s", tmp);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	return n;
}

static obs_data_array_t *names_array(const char *const *names, int n)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < n; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", names[i]);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	return arr;
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
	if (forced_stream_track)
		return forced_stream_track;
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return 0;
	obs_encoder_t *enc = obs_output_get_audio_encoder(out, 0);
	int track = enc ? (int)obs_encoder_get_mixer_index(enc) + 1 : 0;
	obs_output_release(out);
	return track;
}

// Locked. Push the fader list (and the plan's allowance) into a running mixer.
static void apply_faders(obs_data_t *s)
{
	if (!mixer)
		return;
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n = fader_names(s, names, FDR_MAX_FADERS);
	const char *ptrs[FDR_MAX_FADERS];
	for (int i = 0; i < n; i++)
		ptrs[i] = names[i];
	fdr_mixer_set_faders(mixer, ptrs, n, max_stems_seen);
}

static void apply(obs_data_t *s)
{
	pthread_mutex_lock(&uplink_mu);
	fdr_mixer_stop(mixer);
	mixer = NULL;
	fdr_uplink_stop(uplink);
	uplink = NULL;
	sources_fallback = false;
	if (s && should_run(s)) {
		bool want_sources = sources_mode(s);
		fdr_uplink_config cfg = {
			.server = obs_data_get_string(s, "server"),
			.key = obs_data_get_string(s, "key"),
			.names_changed = names_changed,
			.update_available = update_available,
			.tracks = {(int)obs_data_get_int(s, "track_voice"), (int)obs_data_get_int(s, "track_game"),
				   (int)obs_data_get_int(s, "track_music")},
			.sources_mode = want_sources,
		};
		int stream_track = stream_audio_track();
		if (want_sources) {
			// The mixer needs a stereo, 48 kHz output; otherwise this session runs in tracks mode.
			uplink = fdr_uplink_start(&cfg);
			mixer = uplink ? fdr_mixer_start(uplink, stream_track ? stream_track : 1) : NULL;
			if (uplink && !mixer) {
				fdr_uplink_stop(uplink);
				cfg.sources_mode = false;
				uplink = fdr_uplink_start(&cfg);
				sources_fallback = true;
			}
		} else {
			uplink = fdr_uplink_start(&cfg);
		}
		if (mixer) {
			apply_faders(s);
			if (frontend_loaded)
				fdr_mixer_set_transition_duration(mixer, obs_frontend_get_transition_duration());
			else if (forced_transition_ms)
				fdr_mixer_set_transition_duration(mixer, forced_transition_ms);
		}
		if (uplink && (streaming || forced_stream_track))
			fdr_uplink_watch_stream_track(uplink, stream_track);
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
	// Remembered for the dialog when the plugin is idle (the mixer follows the hub itself).
	if (uplink && st->max_stems > 0)
		max_stems_seen = st->max_stems;
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
	bool sources = mixer != NULL;
	if (!st.running)
		dstr_copy(out, streaming ? "Not running. Check your key." : "Idle. Starts when you go live.");
	else if (!st.connected)
		dstr_printf(out, "Connecting… %s", st.last_error[0] ? st.last_error : "");
	else if (st.stream_track_silent)
		dstr_printf(out, "Connected, but Track %d (what OBS streams) is silent: viewers hear nothing.", st.stream_track);
	else if (st.mix_mismatch[0] && sources)
		dstr_printf(out, "Connected. \"%s\" isn't in your stream the way Faders sends it: moved to Everything else.", st.mix_mismatch);
	else if (st.mix_mismatch[0])
		dstr_printf(out, "Connected, but %s is sent to Faders and not to your stream (Track %d).", st.mix_mismatch, st.stream_track);
	else if (st.listeners == 0)
		dstr_printf(out, "Connected. Paused until a viewer turns Faders on. %d layers%s.", st.stems, sources && st.stream_track ? "; Faders mix matches your stream" : "");
	else
		dstr_printf(out, "Connected. %d listener%s, %d kbps, %d layers%s.", st.listeners, st.listeners == 1 ? "" : "s", st.uplink_kbps, st.stems, sources && st.stream_track ? "; Faders mix matches your stream" : "");
}

// ── Migration from tracks mode (first run of 0.4.0 on an existing install) ─────────────

struct migrate {
	int tracks[3];
	int moved;
	struct dstr summary;
};

static bool migrate_cb(void *param, obs_source_t *s)
{
	struct migrate *mg = param;
	uint32_t flags = obs_source_get_output_flags(s);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE))
		return true;
	static const char *const names[] = {"Voice", "Game", "Music"};
	uint32_t mixers = obs_source_get_audio_mixers(s);
	const char *fader = NULL;
	for (int i = 0; i < 3 && !fader; i++)
		if (mg->tracks[i] >= 1 && mg->tracks[i] <= 6 && (mixers & (1u << (mg->tracks[i] - 1))))
			fader = names[i];
	obs_data_t *priv = obs_source_get_private_settings(s);
	if (!obs_data_has_user_value(priv, FDR_FADER_SETTING)) {
		// On a Faders track → that fader; on none → Everything else (what it was for listeners).
		obs_data_set_string(priv, FDR_FADER_SETTING, fader ? fader : "");
		mg->moved++;
		dstr_catf(&mg->summary, "%s\"%s\" → %s", mg->moved > 1 ? ", " : "", obs_source_get_name(s), fader ? fader : "Everything else");
	}
	obs_data_release(priv);
	return true;
}

// Called once the scene collection is loaded (sources exist). Writes the assignments the track
// matrix implied, switches to sources mode, and logs what it did.
static void migrate_if_needed(void)
{
	if (!settings_source)
		return;
	obs_data_t *s = obs_source_get_settings(settings_source);
	bool legacy = obs_data_has_user_value(s, "track_voice") && !obs_data_has_user_value(s, S_MODE) && !obs_data_get_bool(s, S_MIGRATED);
	if (legacy) {
		struct migrate mg = {{(int)obs_data_get_int(s, "track_voice"), (int)obs_data_get_int(s, "track_game"), (int)obs_data_get_int(s, "track_music")}, 0, {0}};
		obs_enum_sources(migrate_cb, &mg);
		obs_data_t *upd = obs_data_create();
		obs_data_set_string(upd, S_MODE, "sources");
		obs_data_set_bool(upd, S_MIGRATED, true);
		obs_source_update(settings_source, upd); // persists and applies
		obs_data_release(upd);
		blog(LOG_INFO, "[faders] switched to sources mode: %d source(s) assigned from your tracks (voice=%d game=%d music=%d)%s%s. Your Faders tracks are no longer needed.", mg.moved, mg.tracks[0], mg.tracks[1], mg.tracks[2], mg.moved ? ": " : "", mg.summary.array ? mg.summary.array : "");
		dstr_free(&mg.summary);
	}
	obs_data_release(s);
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
	obs_data_set_default_string(s, S_MODE, "sources");
	static const char *const defaults[] = {"Voice", "Game", "Music"};
	obs_data_array_t *arr = names_array(defaults, 3);
	obs_data_set_default_array(s, S_FADERS, arr);
	obs_data_array_release(arr);
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

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool tracks = !sources_mode(settings);
	obs_property_set_visible(obs_properties_get(props, "tracks"), tracks);
	obs_property_set_visible(obs_properties_get(props, "faders_group"), !tracks);
	obs_property_set_visible(obs_properties_get(props, "sources_group"), !tracks);
	return true;
}

struct source_rows {
	obs_properties_t *group;
	obs_data_t *settings;
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n;
	int rows;
};

static bool add_source_row(void *param, obs_source_t *s)
{
	struct source_rows *r = param;
	uint32_t flags = obs_source_get_output_flags(s);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE) || obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT)
		return true;
	char key[80];
	snprintf(key, sizeof(key), SRC_PREFIX "%s", obs_source_get_uuid(s));
	obs_property_t *p = obs_properties_add_list(r->group, key, obs_source_get_name(s), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	const char *pick = fdr_mixer_auto_fader(s);
	// The automatic pick, as the mixer resolves it against the configured faders.
	const char *resolved = NULL;
	for (int i = 0; i < r->n && !resolved; i++)
		if (pick && astrcmpi(r->names[i], pick) == 0)
			resolved = r->names[i];
	if (!resolved && pick && strcmp(pick, "Discord") == 0)
		for (int i = 0; i < r->n && !resolved; i++)
			if (astrcmpi(r->names[i], "Voice") == 0)
				resolved = r->names[i];
	char auto_label[64];
	snprintf(auto_label, sizeof(auto_label), "Auto (→ %s)", resolved ? resolved : "Everything else");
	obs_property_list_add_string(p, auto_label, CHOICE_AUTO);
	for (int i = 0; i < r->n; i++)
		obs_property_list_add_string(p, r->names[i], r->names[i]);
	obs_property_list_add_string(p, "Everything else", CHOICE_REST);
	// What the dialog shows: the source's current assignment.
	obs_data_t *priv = obs_source_get_private_settings(s);
	const char *cur = obs_data_has_user_value(priv, FDR_FADER_SETTING) ? obs_data_get_string(priv, FDR_FADER_SETTING) : CHOICE_AUTO;
	if (cur[0] == 0)
		cur = CHOICE_REST;
	else if (strcmp(cur, CHOICE_AUTO) != 0) {
		bool known = false;
		for (int i = 0; i < r->n; i++)
			known = known || astrcmpi(r->names[i], cur) == 0;
		if (!known) { // assigned to a fader that no longer exists: shown as is, mixes as Everything else
			char label[96];
			snprintf(label, sizeof(label), "%s (no such fader: Everything else)", cur);
			obs_property_list_add_string(p, label, cur);
		}
	}
	obs_data_set_string(r->settings, key, cur);
	obs_data_release(priv);
	r->rows++;
	return true;
}

static obs_properties_t *src_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = obs_source_get_settings(settings_source);

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
			    "Audio Properties, tick Track %d for your mic, game and music. "
			    "<a href=\"https://faders.tv/docs/obs-tracks/\">How to set up tracks</a>",
			    now_st.stream_track, now_st.stream_track);
		obs_property_t *w = obs_properties_add_text(props, "stream_silent", warn.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_ERROR);
		obs_property_text_set_info_word_wrap(w, true);
		dstr_free(&warn);
	}
	if (now_st.mix_mismatch[0]) {
		struct dstr warn = {0};
		if (mixer)
			dstr_printf(&warn,
				    "\"%s\" is sent to Faders but isn't in your stream the same way (not on Track %d, captured twice, or "
				    "monitor-only), so it has been moved to Everything else for this session. Fix the source, then "
				    "restart the stream or the plugin. <a href=\"" DOCS_URL "\">How to assign sources</a>",
				    now_st.mix_mismatch, now_st.stream_track);
		else
			dstr_printf(&warn,
				    "Your %s layer is sent to Faders but is not in Track %d, the one OBS streams, so viewers "
				    "can't hear it from the stream and the extension can't line it up. In Edit → Advanced Audio "
				    "Properties, tick Track %d for that source as well as its Faders track. "
				    "<a href=\"https://faders.tv/docs/obs-tracks/\">How to set up tracks</a>",
				    now_st.mix_mismatch, now_st.stream_track, now_st.stream_track);
		obs_property_t *w = obs_properties_add_text(props, "mix_mismatch", warn.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(w, true);
		dstr_free(&warn);
	}
	if (sources_fallback) {
		obs_property_t *w = obs_properties_add_text(props, "fallback", "Faders is using OBS tracks for this session: assigning sources needs stereo, 48 kHz OBS audio (Settings → Audio).", OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(w, true);
	}

	// The hub says when a newer release is out; the link always goes to faders.tv.
	if (now_st.update_available[0]) {
		struct dstr label = {0};
		dstr_printf(&label, "Update available: Faders %s (you have %s). <a href=\"%s\">Download it</a>", now_st.update_available, FDR_VERSION_STR, DOWNLOAD_URL);
		obs_property_t *upd = obs_properties_add_text(props, "update", label.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(upd, OBS_TEXT_INFO_WARNING);
		dstr_free(&label);
	}

	obs_properties_add_bool(props, "enabled", "Send Faders layers");
	obs_properties_add_text(props, "key", "Key", OBS_TEXT_PASSWORD);
	// An info row renders as a QLabel with external links enabled: a real hyperlink that opens the
	// browser directly (a URL button asks "Open this URL?" first).
	obs_properties_add_text(props, "portal", "<a href=\"" PORTAL_URL "\">Open the Faders portal to get key</a>", OBS_TEXT_INFO);

	// ── Faders: the list of names viewers get, capped by the plan ──────────────────────
	obs_properties_t *faders = obs_properties_create();
	struct dstr cap = {0};
	int allowance = now_st.max_stems > 0 ? now_st.max_stems : max_stems_seen;
	struct source_rows rows = {.settings = settings};
	rows.n = fader_names(settings, rows.names, FDR_MAX_FADERS);
	if (rows.n > allowance)
		dstr_printf(&cap, "Your plan includes %d faders: \"%s\" and the ones after it are folded into Everything else until you upgrade. <a href=\"" PORTAL_URL "\">Plans</a>", allowance, rows.names[allowance]);
	else
		dstr_printf(&cap, "Your plan includes %d faders. \"Everything else\" (alerts, transitions, anything not in a fader) is always sent too, so what viewers hear adds up to your stream.", allowance);
	obs_property_t *capinfo = obs_properties_add_text(faders, "cap_info", cap.array, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(capinfo, true);
	dstr_free(&cap);
	obs_properties_add_editable_list(faders, S_FADERS, "Faders (one per line, in order)", OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);
	obs_properties_add_group(props, "faders_group", "Faders", OBS_GROUP_NORMAL, faders);

	// ── Sources: one dropdown per audio source ────────────────────────────────────────
	obs_properties_t *sources = obs_properties_create();
	rows.group = sources;
	obs_enum_sources(add_source_row, &rows);
	if (!rows.rows)
		obs_properties_add_text(sources, "no_sources", "No audio sources in this scene collection yet.", OBS_TEXT_INFO);
	obs_properties_add_group(props, "sources_group", "Sources (which fader each one belongs to)", OBS_GROUP_NORMAL, sources);

	// ── Advanced: mode, and the legacy track dropdowns ────────────────────────────────
	obs_properties_t *adv = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(adv, S_MODE, "Mixer", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "Sources (assign sources to faders here)", "sources");
	obs_property_list_add_string(mode, "Tracks (legacy: OBS tracks 2/3/4 are voice/game/music)", "tracks");
	obs_property_set_modified_callback(mode, mode_modified);
	obs_properties_add_bool(adv, "only_live", "Only send while streaming");
	obs_properties_add_text(adv, "version", "Faders plugin " FDR_VERSION_STR, OBS_TEXT_INFO);
	obs_properties_add_group(props, "advanced", "Advanced", OBS_GROUP_NORMAL, adv);

	obs_properties_t *tracks = obs_properties_create();
	add_track_list(tracks, "track_voice", "Voice");
	add_track_list(tracks, "track_game", "Game");
	add_track_list(tracks, "track_music", "Music");
	obs_properties_add_group(props, "tracks", "Audio tracks (tracks mode; assign sources in Edit → Advanced Audio Properties)", OBS_GROUP_NORMAL, tracks);

	mode_modified(props, mode, settings);
	obs_data_release(settings);
	return props;
}

// Apply the per-source dropdown choices to the sources' private settings.
static bool apply_source_choice(obs_data_item_t *item)
{
	const char *name = obs_data_item_get_name(item);
	if (strncmp(name, SRC_PREFIX, strlen(SRC_PREFIX)) != 0 || obs_data_item_gettype(item) != OBS_DATA_STRING)
		return true;
	obs_source_t *s = obs_get_source_by_uuid(name + strlen(SRC_PREFIX));
	if (!s)
		return true;
	const char *choice = obs_data_item_get_string(item);
	obs_data_t *priv = obs_source_get_private_settings(s);
	const char *before = obs_data_has_user_value(priv, FDR_FADER_SETTING) ? obs_data_get_string(priv, FDR_FADER_SETTING) : CHOICE_AUTO;
	const char *after = strcmp(choice, CHOICE_REST) == 0 ? "" : choice;
	if (strcmp(before, after) != 0) {
		if (strcmp(after, CHOICE_AUTO) == 0)
			obs_data_erase(priv, FDR_FADER_SETTING);
		else
			obs_data_set_string(priv, FDR_FADER_SETTING, after);
		blog(LOG_INFO, "[faders] \"%s\" → %s", obs_source_get_name(s), *after ? (strcmp(after, CHOICE_AUTO) == 0 ? "auto" : after) : "Everything else");
	}
	obs_data_release(priv);
	obs_source_release(s);
	return true;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item))
		apply_source_choice(item);
	char *path = config_file();
	obs_data_save_json_safe(settings, path, "tmp", "bak");
	bfree(path);
	// A change of names or assignments while running doesn't need a restart.
	pthread_mutex_lock(&uplink_mu);
	bool live = uplink && mixer && sources_mode(settings) && obs_data_get_bool(settings, "enabled");
	if (live)
		apply_faders(settings);
	pthread_mutex_unlock(&uplink_mu);
	if (!live)
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
		frontend_loaded = true;
		migrate_if_needed();
		apply_current();
		announce_update();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_set_transition_duration(mixer, obs_frontend_get_transition_duration());
		pthread_mutex_unlock(&uplink_mu);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		migrate_if_needed();
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_rebuild(mixer);
		pthread_mutex_unlock(&uplink_mu);
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_rebuild(mixer);
		pthread_mutex_unlock(&uplink_mu);
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

// ── Scripting / automation procedures (also used by the headless tests) ─────────────────
// faders_configure(server, key, voice, game, music, only_live): tracks mode.
// faders_configure_sources(server, key, faders, only_live): sources mode; faders = comma list.
// faders_assign(source, fader): fader name, "" = Everything else, "auto".
// faders_watch_stream_track(track) · faders_status(out status) · faders_mixer_stats(out json)

static void proc_watch_stream_track(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	forced_stream_track = (int)calldata_int(cd, "track");
	pthread_mutex_lock(&uplink_mu);
	fdr_uplink_watch_stream_track(uplink, forced_stream_track);
	fdr_mixer_set_stream_track(mixer, forced_stream_track);
	pthread_mutex_unlock(&uplink_mu);
}

static void proc_configure(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "enabled", true);
	obs_data_set_string(s, "server", calldata_string(cd, "server"));
	obs_data_set_string(s, "key", calldata_string(cd, "key"));
	obs_data_set_string(s, S_MODE, "tracks");
	obs_data_set_bool(s, S_MIGRATED, true);
	obs_data_set_int(s, "track_voice", calldata_int(cd, "voice"));
	obs_data_set_int(s, "track_game", calldata_int(cd, "game"));
	obs_data_set_int(s, "track_music", calldata_int(cd, "music"));
	obs_data_set_bool(s, "only_live", calldata_bool(cd, "only_live"));
	obs_source_update(settings_source, s); // persists and applies via src_update
	obs_data_release(s);
}

static void proc_configure_sources(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "enabled", true);
	obs_data_set_string(s, "server", calldata_string(cd, "server"));
	obs_data_set_string(s, "key", calldata_string(cd, "key"));
	obs_data_set_string(s, S_MODE, "sources");
	obs_data_set_bool(s, S_MIGRATED, true);
	obs_data_set_bool(s, "only_live", calldata_bool(cd, "only_live"));
	// "Voice,Game,Music,Discord"
	char list[512];
	snprintf(list, sizeof(list), "%s", calldata_string(cd, "faders") ? calldata_string(cd, "faders") : "");
	const char *names[FDR_MAX_FADERS];
	int n = 0;
	for (char *tok = strtok(list, ","); tok && n < FDR_MAX_FADERS; tok = strtok(NULL, ","))
		names[n++] = tok;
	obs_data_array_t *arr = names_array(names, n);
	obs_data_set_array(s, S_FADERS, arr);
	obs_data_array_release(arr);
	obs_source_update(settings_source, s);
	obs_data_release(s);
}

static void proc_assign(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_source_t *src = obs_get_source_by_name(calldata_string(cd, "source"));
	const char *fader = calldata_string(cd, "fader");
	pthread_mutex_lock(&uplink_mu);
	if (src)
		fdr_mixer_assign(mixer, src, fader && strcmp(fader, "auto") == 0 ? NULL : fader);
	pthread_mutex_unlock(&uplink_mu);
	obs_source_release(src);
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

static void proc_mixer_stats(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	struct dstr json = {0};
	pthread_mutex_lock(&uplink_mu);
	fdr_mixer_stats_json(mixer, &json);
	pthread_mutex_unlock(&uplink_mu);
	calldata_set_string(cd, "json", json.array);
	dstr_free(&json);
}

static void proc_transition_duration(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	forced_transition_ms = (int)calldata_int(cd, "ms");
	pthread_mutex_lock(&uplink_mu);
	fdr_mixer_set_transition_duration(mixer, forced_transition_ms);
	pthread_mutex_unlock(&uplink_mu);
}

static void proc_rebuild(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(cd);
	pthread_mutex_lock(&uplink_mu);
	fdr_mixer_rebuild(mixer);
	pthread_mutex_unlock(&uplink_mu);
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
	proc_handler_add(ph, "void faders_configure_sources(in string server, in string key, in string faders, in bool only_live)", proc_configure_sources, NULL);
	proc_handler_add(ph, "void faders_assign(in string source, in string fader)", proc_assign, NULL);
	proc_handler_add(ph, "void faders_status(out string status)", proc_status, NULL);
	proc_handler_add(ph, "void faders_mixer_stats(out string json)", proc_mixer_stats, NULL);
	proc_handler_add(ph, "void faders_rebuild()", proc_rebuild, NULL);
	proc_handler_add(ph, "void faders_transition_duration(in int ms)", proc_transition_duration, NULL);

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
