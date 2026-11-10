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
// Each fader is a set of OBS sources, mixed by the plugin from per-source audio (mixer.c),
// plus "Everything else"; no OBS tracks are involved.
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
#define S_FADERS "faders" // editable list: [{"value":"Voice"},…]
#define CHOICE_REST "~rest"
// OBS tracks as fader inputs: "track:<1-6>" → fader name / "~rest" / "~none" (the default).
#define TRACK_PREFIX "track:"
#define CHOICE_NONE "~none"

static obs_source_t *settings_source; // private, never shown in a scene
static fdr_uplink_t *uplink;
static fdr_mixer_t *mixer;
static pthread_mutex_t uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static bool streaming;
static bool frontend_loaded;
static int forced_stream_track; // headless tests: faders_watch_stream_track
static int forced_transition_ms; // headless tests: faders_transition_duration
static int max_stems_seen = FDR_MAX_FADERS;
static bool mixer_unavailable; // the OBS audio output isn't stereo 48 kHz: nothing runs

static char *config_file(void)
{
	return obs_module_config_path("settings.json");
}

static void save_settings(obs_data_t *settings)
{
	char *path = config_file();
	obs_data_save_json_safe(settings, path, "tmp", "bak");
	bfree(path);
}

static void update_available(const char *version);

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
	const char *tracks[6];
	for (int t = 0; t < 6; t++) {
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t + 1);
		const char *v = obs_data_get_string(s, key);
		tracks[t] = !*v || strcmp(v, CHOICE_NONE) == 0 ? NULL : strcmp(v, CHOICE_REST) == 0 ? "" : v;
	}
	fdr_mixer_set_track_faders(mixer, tracks);
}

// The assigned track (1-6) that decides `s`'s fader from the settings alone (the dialog shows
// this whether or not the mixer is running), or 0; *fader is its choice.
static int settings_track_for(obs_data_t *settings, obs_source_t *s, const char **fader)
{
	uint32_t mixers = obs_source_get_audio_mixers(s);
	int stream = stream_audio_track();
	for (int t = 1; t <= 6; t++) {
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
		const char *v = obs_data_get_string(settings, key);
		if (!*v || strcmp(v, CHOICE_NONE) == 0 || t == (stream ? stream : 1) || !(mixers & (1u << (t - 1))))
			continue;
		*fader = v;
		return t;
	}
	return 0;
}

static void apply(obs_data_t *s)
{
	pthread_mutex_lock(&uplink_mu);
	fdr_mixer_stop(mixer);
	mixer = NULL;
	fdr_uplink_stop(uplink);
	uplink = NULL;
	mixer_unavailable = false;
	if (s && should_run(s)) {
		fdr_uplink_config cfg = {.server = obs_data_get_string(s, "server"), .key = obs_data_get_string(s, "key"), .update_available = update_available};
		int stream_track = stream_audio_track();
		uplink = fdr_uplink_start(&cfg);
		// The mixer needs a stereo, 48 kHz output; without it nothing is sent.
		mixer = uplink ? fdr_mixer_start(uplink, stream_track ? stream_track : 1) : NULL;
		if (uplink && !mixer) {
			fdr_uplink_stop(uplink);
			uplink = NULL;
			mixer_unavailable = true;
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
	if (mixer_unavailable)
		dstr_copy(out, "Not running: Faders needs stereo, 48 kHz OBS audio (Settings → Audio).");
	else if (!st.running)
		dstr_copy(out, streaming ? "Not running. Check your key." : "Idle. Starts when you go live.");
	else if (!st.connected)
		dstr_printf(out, "Connecting… %s", st.last_error[0] ? st.last_error : "");
	else if (st.stream_track_silent)
		dstr_printf(out, "Connected, but Track %d (what OBS streams) is silent: viewers hear nothing.", st.stream_track);
	else if (st.mix_mismatch[0])
		dstr_printf(out, "Connected. \"%s\" isn't in your stream the way Faders sends it: moved to Everything else.", st.mix_mismatch);
	else if (st.listeners == 0)
		dstr_printf(out, "Connected. Paused until a viewer turns Faders on. %d layers%s.", st.stems, st.stream_track ? "; Faders mix matches your stream" : "");
	else
		dstr_printf(out, "Connected. %d listener%s, %d kbps, %d layers%s.", st.listeners, st.listeners == 1 ? "" : "s", st.uplink_kbps, st.stems, st.stream_track ? "; Faders mix matches your stream" : "");
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
	for (int t = 1; t <= 6; t++) {
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
		obs_data_set_default_string(s, key, CHOICE_NONE);
	}
	static const char *const defaults[] = {"Voice", "Game", "Music"};
	obs_data_array_t *arr = names_array(defaults, 3);
	obs_data_set_default_array(s, S_FADERS, arr);
	obs_data_array_release(arr);
}

// ── The dialog: one block per fader, with a dropdown for each source or track on it ───
// Rows are rebuilt from what's true (each source's private setting, the track settings) whenever
// the dialog opens or an assignment changes; the table below remembers what each dropdown showed,
// so an update can tell which one the streamer changed.
#define ROW_PREFIX "row:" // "row:<n>": one source or track on a fader
#define ADD_PREFIX "add:" // "add:<fader index>": the "Add a source or track…" dropdown
#define ITEM_SRC "s:"     // dropdown values: "s:<source uuid>", "t:<track 1-6>"
#define ITEM_TRACK "t:"
#define CHOICE_REMOVE "~remove"
#define MAX_ITEMS 128
#define MAX_DIALOG_ROWS 192

struct dialog_item {
	char id[80];
	char label[96];
	int fader;   // index into the names it has a row under, or -1: none (a track: not used)
	int via;     // a source with no row that an assigned track (1-6) brings to a fader, else 0
	uint32_t mixers; // a source: the OBS tracks it is ticked for
	bool track;
};

struct dialog_model {
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n;
	struct dialog_item items[MAX_ITEMS];
	int n_items;
	obs_data_t *settings;
};

struct dialog_row {
	char key[24];
	char fader[FDR_STEM_NAME_MAX];
	char item[80]; // "" for an add dropdown
};
static struct dialog_row dialog_rows[MAX_DIALOG_ROWS];
static int n_dialog_rows;
static char dialog_names[FDR_MAX_FADERS * FDR_STEM_NAME_MAX]; // the fader list the rows were built for

static int name_index(const struct dialog_model *m, const char *name)
{
	for (int i = 0; name && *name && i < m->n; i++)
		if (astrcmpi(m->names[i], name) == 0)
			return i;
	return -1;
}

static bool add_dialog_source(void *param, obs_source_t *s)
{
	struct dialog_model *m = param;
	uint32_t flags = obs_source_get_output_flags(s);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE) || obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT || m->n_items >= MAX_ITEMS)
		return true;
	struct dialog_item *it = &m->items[m->n_items++];
	memset(it, 0, sizeof(*it));
	snprintf(it->id, sizeof(it->id), ITEM_SRC "%s", obs_source_get_uuid(s));
	snprintf(it->label, sizeof(it->label), "%s", obs_source_get_name(s));
	it->mixers = obs_source_get_audio_mixers(s);
	// Its own row: the fader the streamer put it on, or the plugin's starting guess from the kind
	// of source. Tracks don't change that; they add to it.
	obs_data_t *priv = obs_source_get_private_settings(s);
	if (obs_data_has_user_value(priv, FDR_FADER_SETTING)) {
		it->fader = name_index(m, obs_data_get_string(priv, FDR_FADER_SETTING));
	} else {
		const char *pick = fdr_mixer_auto_fader(s);
		it->fader = name_index(m, pick);
		if (it->fader < 0 && pick && strcmp(pick, "Discord") == 0)
			it->fader = name_index(m, "Voice");
	}
	if (it->fader < 0) {
		const char *choice = NULL;
		int t = settings_track_for(m->settings, s, &choice);
		it->via = t && name_index(m, choice) >= 0 ? t : 0;
	}
	obs_data_release(priv);
	return true;
}

static void build_dialog_model(struct dialog_model *m, obs_data_t *settings)
{
	memset(m, 0, sizeof(*m));
	m->settings = settings;
	m->n = fader_names(settings, m->names, FDR_MAX_FADERS);
	obs_enum_sources(add_dialog_source, m);
	int stream_track = stream_audio_track() ? stream_audio_track() : 1;
	for (int t = 1; t <= 6 && m->n_items < MAX_ITEMS; t++) {
		if (t == stream_track) // everything in the stream is on it: nothing to pick
			continue;
		struct dialog_item *it = &m->items[m->n_items++];
		memset(it, 0, sizeof(*it));
		it->track = true;
		snprintf(it->id, sizeof(it->id), ITEM_TRACK "%d", t);
		snprintf(it->label, sizeof(it->label), "OBS Track %d (everything ticked for it)", t);
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
		it->fader = name_index(m, obs_data_get_string(settings, key));
	}
}

// One dropdown. An existing row shows what it holds and offers to remove it. The "Add…" row
// offers only what isn't on a fader yet: a source with a row of its own, or a track on a fader,
// is not in anyone else's list (to move one, remove it first).
static void add_dialog_row(obs_properties_t *group, struct dialog_model *m, int fader, const struct dialog_item *current, bool first)
{
	if (n_dialog_rows >= MAX_DIALOG_ROWS)
		return;
	struct dialog_row *row = &dialog_rows[n_dialog_rows];
	if (current)
		snprintf(row->key, sizeof(row->key), ROW_PREFIX "%d", n_dialog_rows);
	else
		snprintf(row->key, sizeof(row->key), ADD_PREFIX "%d", fader);
	snprintf(row->fader, sizeof(row->fader), "%s", m->names[fader]);
	snprintf(row->item, sizeof(row->item), "%s", current ? current->id : "");
	n_dialog_rows++;
	// The fader's name sits on the left of its first row only.
	obs_property_t *p = obs_properties_add_list(group, row->key, first ? m->names[fader] : " ", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (current)
		obs_property_list_add_string(p, current->label, current->id);
	else
		obs_property_list_add_string(p, "Add a source or track…", "");
	for (int i = 0; !current && i < m->n_items; i++) {
		const struct dialog_item *it = &m->items[i];
		if (it->fader >= 0) // already has a row on a fader
			continue;
		obs_property_list_add_string(p, it->label, it->id);
	}
	if (current) {
		char label[64];
		snprintf(label, sizeof(label), "Remove from %s", m->names[fader]);
		obs_property_list_add_string(p, label, CHOICE_REMOVE);
	}
	obs_data_set_string(m->settings, row->key, row->item);
}

// Sources that are on more than one fader (their own, and one a track they're ticked for is on),
// one sentence each: which source, which faders. Empty when there are none.
static void duplicate_warning(const struct dialog_model *m, struct dstr *out)
{
	for (int i = 0; i < m->n_items; i++) {
		const struct dialog_item *src = &m->items[i];
		if (src->track)
			continue;
		struct dstr where = {0};
		uint32_t seen = 0;
		int count = 0;
		if (src->fader >= 0) {
			seen |= 1u << src->fader;
			count++;
			dstr_cat(&where, m->names[src->fader]);
		}
		for (int k = 0; k < m->n_items; k++) {
			const struct dialog_item *tr = &m->items[k];
			int t = tr->track ? atoi(tr->id + strlen(ITEM_TRACK)) : 0;
			if (!t || tr->fader < 0 || !(src->mixers & (1u << (t - 1))) || (seen & (1u << tr->fader)))
				continue;
			seen |= 1u << tr->fader;
			dstr_catf(&where, "%s%s (with Track %d)", count ? ", " : "", m->names[tr->fader], t);
			count++;
		}
		if (count > 1)
			dstr_catf(out, "%s\"%s\" is on %d faders: %s.", out->len ? " " : "", src->label, count, where.array);
		dstr_free(&where);
	}
	if (out->len)
		dstr_cat(out, " Viewers hear a source once for each fader it's on, so these are louder for them than in your stream. To fix it, untick the source for that track in Edit → Advanced Audio Properties, or remove it from its own fader.");
}

static void add_assignment_rows(obs_properties_t *group, obs_data_t *settings)
{
	static struct dialog_model m; // large: off the stack (dialog code runs on the UI thread only)
	build_dialog_model(&m, settings);
	struct dstr dup = {0};
	duplicate_warning(&m, &dup);
	if (dup.len) {
		obs_property_t *w = obs_properties_add_text(group, "dup_warning", dup.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(w, true);
	}
	dstr_free(&dup);
	n_dialog_rows = 0;
	dialog_names[0] = 0;
	for (int f = 0; f < m.n; f++) {
		strncat(dialog_names, m.names[f], sizeof(dialog_names) - strlen(dialog_names) - 2);
		strcat(dialog_names, "\n");
		bool first = true;
		for (int i = 0; i < m.n_items; i++) {
			if (m.items[i].fader != f)
				continue;
			add_dialog_row(group, &m, f, &m.items[i], first);
			first = false;
		}
		add_dialog_row(group, &m, f, NULL, first);
	}
	// What's left over, by name, so nothing is a mystery (a source a track brings to a fader
	// isn't left over).
	struct dstr rest = {0};
	for (int i = 0; i < m.n_items; i++) {
		if (m.items[i].track || m.items[i].fader >= 0 || m.items[i].via)
			continue;
		dstr_cat(&rest, rest.len ? ", " : "");
		dstr_cat(&rest, m.items[i].label);
	}
	struct dstr text = {0};
	if (rest.len)
		dstr_printf(&text, "Everything else: %s. Anything not on a fader above goes here, along with alerts and transitions.", rest.array);
	else
		dstr_copy(&text, "Everything else: nothing right now. Any source you don't put on a fader goes here, along with transitions.");
	obs_property_t *info = obs_properties_add_text(group, "rest_info", text.array, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(info, true);
	dstr_free(&rest);
	dstr_free(&text);
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
			    "<a href=\"" DOCS_URL "\">Setup guide</a>",
			    now_st.stream_track, now_st.stream_track);
		obs_property_t *w = obs_properties_add_text(props, "stream_silent", warn.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_ERROR);
		obs_property_text_set_info_word_wrap(w, true);
		dstr_free(&warn);
	}
	if (now_st.mix_mismatch[0]) {
		struct dstr warn = {0};
		dstr_printf(&warn,
			    "\"%s\" is sent to Faders but isn't in your stream the same way (not on Track %d, captured twice, or "
			    "monitor-only), so it has been moved to Everything else for this session. Fix the source, then "
			    "restart the stream or the plugin. <a href=\"" DOCS_URL "\">How to assign sources</a>",
			    now_st.mix_mismatch, now_st.stream_track);
		obs_property_t *w = obs_properties_add_text(props, "mix_mismatch", warn.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(w, true);
		dstr_free(&warn);
	}
	if (mixer_unavailable) {
		obs_property_t *w = obs_properties_add_text(props, "unavailable", "Faders isn't running: it needs stereo, 48 kHz OBS audio (Settings → Audio → Channels and Sample Rate).", OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_ERROR);
		obs_property_text_set_info_word_wrap(w, true);
	}

	// The hub says when a newer release is out (kept in the settings, so it outlives the stream);
	// the link always goes to faders.tv.
	obs_data_t *cur = settings_source ? obs_source_get_settings(settings_source) : NULL;
	const char *update = pending_update(cur);
	if (update) {
		struct dstr label = {0};
		dstr_printf(&label, "Update available: Faders %s (you have %s). <a href=\"%s\">Download it</a>", update, FDR_VERSION_STR, DOWNLOAD_URL);
		obs_property_t *upd = obs_properties_add_text(props, "update", label.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(upd, OBS_TEXT_INFO_WARNING);
		dstr_free(&label);
	}
	obs_data_release(cur);

	obs_properties_add_bool(props, "enabled", "Send Faders layers");
	obs_properties_add_text(props, "key", "Key", OBS_TEXT_PASSWORD);
	// An info row renders as a QLabel with external links enabled: a real hyperlink that opens the
	// browser directly (a URL button asks "Open this URL?" first).
	obs_properties_add_text(props, "portal", "<a href=\"" PORTAL_URL "\">Open the Faders portal to get key</a>", OBS_TEXT_INFO);
	obs_properties_add_bool(props, "only_live", "Only send while streaming");

	// ── Faders: the list of names viewers get, capped by the plan ──────────────────────
	obs_properties_t *faders = obs_properties_create();
	struct dstr cap = {0};
	int allowance = now_st.max_stems > 0 ? now_st.max_stems : max_stems_seen;
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n_names = fader_names(settings, names, FDR_MAX_FADERS);
	if (n_names > allowance)
		dstr_printf(&cap, "Your plan includes %d faders: \"%s\" and the ones after it are folded into Everything else until you upgrade. <a href=\"" PORTAL_URL "\">Plans</a>", allowance, names[allowance]);
	else
		dstr_printf(&cap, "Your plan includes %d faders. \"Everything else\" (alerts, transitions, anything not in a fader) is always sent too, so what viewers hear adds up to your stream.", allowance);
	obs_property_t *capinfo = obs_properties_add_text(faders, "cap_info", cap.array, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(capinfo, true);
	dstr_free(&cap);
	obs_properties_add_editable_list(faders, S_FADERS, "Faders (one per line, in order)", OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);
	obs_properties_add_group(props, "faders_group", "Faders", OBS_GROUP_NORMAL, faders);

	// ── What goes on each fader: its sources and OBS tracks, one dropdown each ────────
	obs_properties_t *assign = obs_properties_create();
	add_assignment_rows(assign, settings);
	obs_properties_add_group(props, "sources_group", "What's on each fader", OBS_GROUP_NORMAL, assign);
	obs_properties_add_text(props, "version", "Faders plugin " FDR_VERSION_STR, OBS_TEXT_INFO);

	obs_data_release(settings);
	return props;
}

// Put a source or track on a fader, or (fader NULL) take it off: a source then goes to Everything
// else, a track stops being used.
static void assign_item(obs_data_t *settings, const char *item, const char *fader)
{
	if (strncmp(item, ITEM_TRACK, strlen(ITEM_TRACK)) == 0) {
		int t = atoi(item + strlen(ITEM_TRACK));
		if (t < 1 || t > 6)
			return;
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
		obs_data_set_string(settings, key, fader ? fader : CHOICE_NONE);
		return;
	}
	if (strncmp(item, ITEM_SRC, strlen(ITEM_SRC)) != 0)
		return;
	obs_source_t *s = obs_get_source_by_uuid(item + strlen(ITEM_SRC));
	if (!s)
		return;
	obs_data_t *priv = obs_source_get_private_settings(s);
	obs_data_set_string(priv, FDR_FADER_SETTING, fader ? fader : "");
	blog(LOG_INFO, "[faders] \"%s\" → %s", obs_source_get_name(s), fader ? fader : "Everything else");
	obs_data_release(priv);
	obs_source_release(s);
}

// The dropdowns the streamer changed since the dialog was built. Returns whether any did.
static bool apply_dialog_changes(obs_data_t *settings)
{
	bool changed = false;
	for (int i = 0; i < n_dialog_rows; i++) {
		struct dialog_row *row = &dialog_rows[i];
		if (!obs_data_has_user_value(settings, row->key))
			continue;
		const char *now = obs_data_get_string(settings, row->key);
		if (strcmp(now, row->item) == 0)
			continue;
		changed = true;
		if (*row->item) // an existing row: what it held leaves this fader
			assign_item(settings, row->item, NULL);
		if (*now && strcmp(now, CHOICE_REMOVE) != 0)
			assign_item(settings, now, row->fader);
	}
	// The rows are rebuilt next; their keys mean nothing outside this dialog session.
	for (int i = 0; i < n_dialog_rows; i++)
		obs_data_erase(settings, dialog_rows[i].key);
	if (changed)
		n_dialog_rows = 0;
	return changed;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	bool reassigned = apply_dialog_changes(settings);
	// Renaming, adding or removing a fader changes the rows too.
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n = fader_names(settings, names, FDR_MAX_FADERS);
	char joined[sizeof(dialog_names)] = "";
	for (int i = 0; i < n; i++) {
		strncat(joined, names[i], sizeof(joined) - strlen(joined) - 2);
		strcat(joined, "\n");
	}
	bool renamed = n_dialog_rows > 0 && strcmp(joined, dialog_names) != 0;
	save_settings(settings);
	// A change of names or assignments while running doesn't need a restart.
	pthread_mutex_lock(&uplink_mu);
	bool live = uplink && mixer && obs_data_get_bool(settings, "enabled");
	if (live)
		apply_faders(settings);
	pthread_mutex_unlock(&uplink_mu);
	if (!live)
		apply(settings);
	// An open dialog redraws with the new rows (it asks for the properties again).
	if ((reassigned || renamed) && settings_source)
		obs_source_update_properties(settings_source);
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
		apply_current();
		announce_update();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_set_transition_duration(mixer, obs_frontend_get_transition_duration());
		pthread_mutex_unlock(&uplink_mu);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
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
// faders_configure(server, key, faders, only_live): faders = comma list ("Voice,Game,Music").
// faders_assign(source, fader): fader name, "" = Everything else, "auto".
// faders_assign_track(track, fader): fader name, "" = Everything else, "none" = not used.
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

static void proc_assign_track(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	int track = (int)calldata_int(cd, "track");
	const char *fader = calldata_string(cd, "fader");
	if (track < 1 || track > 6 || !fader)
		return;
	char key[16];
	snprintf(key, sizeof(key), TRACK_PREFIX "%d", track);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, key, strcmp(fader, "none") == 0 ? CHOICE_NONE : *fader ? fader : CHOICE_REST);
	obs_source_update(settings_source, s);
	obs_data_release(s);
}

// Test hooks for the dialog, which is plain OBS properties: faders_dialog builds it exactly as the
// window would and returns the "What's on each fader" rows, one per line, as
//   key<TAB>label<TAB>shown choice<TAB>choice|choice|…        (an info row: key<TAB>text)
// and faders_dialog_set changes one dropdown the way a click does.
static void proc_dialog(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_source_properties(settings_source);
	obs_data_t *settings = obs_source_get_settings(settings_source);
	struct dstr out = {0};
	obs_property_t *group = obs_properties_get(props, "sources_group");
	for (obs_property_t *p = group ? obs_properties_first(obs_property_group_content(group)) : NULL; p; obs_property_next(&p)) {
		const char *key = obs_property_name(p);
		if (obs_property_get_type(p) != OBS_PROPERTY_LIST) {
			dstr_catf(&out, "%s\t%s\n", key, obs_property_description(p));
			continue;
		}
		const char *value = obs_data_get_string(settings, key);
		const char *shown = "";
		struct dstr choices = {0};
		for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
			if (strcmp(obs_property_list_item_string(p, i), value) == 0)
				shown = obs_property_list_item_name(p, i);
			dstr_catf(&choices, "%s%s", i ? "|" : "", obs_property_list_item_name(p, i));
		}
		dstr_catf(&out, "%s\t%s\t%s\t%s\n", key, obs_property_description(p), shown, choices.array ? choices.array : "");
		dstr_free(&choices);
	}
	calldata_set_string(cd, "rows", out.array ? out.array : "");
	dstr_free(&out);
	obs_data_release(settings);
	obs_properties_destroy(props);
}

// value: the visible text of the choice to pick (matched from its start).
static void proc_dialog_set(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	const char *key = calldata_string(cd, "key"), *want = calldata_string(cd, "choice");
	obs_properties_t *props = obs_source_properties(settings_source);
	obs_property_t *p = obs_properties_get(props, key);
	obs_data_t *change = NULL;
	for (size_t i = 0; p && want && i < obs_property_list_item_count(p) && !change; i++) {
		if (strncmp(obs_property_list_item_name(p, i), want, strlen(want)) != 0)
			continue;
		change = obs_data_create();
		obs_data_set_string(change, key, obs_property_list_item_string(p, i));
	}
	obs_properties_destroy(props);
	if (change)
		obs_source_update(settings_source, change);
	calldata_set_bool(cd, "ok", change != NULL);
	obs_data_release(change);
}

static void proc_status(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	struct dstr text = {0};
	status_text(&text);
	calldata_set_string(cd, "status", text.array);
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
	proc_handler_add(ph, "void faders_configure(in string server, in string key, in string faders, in bool only_live)", proc_configure, NULL);
	proc_handler_add(ph, "void faders_assign(in string source, in string fader)", proc_assign, NULL);
	proc_handler_add(ph, "void faders_assign_track(in int track, in string fader)", proc_assign_track, NULL);
	proc_handler_add(ph, "void faders_status(out string status)", proc_status, NULL);
	proc_handler_add(ph, "void faders_dialog(out string rows)", proc_dialog, NULL);
	proc_handler_add(ph, "void faders_dialog_set(in string key, in string choice, out bool ok)", proc_dialog_set, NULL);
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
