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
#include <stdlib.h>
#include <time.h>
#include "uplink.h"
#include "mixer.h"
#include "placement.h"
#include "split.h"
#include "splitmix.h"
#include "logring.h"
#include "report.h"
#include "sysinfo.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("faders", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Faders: let viewers set their own levels for your voice, game, music and more.";
}

#define SOURCE_ID "faders_uplink"
#define PORTAL_URL "https://faders.tv/portal/"
#define PORTAL_KEYS_URL "https://faders.tv/portal/#plugin-keys"
#define DOWNLOAD_URL "https://faders.tv/streamers/#download"
#define DOCS_URL "https://faders.tv/docs/faders-sources/"
#define SETUP_URL "https://faders.tv/docs/streamer-setup/"
#define SUPPORT_URL "https://faders.tv/support/"
// Production hub. The key alone identifies the channel, so streamers only paste their key. The
// server isn't shown in the settings; tests point it elsewhere through faders_configure.
#define DEFAULT_SERVER "wss://hub.faders.tv/ingest"
// Settings keys (settings.json in the plugin's config folder).
#define S_FADERS "faders" // editable list: [{"value":"Voice"},…]
// 0.4.0 mapped OBS tracks to faders ("track:<1-6>"). Tracks decide nothing now; the keys are erased.
#define LEGACY_TRACK_PREFIX "track:"
// Set once Tools → Faders has been opened on a first start (see open_on_first_start).
#define S_FIRST_START "first_start_done"

static obs_source_t *settings_source; // private, never shown in a scene
static fdr_uplink_t *uplink;
static fdr_mixer_t *mixer;
static pthread_mutex_t uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static bool streaming;
static bool frontend_loaded;
static uint64_t loaded_ns, stream_started_ns; // for the debug report's uptimes
static int forced_stream_track; // headless tests: faders_watch_stream_track
static int forced_transition_ms; // headless tests: faders_transition_duration
static int max_stems_seen = FDR_MAX_FADERS; // the mixer's cap: none until the hub says
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

// A fader name as it goes on the wire (PROTOCOL.md): no control characters, no outer spaces, at
// most 24 characters the way the hub counts them (UTF-16 units, so an emoji is two), cut between
// characters, never inside one.
#define NAME_UNITS 24
static void clean_name(const char *v, char out[FDR_STEM_NAME_MAX])
{
	while (*v == ' ' || (*v && (unsigned char)*v < 0x20))
		v++;
	size_t len = 0;
	int units = 0;
	while (*v) {
		unsigned char c = (unsigned char)*v;
		size_t bytes = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
		bool whole = bytes > 0;
		for (size_t i = 1; i < bytes && whole; i++)
			whole = ((unsigned char)v[i] & 0xc0) == 0x80;
		if (!whole) { // not UTF-8: skip the byte
			v++;
			continue;
		}
		if (c < 0x20) { // control characters are dropped
			v++;
			continue;
		}
		int u = bytes == 4 ? 2 : 1;
		if (units + u > NAME_UNITS || len + bytes >= FDR_STEM_NAME_MAX)
			break;
		memcpy(out + len, v, bytes);
		len += bytes;
		units += u;
		v += bytes;
	}
	while (len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = 0;
}

static int fader_names(obs_data_t *s, char names[][FDR_STEM_NAME_MAX], int max)
{
	obs_data_array_t *arr = obs_data_get_array(s, S_FADERS);
	int n = 0;
	size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count && n < max; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *v = obs_data_get_string(item, "value");
		// Trim, cap the length, skip blanks and duplicates (names identify faders).
		char tmp[FDR_STEM_NAME_MAX];
		clean_name(v, tmp);
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

// The fader list as of the last update (main thread), to tell a rename from other edits.
static char last_names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
static int n_last_names = -1;

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
	// The split fader (split.h) first: set_faders rebuilds, which applies it.
	int track = (int)obs_data_get_int(s, FDR_SPLIT_TRACK_KEY);
	fdr_splitmix_set(fdr_mixer_split(mixer), obs_data_get_string(s, FDR_SPLIT_FADER_KEY), track);
	fdr_mixer_set_faders(mixer, ptrs, n, max_stems_seen);
}

// 0.4.0's track-to-fader settings are erased, never read: OBS ticks every new source for every
// track, so a track on a fader placed sources the streamer never chose. Whether any were there.
static bool erase_legacy_tracks(obs_data_t *s)
{
	bool any = false;
	for (int t = 1; t <= 6; t++) {
		char key[16];
		snprintf(key, sizeof(key), LEGACY_TRACK_PREFIX "%d", t);
		if (obs_data_has_user_value(s, key) || obs_data_has_default_value(s, key)) {
			obs_data_erase(s, key);
			any = true;
		}
	}
	if (any)
		fdr_log(LOG_INFO, "removed the old OBS track settings: tracks don't decide faders any more");
	return any;
}

// What the running uplink was started with (locked by uplink_mu).
static struct dstr run_server, run_key;

// Locked. The running uplink and mixer are what these settings ask for: only the fader list and
// the assignments can differ, and those are applied without a restart.
static bool running_as(obs_data_t *s)
{
	return uplink && mixer && should_run(s) && run_server.array && run_key.array && strcmp(run_server.array, obs_data_get_string(s, "server")) == 0 &&
	       strcmp(run_key.array, obs_data_get_string(s, "key")) == 0;
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
		dstr_copy(&run_server, cfg.server);
		dstr_copy(&run_key, cfg.key);
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

// The end of a key: enough to tell whether the remembered account belongs to the key in the
// field, without keeping a second copy of the key.
static const char *key_tail(const char *key)
{
	size_t n = strlen(key);
	return n > 6 ? key + n - 6 : key;
}

// ── How the window looks ────────────────────────────────────────────────────────────────
// OBS draws info rows as Qt labels, which render a subset of HTML: the status is a tinted card,
// problems are callouts with a coloured edge, help text is muted. Colours follow OBS's theme.

enum tone { TONE_NEUTRAL, TONE_OK, TONE_WARN, TONE_ERROR, TONE_ACCENT };

struct look {
	const char *muted;
	const char *edge[5]; // per tone: the dot and the callout's edge
	const char *fill[5]; // per tone: the card's background
	const char *live;    // the LIVE badge
};

static const struct look dark_look = {
	.muted = "#a4a9b6",
	.edge = {"#8a91a3", "#3ccf7c", "#f0b232", "#ff5c6c", "#8f7bff"},
	.fill = {"#2a2d37", "#1d3329", "#3a3020", "#3e2026", "#272645"},
	.live = "#ff4d8d",
};
static const struct look light_look = {
	.muted = "#5c6270",
	.edge = {"#8a91a3", "#1f9d55", "#c98a00", "#d6303c", "#6a4cff"},
	.fill = {"#eef0f4", "#e5f5eb", "#fcf1d9", "#fde6e8", "#ece8ff"},
	.live = "#e0306f",
};

static const struct look *look(void)
{
	return obs_frontend_is_theme_dark() ? &dark_look : &light_look;
}

// Text for an HTML label: names come from the streamer and the hub.
static void cat_escaped(struct dstr *out, const char *s)
{
	for (; s && *s; s++) {
		if (*s == '&')
			dstr_cat(out, "&amp;");
		else if (*s == '<')
			dstr_cat(out, "&lt;");
		else if (*s == '>')
			dstr_cat(out, "&gt;");
		else
			dstr_ncat(out, s, 1);
	}
}

// A callout: a coloured edge, then a bold title over a body (HTML).
static void callout_html(struct dstr *out, enum tone tone, const char *title_html, const char *body_html)
{
	const struct look *l = look();
	dstr_catf(out,
		  "<table width=\"100%%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
		  "<td width=\"4\" bgcolor=\"%s\"></td>"
		  "<td bgcolor=\"%s\" style=\"padding: 9px 12px;\"><b>%s</b>",
		  l->edge[tone], l->fill[tone], title_html);
	if (body_html && *body_html)
		dstr_catf(out, "<br>%s", body_html);
	dstr_cat(out, "</td></tr></table>");
}

// Line breaks every `width` visible characters or so, at a space outside a tag. OBS's form layout
// sizes a word-wrapped label for its narrowest width and leaves a tall gap around it, so labels
// don't wrap themselves: the text is broken here instead.
static void break_lines(const char *html, int width, struct dstr *out)
{
	int col = 0;
	bool tag = false;
	for (const char *p = html; *p; p++) {
		if (*p == '<') {
			tag = true;
			if (!strncmp(p, "<br", 3) || !strncmp(p, "<tr", 3) || !strncmp(p, "<td", 3))
				col = 0;
		}
		if (tag) {
			dstr_ncat(out, p, 1);
			tag = *p != '>';
			continue;
		}
		if (*p == ' ' && col >= width) {
			dstr_cat(out, "<br>");
			col = 0;
			continue;
		}
		if (*p == '&') { // an entity is one character
			const char *semi = strchr(p, ';');
			if (semi && semi - p < 8) {
				dstr_ncat(out, p, semi - p + 1);
				p = semi;
				col++;
				continue;
			}
		}
		dstr_ncat(out, p, 1);
		if (((unsigned char)*p & 0xc0) != 0x80) // count characters, not UTF-8 bytes
			col++;
	}
}

#define LINE_CHARS 84

static obs_property_t *add_html(obs_properties_t *props, const char *name, const char *html)
{
	struct dstr text = {0};
	break_lines(html, LINE_CHARS, &text);
	obs_property_t *p = obs_properties_add_text(props, name, text.array, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, false); // on by default
	dstr_free(&text);
	return p;
}

static obs_property_t *add_callout(obs_properties_t *props, const char *name, enum tone tone, const char *title_html, const char *body_html)
{
	struct dstr html = {0};
	callout_html(&html, tone, title_html, body_html);
	obs_property_t *p = add_html(props, name, html.array);
	dstr_free(&html);
	return p;
}

// Muted help text under a control.
static obs_property_t *add_hint(obs_properties_t *props, const char *name, const char *html)
{
	struct dstr text = {0};
	dstr_printf(&text, "<span style=\"color: %s;\">%s</span>", look()->muted, html);
	obs_property_t *p = add_html(props, name, text.array);
	dstr_free(&text);
	return p;
}

// ── Status ──────────────────────────────────────────────────────────────────────────────
// One description of where things stand, shown as the card at the top of the window (and as
// plain text by faders_status for the tests). Problems the stream has get their own callouts
// below the card, which then only points at them.

struct status_view {
	enum tone tone;
	struct dstr title;  // plain text
	struct dstr detail; // HTML
	struct dstr meta;   // HTML, muted: plan, faders, platforms
	struct dstr fine;   // plain text, small: the technical reason, for support
	bool live;
};

static void status_view_free(struct status_view *v)
{
	dstr_free(&v->title);
	dstr_free(&v->detail);
	dstr_free(&v->meta);
	dstr_free(&v->fine);
}

static const char *plan_label(const char *plan)
{
	if (!plan || !*plan)
		return NULL;
	if (!strcmp(plan, "free"))
		return "Free";
	if (!strcmp(plan, "pro"))
		return "Pro";
	if (!strcmp(plan, "partner"))
		return "Partner";
	return NULL;
}

// The stream's problems the plugin can see, each shown as a callout (see src_properties).
static int problem_count(const fdr_uplink_status *st)
{
	return (st->stream_track_silent ? 1 : 0) + (st->mix_mismatch[0] ? 1 : 0) + (st->mix_duplicate[0] ? 1 : 0);
}

static void describe_status(obs_data_t *s, const fdr_uplink_status *st, struct status_view *v)
{
	memset(v, 0, sizeof(*v));
	v->live = streaming;
	const char *account = obs_data_get_string(s, "account");
	bool remembered = *account && strcmp(obs_data_get_string(s, "account_key"), key_tail(obs_data_get_string(s, "key"))) == 0;
	if (mixer_unavailable) {
		v->tone = TONE_ERROR;
		dstr_copy(&v->title, "Faders can't run");
		dstr_copy(&v->detail, "It needs stereo, 48 kHz audio. Change it in OBS Settings → Audio, then restart OBS.");
		return;
	}
	if (!obs_data_get_bool(s, "enabled")) {
		v->tone = TONE_NEUTRAL;
		dstr_copy(&v->title, "Faders is off");
		dstr_copy(&v->detail, "Viewers can't mix your stream. Tick <b>Send audio to Faders</b> to turn it on.");
		return;
	}
	if (!*obs_data_get_string(s, "key")) {
		v->tone = TONE_ACCENT;
		dstr_copy(&v->title, "Add your plugin key");
		dstr_copy(&v->detail, "<a href=\"" PORTAL_KEYS_URL "\">Create a plugin key</a> in the Faders portal, then paste it below.");
		return;
	}
	if (!st->running && streaming) { // should be running: the uplink didn't start
		v->tone = TONE_ERROR;
		dstr_copy(&v->title, "Faders didn't start");
		dstr_copy(&v->detail, "Look for [faders] lines in Help → Log Files → View Current Log.");
		return;
	}
	if (!st->running) {
		v->tone = TONE_NEUTRAL;
		dstr_copy(&v->title, "Ready");
		if (remembered) {
			dstr_copy(&v->detail, "Connects as <b>");
			cat_escaped(&v->detail, account);
			dstr_cat(&v->detail, "</b> when you go live.");
		} else {
			dstr_copy(&v->detail, "Connects when you go live.");
		}
		return;
	}
	if (!st->connected) {
		int code = st->rejected_status;
		if (code == 401 || code == 403) {
			v->tone = TONE_ERROR;
			dstr_copy(&v->title, "Plugin key not accepted");
			dstr_copy(&v->detail, "It may have been revoked, or only part of it was pasted. <a href=\"" PORTAL_KEYS_URL "\">Create a new key</a> and paste it below.");
		} else if (code == 429) {
			v->tone = TONE_WARN;
			dstr_copy(&v->title, "Too many connection attempts");
			dstr_copy(&v->detail, "Faders will try again shortly.");
		} else if (st->failure[0]) {
			v->tone = TONE_WARN;
			dstr_copy(&v->title, "Can't reach Faders");
			dstr_copy(&v->detail, "Retrying automatically. If this doesn't clear, check your internet connection or firewall.");
			dstr_copy(&v->fine, st->failure);
		} else {
			v->tone = TONE_NEUTRAL;
			dstr_copy(&v->title, "Connecting…");
		}
		return;
	}

	int problems = problem_count(st);
	v->tone = st->stream_track_silent ? TONE_ERROR : problems ? TONE_WARN : TONE_OK;
	const char *who = st->channel[0] ? st->channel : remembered ? account : NULL;
	if (who) {
		dstr_copy(&v->title, "Connected as ");
		dstr_cat(&v->title, who);
	} else {
		dstr_copy(&v->title, "Connected");
	}
	if (problems)
		dstr_printf(&v->detail, "%s needs your attention below.", problems == 1 ? "One thing" : problems == 2 ? "Two things" : "Three things");
	else if (st->listeners == 0)
		dstr_copy(&v->detail, "No one is mixing yet. Viewers start by clicking <b>Mix this stream</b> on your channel.");
	else if (st->listener_limit > 0 && st->listeners >= st->listener_limit)
		dstr_printf(&v->detail, "<b>%d of %d</b> viewers are mixing: your plan's limit. <a href=\"" PORTAL_URL "\">More with Pro</a>", st->listeners, st->listener_limit);
	else if (st->listener_limit > 0)
		dstr_printf(&v->detail, "<b>%d</b> of %d viewers are mixing.", st->listeners, st->listener_limit);
	else
		dstr_printf(&v->detail, "<b>%d</b> viewer%s mixing.", st->listeners, st->listeners == 1 ? " is" : "s are");

	const char *plan = plan_label(st->plan);
	if (plan)
		dstr_catf(&v->meta, "%s plan", plan);
	if (st->stems > 0) // a split fader is one fader with two more layers
		dstr_catf(&v->meta, "%s%d faders + Everything else", v->meta.len ? " · " : "", st->stems - (st->split ? 1 + FDR_SPLIT_EXTRA : 1));
	if (st->platforms[0])
		dstr_catf(&v->meta, "%s%s", v->meta.len ? " · " : "",
			  !strcmp(st->platforms, "twitch,youtube") ? "Twitch and YouTube" : !strcmp(st->platforms, "youtube") ? "YouTube" : "Twitch");
}

// The card at the top of the window.
static void status_card_html(const struct status_view *v, struct dstr *out)
{
	const struct look *l = look();
	dstr_catf(out, "<table width=\"100%%\" cellspacing=\"0\" cellpadding=\"0\" bgcolor=\"%s\"><tr>", l->fill[v->tone]);
	char *logo = obs_module_file("faders-mark.png");
	if (logo) {
		dstr_catf(out, "<td width=\"40\" valign=\"middle\" style=\"padding: 14px 0 14px 14px;\"><img src=\"%s\" width=\"32\" height=\"32\"></td>", logo);
		bfree(logo);
	}
	dstr_cat(out, "<td valign=\"middle\" style=\"padding: 14px 16px;\">");
	dstr_catf(out, "<span style=\"color: %s;\">●</span>&nbsp;<span style=\"font-size: 13pt; font-weight: 600;\">", l->edge[v->tone]);
	cat_escaped(out, v->title.array);
	dstr_cat(out, "</span>");
	if (v->live)
		dstr_catf(out, "&nbsp;&nbsp;<span style=\"background-color: %s; color: #ffffff; font-size: 8pt; font-weight: 700;\">&nbsp;LIVE&nbsp;</span>", l->live);
	if (v->detail.len)
		dstr_catf(out, "<br>%s", v->detail.array);
	if (v->meta.len)
		dstr_catf(out, "<br><span style=\"color: %s; font-size: small;\">%s</span>", l->muted, v->meta.array);
	if (v->fine.len) {
		dstr_catf(out, "<br><span style=\"color: %s; font-size: small;\">", l->muted);
		cat_escaped(out, v->fine.array);
		dstr_cat(out, "</span>");
	}
	dstr_cat(out, "</td></tr></table>");
}

// HTML → text, for faders_status (the tests read it).
static void strip_tags(const char *html, struct dstr *out)
{
	bool tag = false;
	for (const char *p = html; p && *p; p++) {
		if (*p == '<') {
			tag = true;
			if (!strncmp(p, "<br", 3))
				dstr_cat(out, " ");
		}
		else if (*p == '>')
			tag = false;
		else if (!tag && !strncmp(p, "&amp;", 5))
			dstr_cat(out, "&"), p += 4;
		else if (!tag && !strncmp(p, "&lt;", 4))
			dstr_cat(out, "<"), p += 3;
		else if (!tag && !strncmp(p, "&gt;", 4))
			dstr_cat(out, ">"), p += 3;
		else if (!tag && !strncmp(p, "&nbsp;", 6))
			dstr_cat(out, " "), p += 5;
		else if (!tag)
			dstr_ncat(out, p, 1);
	}
}

static void status_text(struct dstr *out)
{
	fdr_uplink_status st;
	get_status(&st);
	obs_data_t *s = settings_source ? obs_source_get_settings(settings_source) : obs_data_create();
	struct status_view v;
	describe_status(s, &st, &v);
	dstr_copy(out, v.title.array);
	if (v.detail.len) {
		dstr_cat(out, ". ");
		strip_tags(v.detail.array, out);
	}
	if (v.meta.len) {
		dstr_cat(out, " (");
		strip_tags(v.meta.array, out);
		dstr_cat(out, ")");
	}
	if (st.connected && st.listeners > 0)
		dstr_catf(out, " %d kbps.", st.uplink_kbps);
	if (v.fine.len)
		dstr_catf(out, " [%s]", v.fine.array);
	status_view_free(&v);
	obs_data_release(s);
}

// ── Keeping an open window current ──────────────────────────────────────────────────────
// The window is rebuilt from scratch whenever OBS is asked to (obs_source_update_properties), so
// a cheap fingerprint of what the status shows is checked twice a second from the tick, and the
// window is rebuilt when it changes (at most every 2 s: a busy channel's listener count moves).

static uint64_t status_fingerprint(const fdr_uplink_status *st)
{
	uint64_t h = 1469598103934665603ULL;
#define MIX(x) (h = (h ^ (uint64_t)(x)) * 1099511628211ULL)
	MIX(st->running);
	MIX(st->connected);
	MIX(st->listeners);
	MIX(st->failure[0] != 0);
	MIX(st->rejected_status);
	MIX(st->stream_track_silent);
	MIX(st->stream_track);
	MIX(st->stems);
	MIX(st->max_stems);
	MIX(st->listener_limit);
	MIX(st->split);
	MIX(st->split_warnings);
	MIX(streaming);
	MIX(mixer_unavailable);
	for (const char *p = st->mix_mismatch; *p; p++)
		MIX(*p);
	for (const char *p = st->mix_duplicate; *p; p++)
		MIX(*p);
	for (const char *p = st->channel; *p; p++)
		MIX(*p);
	for (const char *p = st->plan; *p; p++)
		MIX(*p);
	for (const char *p = st->platforms; *p; p++)
		MIX(*p);
#undef MIX
	return h;
}

static uint64_t shown_fingerprint;
static uint64_t last_check_ns, last_refresh_ns;
static bool refresh_queued;

// A change made in the window itself (a dropdown, the fader list) needs new rows. OBS applies
// that change by calling src_update from inside the widget's own signal, on the UI thread. Asking
// for the properties again right there makes OBS rebuild the window on the spot, deleting the
// widget whose signal is still running: OBS then crashes on its way back out (picking "Remove
// from …" crashed OBS 33 this way). So src_update only raises this flag; the next graphics tick
// queues the redraw onto the UI thread, which runs it once the click has been handled. (A UI
// task queued from the UI thread itself would run immediately, so it has to come from another
// thread.)
static volatile bool redraw_wanted;
static long dialog_redraws; // test hook: update_properties signals seen

static void redraw_window(void *unused)
{
	UNUSED_PARAMETER(unused);
	if (settings_source)
		obs_source_update_properties(settings_source);
}

static void count_redraw(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(cd);
	os_atomic_inc_long(&dialog_redraws);
}

// An open window lists every audio input, so it is redrawn when one is created, renamed or
// removed. libobs sends those signals from whichever thread made the change, often while holding
// its own locks, so the handler only raises a flag. The graphics tick turns it into at most one
// redraw every 250 ms (loading a scene collection creates hundreds of sources), and none while a
// scene collection is being switched: one redraw follows the switch.
#define SOURCES_REDRAW_NS 250000000ULL
#define COLLECTION_SWITCH_MAX_NS 10000000000ULL // a switch we never saw end stops holding redraws back
static volatile bool sources_changed;
static volatile bool collection_switching;
static uint64_t collection_switch_ns, sources_redraw_ns;
static const char *const SOURCE_SIGNALS[] = {"source_create", "source_rename", "source_remove", "source_destroy", NULL};

static bool is_audio_input(obs_source_t *s)
{
	uint32_t flags = s ? obs_source_get_output_flags(s) : 0;
	return (flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_COMPOSITE) && obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT;
}

static void on_source_signal(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	if (is_audio_input(calldata_ptr(cd, "source")))
		os_atomic_set_bool(&sources_changed, true);
}

static void connect_source_signals(bool on)
{
	signal_handler_t *sh = obs_get_signal_handler();
	for (const char *const *sig = SOURCE_SIGNALS; *sig; sig++) {
		if (on)
			signal_handler_connect(sh, *sig, on_source_signal, NULL);
		else
			signal_handler_disconnect(sh, *sig, on_source_signal, NULL);
	}
}

// UI thread: remember who the key belongs to (so the window can say so while offline), then
// redraw an open window.
static void refresh_window(void *unused)
{
	UNUSED_PARAMETER(unused);
	refresh_queued = false;
	if (!settings_source)
		return;
	fdr_uplink_status st;
	get_status(&st);
	obs_data_t *s = obs_source_get_settings(settings_source);
	const char *tail = key_tail(obs_data_get_string(s, "key"));
	if (st.connected && st.channel[0] &&
	    (strcmp(obs_data_get_string(s, "account"), st.channel) != 0 || strcmp(obs_data_get_string(s, "account_key"), tail) != 0 ||
	     (st.max_stems > 0 && obs_data_get_int(s, "account_faders") != st.max_stems) || strcmp(obs_data_get_string(s, "account_plan"), st.plan) != 0 ||
	     strcmp(obs_data_get_string(s, "account_platforms"), st.platforms) != 0)) {
		obs_data_set_string(s, "account", st.channel);
		obs_data_set_string(s, "account_key", tail);
		obs_data_set_string(s, "account_plan", st.plan);
		obs_data_set_string(s, "account_platforms", st.platforms);
		if (st.max_stems > 0)
			obs_data_set_int(s, "account_faders", st.max_stems);
		save_settings(s);
	}
	obs_data_release(s);
	obs_source_update_properties(settings_source);
}

static void watch_status(void *unused, float seconds)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(seconds);
	uint64_t now = os_gettime_ns();
	bool redraw = os_atomic_exchange_bool(&redraw_wanted, false);
	if (os_atomic_load_bool(&collection_switching) && now - collection_switch_ns > COLLECTION_SWITCH_MAX_NS)
		os_atomic_set_bool(&collection_switching, false);
	if (!os_atomic_load_bool(&collection_switching) && now - sources_redraw_ns >= SOURCES_REDRAW_NS && os_atomic_exchange_bool(&sources_changed, false)) {
		sources_redraw_ns = now;
		redraw = true;
	}
	if (redraw)
		obs_queue_task(OBS_TASK_UI, redraw_window, NULL, false);
	if (now - last_check_ns < 500000000ULL || refresh_queued)
		return;
	last_check_ns = now;
	// Never wait on the lock here (this is the graphics thread; starting or stopping holds it).
	if (pthread_mutex_trylock(&uplink_mu) != 0)
		return;
	fdr_uplink_status st;
	fdr_uplink_get_status(uplink, &st);
	pthread_mutex_unlock(&uplink_mu);
	uint64_t fp = status_fingerprint(&st);
	if (fp == shown_fingerprint || now - last_refresh_ns < 2000000000ULL)
		return;
	shown_fingerprint = fp;
	last_refresh_ns = now;
	refresh_queued = true;
	obs_queue_task(OBS_TASK_UI, refresh_window, NULL, false);
}

// ── Settings source (properties window) ─────────────────────────────────────────────────

static const char *src_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Faders";
}

static void *src_create(obs_data_t *settings, obs_source_t *source)
{
	if (erase_legacy_tracks(settings))
		save_settings(settings);
	n_last_names = fader_names(settings, last_names, FDR_MAX_FADERS);
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
	// Names only: the faders start empty. Nothing is put on them for the streamer.
	static const char *const defaults[] = {"Voice", "Game", "Music"};
	obs_data_array_t *arr = names_array(defaults, 3);
	obs_data_set_default_array(s, S_FADERS, arr);
	obs_data_array_release(arr);
	// No split fader, no YouTube track until the streamer picks one (split.h).
	obs_data_set_default_string(s, FDR_SPLIT_FADER_KEY, "");
	obs_data_set_default_int(s, FDR_SPLIT_TRACK_KEY, 0);
}

// ── The dialog: one block per fader, with a dropdown for each source on it ─────────────
// Rows are rebuilt from what's true (each source's private setting) whenever the dialog opens or
// a placement changes; the table below remembers what each dropdown showed, so an update can
// tell which one the streamer changed. A source that was never placed is NEW: it is listed under
// Everything else and offered in every open fader's Add list. Nothing is placed for the streamer.
//
// The dropdowns' values are stored as defaults, never as user values: OBS's copy of the settings
// for Cancel / Discard / Undo (obs_data_apply) and settings.json (obs_data_get_json) only hold
// user values, so a dropdown as it was drawn can't come back as a pick, and none reaches disk. A
// pick is the user value OBS sets on top; apply_dialog_changes consumes it.
#define ROW_PREFIX "row:" // "row:<n>": one source on a fader
#define ADD_PREFIX "add:" // "add:<fader index>": the "Add a source…" dropdown
#define ITEM_SRC "s:"     // dropdown values: "s:<source uuid>"
#define CHOICE_REMOVE "~remove"
#define SPLIT_PREFIX "split:"     // "split:<fader index>": "Two sources: one per platform"
#define TWITCH_PREFIX "twitch:"   // "twitch:<fader index>": the split fader's Twitch source
#define YOUTUBE_PREFIX "youtube:" // "youtube:<fader index>": its YouTube source
#define CHOICE_NONE "~none"
// Every input the mixer can route, plus one add row per fader.
#define MAX_ITEMS FDR_MAX_AUDIO_SOURCES
#define MAX_DIALOG_ROWS (MAX_ITEMS + 3 * FDR_MAX_FADERS)

struct dialog_item {
	char id[80];
	char label[96];
	enum fdr_saved saved;
	char saved_name[FDR_STEM_NAME_MAX]; // the fader it was put on (also when that is gone)
	enum fdr_state state;
	int fader; // index of the fader it has a row under (on it, or locked on it), or -1
	enum fdr_split_role role; // on the split fader: which part it plays (split.h)
};

struct split_view;
struct dialog_model {
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n;
	int open; // faders within the plan's allowance (n when it isn't known)
	struct dialog_item items[MAX_ITEMS];
	int n_items;
	int n_new;
	int n_unmixed;        // audio inputs past what the mixer takes
	struct dstr unmixed;  // their names, ", "-joined (HTML)
	obs_data_t *settings;
	const struct split_view *split; // the split fader as it is now
	int tw_item, yt_item;           // its chosen sources' items, or -1
};

enum row_kind { ROW_SOURCE, ROW_ADD, ROW_SPLIT, ROW_TWITCH, ROW_YOUTUBE };
struct dialog_row {
	char key[24];
	char fader[FDR_STEM_NAME_MAX];
	char item[80]; // "" for an add dropdown or an empty pick; "1"/"0" for the split checkbox
	enum row_kind kind;
};
static struct dialog_row dialog_rows[MAX_DIALOG_ROWS];
static int n_dialog_rows;
static char dialog_names[FDR_MAX_FADERS * FDR_STEM_NAME_MAX]; // the fader list the rows were built for
// The last move that was refused, said in the window until the next one goes through (plain text).
static struct dstr move_feedback;

// The plan's allowance: from the hub while connected, else as last seen for this key; 0 = not
// known. *plan: its name for the window, or NULL.
static int known_allowance(obs_data_t *settings, const fdr_uplink_status *st, const char **plan)
{
	int allowance = st->max_stems > 0 ? st->max_stems : 0;
	const char *p = plan_label(st->plan);
	if (!allowance && !strcmp(obs_data_get_string(settings, "account_key"), key_tail(obs_data_get_string(settings, "key")))) {
		allowance = (int)obs_data_get_int(settings, "account_faders");
		p = plan_label(obs_data_get_string(settings, "account_plan"));
	}
	if (plan)
		*plan = p;
	return allowance;
}

// The faders as the settings have them now, and how many the plan opens (for checking a move).
struct fader_set {
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	const char *ptrs[FDR_MAX_FADERS];
	struct fdr_fader_list list;
};
static void current_faders(obs_data_t *settings, struct fader_set *f)
{
	fdr_uplink_status st;
	get_status(&st);
	int n = fader_names(settings, f->names, FDR_MAX_FADERS);
	for (int i = 0; i < n; i++)
		f->ptrs[i] = f->names[i];
	int allowance = known_allowance(settings, &st, NULL);
	f->list = (struct fdr_fader_list){f->ptrs, n, allowance > 0 && allowance < n ? allowance : n};
}

// A source's saved placement (its private setting); name gets the fader, if any.
static enum fdr_saved saved_placement(obs_source_t *s, char name[FDR_STEM_NAME_MAX])
{
	obs_data_t *priv = obs_source_get_private_settings(s);
	bool has = obs_data_has_user_value(priv, FDR_FADER_SETTING);
	snprintf(name, FDR_STEM_NAME_MAX, "%s", has ? obs_data_get_string(priv, FDR_FADER_SETTING) : "");
	obs_data_release(priv);
	return !has ? FDR_SAVED_NEW : *name ? FDR_SAVED_FADER : FDR_SAVED_REST;
}

// Where a source plays now: the fader's index (in f's list) or -1, and why (placement.h).
static int effective_placement(const struct fader_set *f, obs_source_t *s, enum fdr_saved *saved, char name[FDR_STEM_NAME_MAX], enum fdr_state *state)
{
	*saved = saved_placement(s, name);
	pthread_mutex_lock(&uplink_mu);
	bool dropped = fdr_mixer_is_dropped(mixer, s);
	pthread_mutex_unlock(&uplink_mu);
	return fdr_place_effective(&f->list, *saved, name, dropped, state);
}


// ── The split fader (split.h): the account, its sources, whether it is on ───────────────

// The account the window goes by: the hub's hello in this connection, else as remembered for
// this key (so it can be set up while offline).
struct account_view {
	char plan[24];
	struct fdr_account a;
};
static void known_account(obs_data_t *settings, const fdr_uplink_status *st, struct account_view *v)
{
	bool live = st->plan[0] != 0;
	bool remembered = !strcmp(obs_data_get_string(settings, "account_key"), key_tail(obs_data_get_string(settings, "key")));
	const char *plan = live ? st->plan : remembered ? obs_data_get_string(settings, "account_plan") : "";
	const char *platforms = live ? st->platforms : remembered ? obs_data_get_string(settings, "account_platforms") : "";
	snprintf(v->plan, sizeof(v->plan), "%s", plan);
	v->a = (struct fdr_account){v->plan, strstr(platforms, "twitch") != NULL, strstr(platforms, "youtube") != NULL};
}

static enum fdr_platform platform_of(obs_source_t *s)
{
	obs_data_t *priv = obs_source_get_private_settings(s);
	enum fdr_platform p = fdr_platform_parse(obs_data_get_string(priv, FDR_PLATFORM_SETTING));
	obs_data_release(priv);
	return p;
}
// Locked by the caller as a placement change: the source's platform tag (ALL erases it).
static void set_platform(obs_source_t *s, enum fdr_platform p)
{
	obs_data_t *priv = obs_source_get_private_settings(s);
	if (p == FDR_PLATFORM_ALL)
		obs_data_erase(priv, FDR_PLATFORM_SETTING);
	else
		obs_data_set_string(priv, FDR_PLATFORM_SETTING, fdr_platform_name(p));
	obs_data_release(priv);
}

// The split fader's sources: the first audio input of each platform saved on `fader` (as the
// mixer picks them, splitmix.c), and how many sources are saved on it in all.
struct split_sources {
	const char *fader;
	obs_source_t *tw, *yt; // references
	int on_fader;
	obs_source_t *only; // the one source on it (when on_fader == 1)
};
static bool find_split_cb(void *param, obs_source_t *s)
{
	struct split_sources *f = param;
	if (!is_audio_input(s))
		return true;
	char name[FDR_STEM_NAME_MAX];
	if (saved_placement(s, name) != FDR_SAVED_FADER || astrcmpi(name, f->fader) != 0)
		return true;
	f->on_fader++;
	f->only = s;
	enum fdr_platform p = platform_of(s);
	if (p == FDR_PLATFORM_TWITCH && !f->tw)
		f->tw = obs_source_get_ref(s);
	else if (p == FDR_PLATFORM_YOUTUBE && !f->yt)
		f->yt = obs_source_get_ref(s);
	return true;
}
static void find_split_sources(const char *fader, struct split_sources *out)
{
	memset(out, 0, sizeof(*out));
	out->fader = fader;
	if (fader && *fader)
		obs_enum_sources(find_split_cb, out);
	if (out->on_fader != 1)
		out->only = NULL;
}

// Everything the window and the procedures need to know about the split, now.
struct split_view {
	struct account_view acc;
	struct fader_set faders;
	char fader[FDR_STEM_NAME_MAX]; // saved, "" = none (or not one of the faders any more)
	int index;                     // its index, -1 = none
	int track;                     // YouTube's track, 0 = not picked
	struct split_sources src;
	struct fdr_split_state state;
	int stream_track; // Twitch's
};
static void split_view_get(obs_data_t *settings, struct split_view *v)
{
	fdr_uplink_status st;
	get_status(&st);
	known_account(settings, &st, &v->acc);
	current_faders(settings, &v->faders);
	v->index = fdr_place_find(&v->faders.list, obs_data_get_string(settings, FDR_SPLIT_FADER_KEY));
	snprintf(v->fader, sizeof(v->fader), "%s", v->index >= 0 ? v->faders.names[v->index] : "");
	int t = (int)obs_data_get_int(settings, FDR_SPLIT_TRACK_KEY);
	v->track = t >= 1 && t <= 6 ? t : 0;
	find_split_sources(v->fader, &v->src);
	struct fdr_split_input in = {&v->faders.list, v->fader, v->track, v->acc.a, v->src.tw != NULL, v->src.yt != NULL};
	fdr_split_evaluate(&in, &v->state);
	int stream = stream_audio_track();
	v->stream_track = stream ? stream : 1;
}
static void split_view_free(struct split_view *v)
{
	obs_source_release(v->src.tw);
	obs_source_release(v->src.yt);
	v->src.tw = v->src.yt = NULL;
}
// What a source does on the split fader (split.h): FDR_SPLIT_ROLE_NONE when it isn't on it.
static enum fdr_split_role split_role_of(const struct split_view *v, obs_source_t *s)
{
	char name[FDR_STEM_NAME_MAX];
	if (!v->fader[0] || saved_placement(s, name) != FDR_SAVED_FADER)
		return FDR_SPLIT_ROLE_NONE;
	enum fdr_platform p = platform_of(s);
	bool chosen = (p == FDR_PLATFORM_TWITCH && s == v->src.tw) || (p == FDR_PLATFORM_YOUTUBE && s == v->src.yt);
	return fdr_split_role(&v->state, v->fader, name, p, chosen);
}

// A name cut to fit a label, between characters, never inside one (OBS would show a broken
// character at the end of the row).
static void cut_label(char *label, size_t size, const char *name)
{
	snprintf(label, size, "%s", name);
	size_t len = strlen(label), start = len;
	while (start > 0 && ((unsigned char)label[start - 1] & 0xc0) == 0x80)
		start--;
	if (start > 0) {
		unsigned char lead = (unsigned char)label[start - 1];
		size_t need = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : 4;
		if (len - (start - 1) < need)
			label[start - 1] = 0;
	}
}

static bool add_dialog_source(void *param, obs_source_t *s)
{
	struct dialog_model *m = param;
	if (!is_audio_input(s))
		return true;
	// Past what the mixer takes, a source isn't mixed on its own and its audio stays in Everything
	// else, wherever it was put: it is named there, with no dropdown.
	if (m->n_items >= MAX_ITEMS) {
		dstr_cat(&m->unmixed, m->unmixed.len ? ", " : "");
		cat_escaped(&m->unmixed, obs_source_get_name(s));
		m->n_unmixed++;
		return true;
	}
	struct dialog_item *it = &m->items[m->n_items++];
	memset(it, 0, sizeof(*it));
	snprintf(it->id, sizeof(it->id), ITEM_SRC "%s", obs_source_get_uuid(s));
	cut_label(it->label, sizeof(it->label), obs_source_get_name(s));
	const char *ptrs[FDR_MAX_FADERS];
	for (int i = 0; i < m->n; i++)
		ptrs[i] = m->names[i];
	struct fdr_fader_list l = {ptrs, m->n, m->open};
	it->saved = saved_placement(s, it->saved_name);
	pthread_mutex_lock(&uplink_mu);
	bool dropped = fdr_mixer_is_dropped(mixer, s);
	pthread_mutex_unlock(&uplink_mu);
	it->fader = fdr_place_effective(&l, it->saved, it->saved_name, dropped, &it->state);
	// Rows show where it was put: a locked or dropped source stays under its fader (the plan
	// callout and the stream callouts say why it plays in Everything else).
	if (it->state == FDR_STATE_LOCKED || it->state == FDR_STATE_DROPPED)
		it->fader = fdr_place_find(&l, it->saved_name);
	if (it->state == FDR_STATE_NEW)
		m->n_new++;
	// The split fader shows its two sources in its own lists; the YouTube source while the
	// split is off, and anything else saved on it, play in Everything else.
	it->role = m->split ? split_role_of(m->split, s) : FDR_SPLIT_ROLE_NONE;
	if (it->role == FDR_SPLIT_ROLE_PARKED || it->role == FDR_SPLIT_ROLE_EXTRA)
		it->fader = -1;
	if (m->split && s == m->split->src.tw)
		m->tw_item = m->n_items - 1;
	if (m->split && s == m->split->src.yt)
		m->yt_item = m->n_items - 1;
	return true;
}

static void build_dialog_model(struct dialog_model *m, obs_data_t *settings, int allowance, const struct split_view *split)
{
	dstr_free(&m->unmixed);
	memset(m, 0, sizeof(*m));
	m->settings = settings;
	m->split = split;
	m->tw_item = m->yt_item = -1;
	m->n = fader_names(settings, m->names, FDR_MAX_FADERS);
	m->open = allowance > 0 && allowance < m->n ? allowance : m->n;
	obs_enum_sources(add_dialog_source, m);
}

// One dropdown. An existing row shows what it holds and offers to remove it. An open fader's
// "Add…" row offers what isn't on a fader (NEW sources marked so); a locked fader's offers nothing.
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
	row->kind = current ? ROW_SOURCE : ROW_ADD;
	n_dialog_rows++;
	bool locked = fader >= m->open;
	// The fader's name sits on the left of its first row only.
	char label[FDR_STEM_NAME_MAX + 32];
	snprintf(label, sizeof(label), "%s%s", m->names[fader], locked ? " (not in your plan)" : "");
	obs_property_t *p = obs_properties_add_list(group, row->key, first ? label : " ", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (current)
		obs_property_list_add_string(p, current->label, current->id);
	else
		obs_property_list_add_string(p, locked ? "Not in your plan: plays in Everything else" : "Add a source…", "");
	for (int i = 0; !current && !locked && i < m->n_items; i++) {
		const struct dialog_item *it = &m->items[i];
		if (it->fader >= 0 || it->role == FDR_SPLIT_ROLE_PARKED) // on a fader (or kept for the split fader)
			continue;
		char text[sizeof(it->label) + 8];
		snprintf(text, sizeof(text), "%s%s", it->label, it->state == FDR_STATE_NEW ? " (new)" : "");
		obs_property_list_add_string(p, text, it->id);
	}
	if (current) {
		char remove[FDR_STEM_NAME_MAX + 16];
		snprintf(remove, sizeof(remove), "Remove from %s", m->names[fader]);
		obs_property_list_add_string(p, remove, CHOICE_REMOVE);
	}
	obs_data_set_default_string(m->settings, row->key, row->item);
	obs_data_unset_user_value(m->settings, row->key);
}


static struct dialog_row *new_row(enum row_kind kind, const char *prefix, int fader, const char *name, const char *item)
{
	if (n_dialog_rows >= MAX_DIALOG_ROWS)
		return NULL;
	struct dialog_row *row = &dialog_rows[n_dialog_rows++];
	snprintf(row->key, sizeof(row->key), "%s%d", prefix, fader);
	snprintf(row->fader, sizeof(row->fader), "%s", name);
	snprintf(row->item, sizeof(row->item), "%s", item);
	row->kind = kind;
	return row;
}

// "Two sources: one per platform" under a fader: ticked on the split fader; on the others, only
// while no fader has it, the plan is Pro or Partner and YouTube is connected (split.h).
static void add_split_checkbox(obs_properties_t *group, struct dialog_model *m, int fader)
{
	const struct split_view *v = m->split;
	bool is_split = v->index == fader;
	struct dialog_row *row = new_row(ROW_SPLIT, SPLIT_PREFIX, fader, m->names[fader], is_split ? "1" : "0");
	if (!row)
		return;
	enum fdr_split_gate g = fdr_split_gate(&v->acc.a, &v->faders.list, v->fader, m->names[fader]);
	char label[160];
	if (is_split || g == FDR_SPLIT_GATE_OK)
		snprintf(label, sizeof(label), "Two sources: one per platform");
	else
		snprintf(label, sizeof(label), "Two sources: one per platform (%s)", fdr_split_gate_text(g));
	obs_property_t *p = obs_properties_add_bool(group, row->key, label);
	obs_property_set_long_description(p, "One source for Twitch and one for YouTube, for example different music on each. Viewers get a slider for each. Counts as 1 fader.");
	// The split fader's own box can always be unticked (also after a downgrade).
	obs_property_set_enabled(p, is_split || g == FDR_SPLIT_GATE_OK);
	obs_data_set_default_bool(m->settings, row->key, is_split);
	obs_data_unset_user_value(m->settings, row->key);
}

// One of the split fader's two source lists: (none), the source it has, and every source that
// isn't on a fader (NEW ones marked).
static void add_split_list(obs_properties_t *group, struct dialog_model *m, int fader, enum fdr_platform platform)
{
	bool tw = platform == FDR_PLATFORM_TWITCH;
	int cur = tw ? m->tw_item : m->yt_item, other = tw ? m->yt_item : m->tw_item;
	const struct dialog_item *current = cur >= 0 ? &m->items[cur] : NULL;
	struct dialog_row *row = new_row(tw ? ROW_TWITCH : ROW_YOUTUBE, tw ? TWITCH_PREFIX : YOUTUBE_PREFIX, fader, m->names[fader], current ? current->id : "");
	if (!row)
		return;
	char label[FDR_STEM_NAME_MAX + 32];
	snprintf(label, sizeof(label), "%s · %s source", m->names[fader], tw ? "Twitch" : "YouTube");
	obs_property_t *p = obs_properties_add_list(group, row->key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(p, tw ? "What Twitch viewers hear on this fader: the source in your Twitch stream's audio."
						: "What YouTube viewers hear on this fader: the source on the OBS track that goes to YouTube.");
	if (current)
		obs_property_list_add_string(p, current->label, current->id);
	obs_property_list_add_string(p, "(none)", current ? CHOICE_NONE : "");
	for (int i = 0; i < m->n_items; i++) {
		const struct dialog_item *it = &m->items[i];
		if (i == cur || i == other || it->fader >= 0 || it->role == FDR_SPLIT_ROLE_PARKED)
			continue;
		char text[sizeof(it->label) + 8];
		snprintf(text, sizeof(text), "%s%s", it->label, it->state == FDR_STATE_NEW ? " (new)" : "");
		obs_property_list_add_string(p, text, it->id);
	}
	obs_data_set_default_string(m->settings, row->key, row->item);
	obs_data_unset_user_value(m->settings, row->key);
}

// The split fader's block: its two lists, YouTube's track, the box, what it counts as and what
// still needs doing, and the track checks per platform.
static void add_split_block(obs_properties_t *group, struct dialog_model *m, int fader)
{
	const struct split_view *v = m->split;
	add_split_list(group, m, fader, FDR_PLATFORM_TWITCH);
	add_split_list(group, m, fader, FDR_PLATFORM_YOUTUBE);
	obs_property_t *t = obs_properties_add_list(group, FDR_SPLIT_TRACK_KEY, "Which OBS track goes to YouTube?", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(t, "The track your YouTube output sends, for example the one obs-multi-rtmp or Aitum uses for YouTube. Twitch keeps the track OBS streams.");
	obs_property_list_add_int(t, "Pick a track", 0);
	for (int i = 1; i <= 6; i++) {
		char name[16];
		snprintf(name, sizeof(name), "Track %d", i);
		obs_property_list_add_int(t, name, i);
	}
	add_split_checkbox(group, m, fader);

	struct dstr text = {0}, name = {0};
	cat_escaped(&name, m->names[fader]);
	dstr_printf(&text, "Counts as 1 fader. Viewers get two sliders: %s · Twitch and %s · YouTube, with their own platform's at 100%% and the other at 0%%.", name.array, name.array);
	switch (v->state.why) {
	case FDR_SPLIT_ON:
		break;
	case FDR_SPLIT_PLAN:
		dstr_cat(&text, "<br><b>Off for now:</b> it needs Pro or Partner. Your picks are kept, and the YouTube source plays in Everything else until then.");
		break;
	case FDR_SPLIT_NO_YOUTUBE:
		dstr_cat(&text, "<br><b>Off for now:</b> connect YouTube to your Faders account to use this. Your picks are kept.");
		break;
	case FDR_SPLIT_LOCKED:
		dstr_cat(&text, "<br><b>Off:</b> this fader isn't in your plan, so it plays in Everything else.");
		break;
	case FDR_SPLIT_TOO_MANY:
		dstr_catf(&text, "<br><b>Off:</b> with a fader split per platform, the stream holds %d faders at most.", FDR_SPLIT_STEMS_MAX - 1 - FDR_SPLIT_EXTRA);
		break;
	default: {
		dstr_cat(&text, "<br><b>To turn it on, pick</b> ");
		const char *missing[3];
		int n = 0;
		if (!v->src.tw)
			missing[n++] = "a Twitch source";
		if (!v->src.yt)
			missing[n++] = "a YouTube source";
		if (!v->track)
			missing[n++] = "the YouTube track";
		for (int i = 0; i < n; i++)
			dstr_catf(&text, "%s%s", i == 0 ? "" : i == n - 1 ? " and " : ", ", missing[i]);
		dstr_cat(&text, ".");
		break;
	}
	}
	add_hint(group, "split_info", text.array);

	// Each platform's source on its own track, and not on the other's.
	if (v->src.tw && v->src.yt && v->track) {
		unsigned w = fdr_split_track_warnings(obs_source_get_audio_mixers(v->src.tw), obs_source_get_audio_mixers(v->src.yt), v->stream_track, v->track);
		fdr_uplink_status st;
		get_status(&st);
		if (st.split_track == v->track)
			w |= st.split_warnings & (FDR_SPLIT_WARN_YOUTUBE_TRACK_SILENT | FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK);
		dstr_copy(&text, "");
		if (w & FDR_SPLIT_WARN_TWITCH_NOT_ON_TRACK)
			dstr_catf(&text, "%sTwitch source isn't on Track %d.", text.len ? "<br>" : "", v->stream_track);
		if (w & FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK)
			dstr_catf(&text, "%sYouTube source isn't on Track %d.", text.len ? "<br>" : "", v->track);
		if (w & FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK)
			dstr_catf(&text, "%sTwitch source is also on the YouTube track: YouTube viewers will hear it twice.", text.len ? "<br>" : "");
		if (w & FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK)
			dstr_catf(&text, "%sYouTube source is also on the Twitch track: Twitch viewers will hear it twice.", text.len ? "<br>" : "");
		if (w & FDR_SPLIT_WARN_YOUTUBE_TRACK_SILENT)
			dstr_catf(&text, "%sTrack %d, the one you picked for YouTube, is silent.", text.len ? "<br>" : "", v->track);
		if (text.len)
			add_callout(group, "split_status", TONE_WARN, "Check the tracks per platform", text.array);
	}
	dstr_free(&text);
	dstr_free(&name);
}

static void add_assignment_rows(obs_properties_t *group, obs_data_t *settings, int allowance)
{
	static struct dialog_model m; // large: off the stack (dialog code runs on the UI thread only)
	static struct split_view split;
	split_view_get(settings, &split);
	build_dialog_model(&m, settings, allowance, &split);
	if (move_feedback.len) {
		struct dstr text = {0};
		cat_escaped(&text, move_feedback.array);
		add_callout(group, "move_feedback", TONE_WARN, "That move didn't happen", text.array);
		dstr_free(&text);
	}
	if (m.n_new) {
		struct dstr title = {0};
		if (m.n_new == 1)
			dstr_copy(&title, "1 source isn't placed yet");
		else
			dstr_printf(&title, "%d sources aren't placed yet", m.n_new);
		add_callout(group, "new_info", TONE_NEUTRAL, title.array,
			    "Pick a fader for each one in that fader's <b>Add a source…</b> list. Anything you don't place plays in Everything else.");
		dstr_free(&title);
	}
	n_dialog_rows = 0;
	dialog_names[0] = 0;
	for (int f = 0; f < m.n; f++) {
		strncat(dialog_names, m.names[f], sizeof(dialog_names) - strlen(dialog_names) - 2);
		strcat(dialog_names, "\n");
		if (f == split.index) {
			add_split_block(group, &m, f);
			continue;
		}
		bool first = true;
		for (int i = 0; i < m.n_items; i++) {
			if (m.items[i].fader != f)
				continue;
			add_dialog_row(group, &m, f, &m.items[i], first);
			first = false;
		}
		add_dialog_row(group, &m, f, NULL, first);
		if (f < m.open)
			add_split_checkbox(group, &m, f);
	}
	// What's left over, by name, so nothing is a mystery: NEW sources marked, and a source whose
	// fader is gone says which one it was on (renaming a fader back brings it back).
	struct dstr rest = {0};
	for (int i = 0; i < m.n_items; i++) {
		const struct dialog_item *it = &m.items[i];
		if (it->fader >= 0)
			continue;
		dstr_cat(&rest, rest.len ? ", " : "");
		cat_escaped(&rest, it->label);
		if (it->state == FDR_STATE_NEW) {
			dstr_cat(&rest, " (new)");
		} else if (it->state == FDR_STATE_DELETED) {
			dstr_cat(&rest, " (its fader \"");
			cat_escaped(&rest, it->saved_name);
			dstr_cat(&rest, "\" is gone)");
		} else if (it->role == FDR_SPLIT_ROLE_PARKED) {
			dstr_cat(&rest, " (");
			cat_escaped(&rest, split.fader);
			dstr_cat(&rest, "'s YouTube source, until it's on)");
		} else if (it->role == FDR_SPLIT_ROLE_EXTRA) {
			dstr_cat(&rest, " (");
			cat_escaped(&rest, split.fader);
			dstr_cat(&rest, " takes one source per platform)");
		}
	}
	if (m.unmixed.len) {
		dstr_cat(&rest, rest.len ? ", " : "");
		dstr_cat_dstr(&rest, &m.unmixed);
	}
	struct dstr text = {0};
	dstr_printf(&text, "<b>Everything else:</b> %s. Any source not on a fader above goes here, along with alerts and transitions.", rest.len ? rest.array : "nothing right now");
	if (m.n_unmixed)
		dstr_catf(&text, " %d source%s past the mixer's limit of %d always play%s here.", m.n_unmixed, m.n_unmixed == 1 ? "" : "s", FDR_MAX_AUDIO_SOURCES, m.n_unmixed == 1 ? "s" : "");
	add_hint(group, "rest_info", text.array);
	dstr_free(&rest);
	dstr_free(&text);
	dstr_free(&m.unmixed);
	split_view_free(&split);
}

static obs_properties_t *src_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = obs_source_get_settings(settings_source);
	fdr_uplink_status st;
	get_status(&st);
	shown_fingerprint = status_fingerprint(&st);

	// ── Status card, then a callout for each problem it mentions ─────────────────────────
	struct status_view view;
	describe_status(settings, &st, &view);
	struct dstr html = {0};
	status_card_html(&view, &html);
	add_html(props, "status_info", html.array);
	status_view_free(&view);

	if (st.stream_track_silent) {
		dstr_printf(&html,
			    "Track %d, the one OBS streams, is silent, so viewers hear nothing. In Edit → Advanced Audio "
			    "Properties, tick Track %d for your mic, game and music. <a href=\"" SETUP_URL "\">Setup guide</a>",
			    st.stream_track, st.stream_track);
		add_callout(props, "stream_silent", TONE_ERROR, "Your stream has no audio", html.array);
	}
	if (st.mix_mismatch[0]) {
		struct dstr title = {0};
		dstr_cat(&title, "\"");
		cat_escaped(&title, st.mix_mismatch);
		dstr_cat(&title, "\" moved to Everything else");
		dstr_printf(&html,
			    "It's sent to Faders but isn't in your stream the same way: not on Track %d, captured twice, or "
			    "monitor only. Fix the source, then restart the stream. <a href=\"" DOCS_URL "\">How to assign sources</a>",
			    st.stream_track);
		add_callout(props, "mix_mismatch", TONE_WARN, title.array, html.array);
		dstr_free(&title);
	}
	if (st.mix_duplicate[0]) {
		struct dstr title = {0};
		dstr_cat(&title, "\"");
		cat_escaped(&title, st.mix_duplicate);
		dstr_cat(&title, "\" is in your stream twice");
		add_callout(props, "mix_duplicate", TONE_WARN, title.array,
			    "It's captured twice (for example by Desktop Audio as well), or another source plays the same "
			    "audio. Its fader controls one copy; the other stays in Everything else.");
		dstr_free(&title);
	}
	// The hub says when a newer release is out (kept in the settings, so it outlives the stream).
	const char *update = pending_update(settings);
	if (update) {
		struct dstr title = {0};
		dstr_printf(&title, "Faders %s is available", update);
		dstr_printf(&html, "You have %s. <a href=\"%s\">Download the update</a>", FDR_VERSION_STR, DOWNLOAD_URL);
		add_callout(props, "update", TONE_ACCENT, title.array, html.array);
		dstr_free(&title);
	}

	// ── Connection: the group's tick is the on/off switch and greys out the rest ─────────
	obs_properties_t *conn = obs_properties_create();
	obs_property_t *key = obs_properties_add_text(conn, "key", "Plugin key", OBS_TEXT_PASSWORD);
	obs_property_set_long_description(key, "Your key from the Faders portal. It isn't your Twitch or YouTube stream key.");
	add_hint(conn, "key_hint",
		 *obs_data_get_string(settings, "key") ? "Manage your keys in the <a href=\"" PORTAL_KEYS_URL "\">Faders portal</a>."
						       : "No key yet? <a href=\"" PORTAL_KEYS_URL "\">Create one in the Faders portal</a>.");
	obs_property_t *only_live = obs_properties_add_bool(conn, "only_live", "Only connect while I'm live");
	obs_property_set_long_description(only_live, "Untick to stay connected whenever OBS is open, for example to try it with a viewer before going live.");
	obs_properties_add_group(props, "enabled", "Send audio to Faders", OBS_GROUP_CHECKABLE, conn);

	// ── Faders: the names viewers get, capped by the plan ────────────────────────────────
	obs_properties_t *faders = obs_properties_create();
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n_names = fader_names(settings, names, FDR_MAX_FADERS);
	// The plan's allowance: from the hub while connected, else as last seen for this key.
	const char *plan = NULL;
	int allowance = known_allowance(settings, &st, &plan);
	if (allowance > 0 && n_names > allowance) {
		struct dstr title = {0};
		dstr_printf(&title, "Your %s%splan includes %d faders", plan ? plan : "", plan ? " " : "", allowance);
		dstr_copy(&html, "\"");
		cat_escaped(&html, names[allowance]);
		dstr_cat(&html, "\" and the faders after it are mixed into Everything else. <a href=\"" PORTAL_URL "\">Upgrade for more</a>");
		add_callout(faders, "cap_info", TONE_WARN, title.array, html.array);
		dstr_free(&title);
	} else if (allowance > 0) {
		dstr_printf(&html, "Your %s%splan includes %d faders. Everything else (alerts, transitions, anything not on a fader) is always sent too, so together they add up to your stream.",
			    plan ? plan : "", plan ? " " : "", allowance);
		add_hint(faders, "cap_info", html.array);
	} else {
		add_hint(faders, "cap_info",
			 "Everything else (alerts, transitions, anything not on a fader) is always sent too, so together they add up to your stream.");
	}
	obs_property_t *list = obs_properties_add_editable_list(faders, S_FADERS, "Fader names", OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);
	obs_property_set_long_description(list, "In the order viewers see them. Use + to add a fader and double-click a name to rename it.");
	obs_properties_add_group(props, "faders_group", "Your faders", OBS_GROUP_NORMAL, faders);

	// ── What goes on each fader: its sources, one dropdown each ──────────────────────────
	obs_properties_t *assign = obs_properties_create();
	add_assignment_rows(assign, settings, allowance);
	obs_properties_add_group(props, "sources_group", "What's on each fader", OBS_GROUP_NORMAL, assign);

	dstr_printf(&html,
		    "<span style=\"color: %s; font-size: small;\">Faders %s &nbsp;·&nbsp; <a href=\"" SETUP_URL "\">Setup guide</a> &nbsp;·&nbsp; "
		    "<a href=\"" PORTAL_URL "\">Portal</a> &nbsp;·&nbsp; <a href=\"" SUPPORT_URL "\">Help</a></span>",
		    look()->muted, FDR_VERSION_STR);
	add_html(props, "version", html.array);

	dstr_free(&html);
	obs_data_release(settings);
	return props;
}

// Move a source (by uuid or, for scripts, by name) to where `value` says (placement.h), after
// checking the destination against the faders as they are now. A refused move changes nothing;
// *error (if given) says why, in words for the window. Main/UI thread.
// Store a placement: the fader (NULL = NEW, "" = Everything else) and the platform tag; the mixer
// re-reads it. Main/UI thread.
static void store_placement(obs_source_t *s, const char *to, enum fdr_platform platform)
{
	set_platform(s, platform);
	pthread_mutex_lock(&uplink_mu);
	if (mixer) {
		fdr_mixer_assign(mixer, s, to);
	} else {
		obs_data_t *priv = obs_source_get_private_settings(s);
		if (to)
			obs_data_set_string(priv, FDR_FADER_SETTING, to);
		else
			obs_data_erase(priv, FDR_FADER_SETTING);
		obs_data_release(priv);
	}
	pthread_mutex_unlock(&uplink_mu);
}

// A platform pick for the split fader ("tw:<fader>" / "yt:<fader>", split.h): the source becomes
// its Twitch (or YouTube) source; the one it had goes to Everything else.
static bool place_split_source(obs_data_t *settings, obs_source_t *s, const char *value, struct dstr *error)
{
	struct fader_set f;
	current_faders(settings, &f);
	enum fdr_platform p = FDR_PLATFORM_ALL;
	char fader[FDR_PLACE_NAME_MAX];
	enum fdr_split_reject r = fdr_split_check_pick(&f.list, obs_data_get_string(settings, FDR_SPLIT_FADER_KEY), value, &p, fader, sizeof(fader));
	if (r != FDR_SPLIT_OK || !s) {
		if (error) {
			dstr_printf(error, "Couldn't move \"%s\": ", s ? obs_source_get_name(s) : "?");
			if (!s)
				dstr_copy(error, "Couldn't move that source: it no longer exists.");
			else if (r == FDR_SPLIT_REJECT_MISSING)
				dstr_cat(error, "that fader isn't there any more, or isn't in your plan.");
			else
				dstr_cat(error, "only a fader with \"Two sources: one per platform\" takes a source per platform.");
		}
		fdr_log(LOG_INFO, "move of \"%s\" to %s refused: %s", s ? obs_source_get_name(s) : "?", value ? value : "(null)", fdr_split_reject_name(r));
		return false;
	}
	struct split_sources had;
	find_split_sources(fader, &had);
	obs_source_t *before = p == FDR_PLATFORM_TWITCH ? had.tw : had.yt;
	if (before && before != s) {
		store_placement(before, "", FDR_PLATFORM_ALL);
		fdr_log(LOG_INFO, "\"%s\" → Everything else (replaced as %s's %s source)", obs_source_get_name(before), fader, p == FDR_PLATFORM_TWITCH ? "Twitch" : "YouTube");
	}
	obs_source_release(had.tw);
	obs_source_release(had.yt);
	store_placement(s, fader, p);
	fdr_log(LOG_INFO, "\"%s\" → %s (%s)", obs_source_get_name(s), fader, p == FDR_PLATFORM_TWITCH ? "Twitch" : "YouTube");
	return true;
}

static bool place_source(obs_data_t *settings, obs_source_t *s, const char *value, struct dstr *error)
{
	if (fdr_split_value_decode(value, NULL, NULL, 0))
		return place_split_source(settings, s, value, error);
	struct fader_set f;
	current_faders(settings, &f);
	enum fdr_value kind;
	char name[FDR_PLACE_NAME_MAX];
	int idx;
	enum fdr_reject r = fdr_place_check(&f.list, value, &kind, name, &idx);
	if (r != FDR_OK) {
		char want[FDR_PLACE_NAME_MAX * 2];
		fdr_value_decode(value, want, sizeof(want));
		if (error) {
			dstr_printf(error, "Couldn't move \"%s\": ", s ? obs_source_get_name(s) : "?");
			if (r == FDR_REJECT_LOCKED)
				dstr_catf(error, "\"%s\" isn't in your plan, so it plays in Everything else.", want);
			else if (r == FDR_REJECT_MISSING)
				dstr_catf(error, "there's no fader called \"%s\" any more.", want);
			else
				dstr_cat(error, "that isn't a place a source can go.");
		}
		fdr_log(LOG_INFO, "move of \"%s\" to %s refused: %s", s ? obs_source_get_name(s) : "?", value ? value : "(null)", fdr_reject_name(r));
		return false;
	}
	// The split fader holds its Twitch and YouTube sources and nothing else.
	if (kind == FDR_VALUE_FADER && fdr_split_check_move(obs_data_get_string(settings, FDR_SPLIT_FADER_KEY), name) == FDR_SPLIT_REJECT_ONLY_TWO) {
		if (error)
			dstr_printf(error,
				    "Couldn't move \"%s\": \"%s\" takes one Twitch source and one YouTube source. Pick them in its Twitch source and YouTube source lists.",
				    s ? obs_source_get_name(s) : "?", name);
		fdr_log(LOG_INFO, "move of \"%s\" to %s refused: only two", s ? obs_source_get_name(s) : "?", value);
		return false;
	}
	if (!s) {
		if (error)
			dstr_copy(error, "Couldn't move that source: it no longer exists.");
		return false;
	}
	if (kind == FDR_VALUE_KEEP)
		return true;
	const char *to = kind == FDR_VALUE_FADER ? name : kind == FDR_VALUE_REST ? "" : NULL;
	store_placement(s, to, FDR_PLATFORM_ALL); // a plain move: no platform any more
	fdr_log(LOG_INFO, "\"%s\" → %s", obs_source_get_name(s), !to ? "not placed (new)" : *to ? to : "Everything else");
	return true;
}

// A dropdown's item ("s:<uuid>") moved to `value`; a refusal goes to the window's feedback line.
static bool place_item(obs_data_t *settings, const char *item, const char *value)
{
	obs_source_t *s = strncmp(item, ITEM_SRC, strlen(ITEM_SRC)) == 0 ? obs_get_source_by_uuid(item + strlen(ITEM_SRC)) : NULL;
	struct dstr error = {0};
	bool ok = place_source(settings, s, value, &error);
	if (ok)
		dstr_free(&move_feedback);
	else
		dstr_move(&move_feedback, &error);
	dstr_free(&error);
	obs_source_release(s);
	return ok;
}

// "Two sources: one per platform" ticked or unticked on `fader`. Ticking is checked (split.h): its
// one source, if any, becomes its Twitch source. Unticking keeps the Twitch source on the fader
// and puts the YouTube source in Everything else. A refusal goes to the window's feedback line.
static bool toggle_split(obs_data_t *settings, const char *fader, bool on)
{
	struct split_view v;
	split_view_get(settings, &v);
	struct dstr error = {0};
	bool ok = true;
	if (on) {
		struct split_sources here;
		find_split_sources(fader, &here);
		enum fdr_split_reject r = fdr_split_check_tick(&v.acc.a, &v.faders.list, v.fader, fader, here.on_fader);
		if (r != FDR_SPLIT_OK) {
			ok = false;
			dstr_printf(&error, "Couldn't give \"%s\" two sources: ", fader);
			if (r == FDR_SPLIT_REJECT_PLAN)
				dstr_cat(&error, "that needs Pro or Partner.");
			else if (r == FDR_SPLIT_REJECT_YOUTUBE)
				dstr_cat(&error, "connect YouTube to your Faders account first.");
			else if (r == FDR_SPLIT_REJECT_OTHER)
				dstr_catf(&error, "only one fader can do this, and \"%s\" does.", v.fader);
			else if (r == FDR_SPLIT_REJECT_FULL)
				dstr_catf(&error, "it has %d sources, and it would hold only one for Twitch and one for YouTube. Move the others off it first.", here.on_fader);
			else
				dstr_cat(&error, "it isn't in your plan.");
			fdr_log(LOG_INFO, "split on \"%s\" refused: %s", fader, fdr_split_reject_name(r));
		} else {
			obs_data_set_string(settings, FDR_SPLIT_FADER_KEY, fader);
			if (here.only) {
				store_placement(here.only, fader, FDR_PLATFORM_TWITCH);
				fdr_log(LOG_INFO, "\"%s\" is %s's Twitch source", obs_source_get_name(here.only), fader);
			}
			fdr_log(LOG_INFO, "fader \"%s\": two sources, one per platform", fader);
		}
		obs_source_release(here.tw);
		obs_source_release(here.yt);
	} else if (v.index >= 0 && !astrcmpi(v.fader, fader)) {
		obs_data_set_string(settings, FDR_SPLIT_FADER_KEY, "");
		if (v.src.tw)
			store_placement(v.src.tw, v.fader, FDR_PLATFORM_ALL);
		if (v.src.yt)
			store_placement(v.src.yt, "", FDR_PLATFORM_ALL);
		fdr_log(LOG_INFO, "fader \"%s\": one source list again", fader);
	}
	split_view_free(&v);
	if (ok)
		dstr_free(&move_feedback);
	else
		dstr_move(&move_feedback, &error);
	dstr_free(&error);
	return ok;
}

// The dropdowns the streamer changed since the dialog was built. Returns whether any did.
static bool apply_dialog_changes(obs_data_t *settings)
{
	bool changed = false;
	for (int i = 0; i < n_dialog_rows; i++) {
		struct dialog_row *row = &dialog_rows[i];
		if (!obs_data_has_user_value(settings, row->key))
			continue;
		if (row->kind == ROW_SPLIT) {
			bool now = obs_data_get_bool(settings, row->key);
			if (now == (row->item[0] == '1'))
				continue;
			changed = true;
			toggle_split(settings, row->fader, now);
			continue;
		}
		const char *now = obs_data_get_string(settings, row->key);
		if (row->kind == ROW_TWITCH || row->kind == ROW_YOUTUBE) {
			if (strcmp(now, row->item) == 0)
				continue;
			if (!strcmp(now, CHOICE_NONE) && *row->item) {
				changed = true;
				place_item(settings, row->item, "x:rest");
			} else if (!strncmp(now, ITEM_SRC, strlen(ITEM_SRC))) {
				changed = true;
				char value[FDR_STEM_NAME_MAX + 8];
				fdr_split_value_encode(row->kind == ROW_TWITCH ? FDR_PLATFORM_TWITCH : FDR_PLATFORM_YOUTUBE, row->fader, value, sizeof(value));
				place_item(settings, now, value);
			}
			continue;
		}
		if (strcmp(now, row->item) == 0)
			continue;
		// An existing row offers only "Remove"; an Add row only sources. Anything else is not a
		// pick (an old copy of the dropdowns, e.g. from OBS's Cancel, Discard or Undo).
		if (*row->item ? strcmp(now, CHOICE_REMOVE) != 0 : strncmp(now, ITEM_SRC, strlen(ITEM_SRC)) != 0)
			continue;
		changed = true;
		if (*row->item) {
			place_item(settings, row->item, "x:rest");
		} else {
			char value[FDR_STEM_NAME_MAX + 8];
			fdr_value_encode(FDR_VALUE_FADER, row->fader, value, sizeof(value));
			place_item(settings, now, value);
		}
	}
	// The rows are rebuilt next; their keys mean nothing outside this dialog session.
	for (int i = 0; i < n_dialog_rows; i++)
		obs_data_erase(settings, dialog_rows[i].key);
	if (changed)
		n_dialog_rows = 0;
	return changed;
}

struct rename {
	const char *from, *to;
};

static bool rename_source(void *param, obs_source_t *s)
{
	const struct rename *r = param;
	if (!is_audio_input(s))
		return true;
	obs_data_t *priv = obs_source_get_private_settings(s);
	if (obs_data_has_user_value(priv, FDR_FADER_SETTING) && astrcmpi(obs_data_get_string(priv, FDR_FADER_SETTING), r->from) == 0)
		obs_data_set_string(priv, FDR_FADER_SETTING, r->to);
	obs_data_release(priv);
	return true;
}

// A fader renamed in place (same position, same number of faders, the old name gone and the new
// one new) keeps what was on it: sources are stored by fader name, so they follow.
static void follow_renames(obs_data_t *settings, const char names[][FDR_STEM_NAME_MAX], int n)
{
	if (n_last_names == n) {
		for (int i = 0; i < n; i++) {
			if (astrcmpi(last_names[i], names[i]) == 0)
				continue;
			bool reused = false;
			for (int j = 0; j < n; j++)
				reused = reused || astrcmpi(names[j], last_names[i]) == 0 || astrcmpi(last_names[j], names[i]) == 0;
			if (reused)
				continue; // a swap or a reorder, not a rename
			struct rename r = {last_names[i], names[i]};
			obs_enum_sources(rename_source, &r);
			if (!astrcmpi(obs_data_get_string(settings, FDR_SPLIT_FADER_KEY), r.from))
				obs_data_set_string(settings, FDR_SPLIT_FADER_KEY, r.to); // still the split fader
			fdr_log(LOG_INFO, "fader \"%s\" renamed to \"%s\": its sources follow", r.from, r.to);
		}
	}
	memcpy(last_names, names, sizeof(last_names));
	n_last_names = n;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	erase_legacy_tracks(settings);
	bool reassigned = apply_dialog_changes(settings);
	// Renaming, adding or removing a fader changes the rows too.
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n = fader_names(settings, names, FDR_MAX_FADERS);
	follow_renames(settings, names, n);
	char joined[sizeof(dialog_names)] = "";
	for (int i = 0; i < n; i++) {
		strncat(joined, names[i], sizeof(joined) - strlen(joined) - 2);
		strcat(joined, "\n");
	}
	bool renamed = n_dialog_rows > 0 && strcmp(joined, dialog_names) != 0;
	// The YouTube track changes what the split fader's block says.
	static long long last_split_track = -1;
	long long split_track = obs_data_get_int(settings, FDR_SPLIT_TRACK_KEY);
	bool split_changed = last_split_track >= 0 && split_track != last_split_track;
	last_split_track = split_track;
	save_settings(settings);
	// A change of names or placements while running doesn't need a restart.
	pthread_mutex_lock(&uplink_mu);
	// A new key or server, or "Only send while streaming" switched on while offline, does.
	bool live = running_as(settings);
	if (live)
		apply_faders(settings);
	pthread_mutex_unlock(&uplink_mu);
	if (!live)
		apply(settings);
	// An open dialog redraws with the new rows, once OBS has finished handling the change (see
	// redraw_wanted: never from in here).
	if (reassigned || renamed || split_changed)
		os_atomic_set_bool(&redraw_wanted, true);
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

static long dialog_opens; // test hook: times Tools → Faders was opened by the plugin or the menu

static void open_settings(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_atomic_inc_long(&dialog_opens);
	obs_frontend_open_source_properties(settings_source);
}

// Once per release: open Tools → Faders at startup so the update notice is seen without a stream.
static bool announce_update(obs_data_t *settings)
{
	const char *update = pending_update(settings);
	if (update && strcmp(obs_data_get_string(settings, "update_announced"), update) != 0) {
		obs_data_set_string(settings, "update_announced", update);
		return true;
	}
	return false;
}

static bool placed_anywhere(void *param, obs_source_t *s)
{
	if (!is_audio_input(s))
		return true;
	obs_data_t *priv = obs_source_get_private_settings(s);
	*(bool *)param = obs_data_has_user_value(priv, FDR_FADER_SETTING);
	obs_data_release(priv);
	return !*(bool *)param;
}

// The first start after installing: nothing is placed for the streamer, so Tools → Faders opens
// once to show the faders are empty and every source is NEW. Not if something is already placed
// (an earlier install), and never again.
static bool open_on_first_start(obs_data_t *settings)
{
	if (obs_data_get_bool(settings, S_FIRST_START))
		return false;
	obs_data_set_bool(settings, S_FIRST_START, true);
	bool placed = false;
	obs_enum_sources(placed_anywhere, &placed);
	return !placed;
}

static void on_finished_loading(void)
{
	frontend_loaded = true;
	apply_current();
	obs_data_t *settings = obs_source_get_settings(settings_source);
	bool first = open_on_first_start(settings);
	bool update = announce_update(settings);
	if (first || update || !obs_data_has_user_value(settings, S_FIRST_START))
		save_settings(settings);
	if (first || update)
		obs_queue_task(OBS_TASK_UI, open_settings, NULL, false);
	obs_data_release(settings);
}

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		on_finished_loading();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_set_transition_duration(mixer, obs_frontend_get_transition_duration());
		pthread_mutex_unlock(&uplink_mu);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		// The window waits for the switch to finish (see watch_status), then redraws once.
		collection_switch_ns = os_gettime_ns();
		os_atomic_set_bool(&collection_switching, true);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		pthread_mutex_lock(&uplink_mu);
		fdr_mixer_rebuild(mixer);
		pthread_mutex_unlock(&uplink_mu);
		os_atomic_set_bool(&collection_switching, false);
		os_atomic_set_bool(&sources_changed, true);
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
		stream_started_ns = os_gettime_ns();
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
// faders_assign(source, fader → ok, error): fader name, "" = Everything else. Checked like a pick.
// faders_place(source, value → ok, error): a placement value (placement.h): "f:<fader>", "x:rest",
//   "x:new" (back to not placed), "x:keep".
// faders_placement(source → saved, effective, state, platform): saved = "new" / "rest" / the
//   fader's name; effective = the fader it plays on ("" = Everything else); state = "new", "rest",
//   "ok", "locked", "deleted", "dropped", or on the split fader "parked" (its YouTube source while
//   the split is off) or "extra" (anything else saved on it); platform = "twitch", "youtube", "".
// faders_place also takes "tw:<fader>" / "yt:<fader>": the split fader's Twitch / YouTube source.
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

static void place_proc(calldata_t *cd, const char *value)
{
	obs_source_t *src = obs_get_source_by_name(calldata_string(cd, "source"));
	obs_data_t *settings = obs_source_get_settings(settings_source);
	struct dstr error = {0};
	bool ok = place_source(settings, src, value, &error);
	calldata_set_bool(cd, "ok", ok);
	calldata_set_string(cd, "error", error.array ? error.array : "");
	dstr_free(&error);
	obs_data_release(settings);
	obs_source_release(src);
}

static void proc_assign(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	const char *fader = calldata_string(cd, "fader");
	char value[FDR_STEM_NAME_MAX * 2] = "";
	if (fader && !*fader)
		fdr_value_encode(FDR_VALUE_REST, NULL, value, sizeof(value));
	else if (fader)
		fdr_value_encode(FDR_VALUE_FADER, fader, value, sizeof(value));
	place_proc(cd, value);
}

static void proc_place(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	place_proc(cd, calldata_string(cd, "value"));
}

static void proc_placement(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	obs_source_t *src = obs_get_source_by_name(calldata_string(cd, "source"));
	obs_data_t *settings = obs_source_get_settings(settings_source);
	struct fader_set f;
	current_faders(settings, &f);
	enum fdr_saved saved = FDR_SAVED_NEW;
	enum fdr_state state = FDR_STATE_NEW;
	char name[FDR_STEM_NAME_MAX] = "";
	int idx = src ? effective_placement(&f, src, &saved, name, &state) : -1;
	// On the split fader: the YouTube source while it's off ("parked") and anything else saved on
	// it ("extra") play in Everything else (split.h).
	const char *state_name = fdr_state_name(state);
	if (src && state == FDR_STATE_ON_FADER) {
		struct split_view v;
		split_view_get(settings, &v);
		enum fdr_split_role role = split_role_of(&v, src);
		if (role == FDR_SPLIT_ROLE_PARKED || role == FDR_SPLIT_ROLE_EXTRA) {
			idx = -1;
			state_name = fdr_split_role_name(role);
		}
		split_view_free(&v);
	}
	calldata_set_string(cd, "saved", saved == FDR_SAVED_NEW ? "new" : saved == FDR_SAVED_REST ? "rest" : name);
	calldata_set_string(cd, "effective", idx >= 0 ? f.names[idx] : "");
	calldata_set_string(cd, "state", state_name);
	calldata_set_string(cd, "platform", src ? fdr_platform_name(platform_of(src)) : "");
	obs_data_release(settings);
	obs_source_release(src);
}

// Headless tests have no frontend: deliver one of its events the way OBS would.
static void proc_frontend_event(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	on_frontend_event((enum obs_frontend_event)calldata_int(cd, "event"), NULL);
}

static void proc_dialog_opens(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	calldata_set_int(cd, "n", os_atomic_load_long(&dialog_opens));
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
		if (obs_property_get_type(p) == OBS_PROPERTY_BOOL) { // key<TAB>[x] label (disabled)
			dstr_catf(&out, "%s\t%s %s%s\n", key, obs_data_get_bool(settings, key) ? "[x]" : "[ ]", obs_property_description(p), obs_property_enabled(p) ? "" : " (disabled)");
			continue;
		}
		if (obs_property_get_type(p) != OBS_PROPERTY_LIST) {
			dstr_catf(&out, "%s\t", key);
			strip_tags(obs_property_description(p), &out);
			dstr_cat(&out, "\n");
			continue;
		}
		const char *value = obs_data_get_string(settings, key);
		bool ints = obs_property_list_format(p) == OBS_COMBO_FORMAT_INT;
		const char *shown = "";
		struct dstr choices = {0};
		for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
			if (ints ? obs_property_list_item_int(p, i) == obs_data_get_int(settings, key) : strcmp(obs_property_list_item_string(p, i), value) == 0)
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
	if (p && want && obs_property_get_type(p) == OBS_PROPERTY_BOOL) { // choice: "on" / "off"
		change = obs_data_create();
		obs_data_set_bool(change, key, !strcmp(want, "on"));
	}
	for (size_t i = 0; p && want && obs_property_get_type(p) == OBS_PROPERTY_LIST && i < obs_property_list_item_count(p) && !change; i++) {
		if (strncmp(obs_property_list_item_name(p, i), want, strlen(want)) != 0)
			continue;
		change = obs_data_create();
		if (obs_property_list_format(p) == OBS_COMBO_FORMAT_INT)
			obs_data_set_int(change, key, obs_property_list_item_int(p, i));
		else
			obs_data_set_string(change, key, obs_property_list_item_string(p, i));
	}
	obs_properties_destroy(props);
	long before = os_atomic_load_long(&dialog_redraws);
	if (change)
		obs_source_update(settings_source, change);
	calldata_set_bool(cd, "ok", change != NULL);
	// How often the window was told to rebuild while OBS was still applying the change: must be 0.
	calldata_set_int(cd, "redraws_during_update", os_atomic_load_long(&dialog_redraws) - before);
	obs_data_release(change);
}

static void proc_dialog_redraws(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	calldata_set_int(cd, "n", os_atomic_load_long(&dialog_redraws));
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

static void proc_split_stats(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	struct dstr json = {0};
	pthread_mutex_lock(&uplink_mu);
	fdr_splitmix_stats_json(fdr_mixer_split(mixer), &json);
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

// ── Debug report ────────────────────────────────────────────────────────────────────────
// A snapshot of what the plugin knows, built into the report by report.c (redacted, names on or
// off, fail closed). Nothing here sends it anywhere: the streamer copies or saves it.

#define REPORT_MAX_SOURCES 1000 // read at most this many audio inputs (the report lists 200)

struct report_sources {
	struct fdr_report_source list[REPORT_MAX_SOURCES];
	char *strings[REPORT_MAX_SOURCES * 4];
	int n, n_strings, total;
	struct fader_set faders;
	uint32_t stream_bit;
};

static const char *keep(struct report_sources *r, const char *s)
{
	char *d = bstrdup(s ? s : "");
	r->strings[r->n_strings++] = d;
	return d;
}

static const char *monitoring_name(enum obs_monitoring_type t)
{
	return t == OBS_MONITORING_TYPE_MONITOR_ONLY ? "monitor only" : t == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT ? "monitor and output" : "none";
}

static bool collect_report_source(void *param, obs_source_t *s)
{
	struct report_sources *r = param;
	if (!is_audio_input(s))
		return true;
	r->total++;
	if (r->n >= REPORT_MAX_SOURCES)
		return true;
	enum fdr_saved saved;
	enum fdr_state state;
	char name[FDR_STEM_NAME_MAX];
	int idx = effective_placement(&r->faders, s, &saved, name, &state);
	uint32_t mixers = obs_source_get_audio_mixers(s);
	r->list[r->n++] = (struct fdr_report_source){
		.name = keep(r, obs_source_get_name(s)),
		.kind = keep(r, obs_source_get_id(s)),
		.saved = keep(r, saved == FDR_SAVED_NEW ? "new" : saved == FDR_SAVED_REST ? "rest" : name),
		.plays = keep(r, idx >= 0 ? r->faders.names[idx] : ""),
		.state = fdr_state_name(state),
		.muted = obs_source_muted(s),
		.monitoring = monitoring_name(obs_source_get_monitoring_type(s)),
		.tracks = mixers,
		.on_stream_track = (mixers & r->stream_bit) != 0,
	};
	return true;
}

// The last part of a home folder path: the OS user name as folders spell it.
static const char *home_user(char *buf, size_t size)
{
	const char *home = getenv("HOME") ? getenv("HOME") : getenv("USERPROFILE");
	if (!home || !*home)
		return NULL;
	snprintf(buf, size, "%s", home);
	size_t n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '/' || buf[n - 1] == '\\'))
		buf[--n] = 0;
	const char *a = strrchr(buf, '/'), *b = strrchr(buf, '\\');
	const char *last = a > b ? a : b;
	return last ? last + 1 : buf;
}

static int utc_offset_minutes(time_t now)
{
	struct tm lt, gt;
#ifdef _WIN32
	localtime_s(&lt, &now);
	gmtime_s(&gt, &now);
#else
	localtime_r(&now, &lt);
	gmtime_r(&now, &gt);
#endif
	gt.tm_isdst = lt.tm_isdst;
	return (int)(difftime(mktime(&lt), mktime(&gt)) / 60.0);
}

// Build the report. true: *report is it (bfree() it). false: *error says why there is none.
static bool debug_report(bool names, char **report, const char **error)
{
	*report = NULL;
	*error = NULL;
	static struct report_sources r; // large: off the stack (UI thread, or a test's main thread)
	memset(&r, 0, sizeof(r));
	obs_data_t *settings = obs_source_get_settings(settings_source);
	fdr_uplink_status st;
	get_status(&st);
	current_faders(settings, &r.faders);
	int track = forced_stream_track ? forced_stream_track : frontend_loaded ? stream_audio_track() : 0;
	r.stream_bit = 1u << ((track ? track : 1) - 1);
	obs_enum_sources(collect_report_source, &r);

	struct fdr_report_fader faders[FDR_MAX_FADERS];
	for (int i = 0; i < r.faders.list.n; i++)
		faders[i] = (struct fdr_report_fader){r.faders.names[i], i < r.faders.list.open};
	static struct fdr_log_line log[FDR_LOG_LINES];
	uint64_t log_total = 0;
	int n_log = fdr_logring_copy(log, FDR_LOG_LINES, &log_total);

	struct obs_audio_info ai = {0};
	obs_get_audio_info(&ai);
	char *config_dir = obs_module_config_path("");
	const char *module_dir = obs_get_module_binary_path(obs_current_module());
	// The stream key is read only to scrub it from the report, never to put it in.
	char *stream_key = NULL;
	if (frontend_loaded) {
		obs_service_t *svc = obs_frontend_get_streaming_service();
		const char *k = svc ? obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : NULL;
		stream_key = k && *k ? bstrdup(k) : NULL;
	}
	char home[512];
	const char *key = obs_data_get_string(settings, "key");
	const char *server = obs_data_get_string(settings, "server");
	const char *account = obs_data_get_string(settings, "account");
	bool remembered = *account && strcmp(obs_data_get_string(settings, "account_key"), key_tail(key)) == 0;
	const char *plan = st.plan[0] ? st.plan : remembered ? obs_data_get_string(settings, "account_plan") : "";
	const char *allow_plan = NULL;
	time_t now = time(NULL);
	uint64_t now_ns = os_gettime_ns();
	struct fdr_report_input in = {
		.names = names,
		.now = (int64_t)now,
		.utc_offset_min = utc_offset_minutes(now),
		.plugin_version = FDR_VERSION_STR,
		.plugin_uptime_s = (int64_t)((now_ns - loaded_ns) / 1000000000ULL),
		.update_available = pending_update(settings) ? pending_update(settings) : "",
		.config_dir = config_dir ? config_dir : "",
		.module_dir = module_dir ? module_dir : "",
		.obs_version = obs_get_version_string(),
		.obs_locale = obs_get_locale(),
		.streaming = streaming,
		.stream_uptime_s = streaming && stream_started_ns ? (int64_t)((now_ns - stream_started_ns) / 1000000000ULL) : 0,
		.stream_audio_track = track,
		.sample_rate = (int)ai.samples_per_sec,
		.channels = (int)get_audio_channels(ai.speakers),
		.mixer_ok = !mixer_unavailable && ai.samples_per_sec == 48000 && ai.speakers == SPEAKERS_STEREO,
		.enabled = obs_data_get_bool(settings, "enabled"),
		.only_live = obs_data_get_bool(settings, "only_live"),
		.plugin_key_set = *key != 0,
		.channel = st.channel[0] ? st.channel : remembered ? account : "",
		.plan = plan,
		.platforms = st.platforms,
		.listener_limit = st.listener_limit,
		.max_stems = st.max_stems,
		.running = st.running,
		.connected = st.connected,
		.ever_connected = st.ever_connected,
		.server = strcmp(server, DEFAULT_SERVER) == 0 ? DEFAULT_SERVER : "custom",
		.listeners = st.listeners,
		.uplink_kbps = st.uplink_kbps,
		.stems_on_wire = st.stems,
		.stream_track = st.stream_track,
		.stream_track_silent = st.stream_track_silent,
		.chunks_sent = st.chunks_sent,
		.last_failure = st.failure,
		.rejected_status = st.rejected_status,
		.mix_mismatch = st.mix_mismatch,
		.mix_duplicate = st.mix_duplicate,
		.faders = faders,
		.n_faders = r.faders.list.n,
		.allowance = known_allowance(settings, &st, &allow_plan),
		.sources = r.list,
		.n_sources = r.n,
		.audio_total = r.total,
		.past_limit = r.total > FDR_MAX_AUDIO_SOURCES ? r.total - FDR_MAX_AUDIO_SOURCES : 0,
		.log = log,
		.n_log = n_log,
		.log_total = log_total,
		.redact = {.plugin_key = key, .stream_key = stream_key, .user_names = {getenv("USER") ? getenv("USER") : getenv("USERNAME"), home_user(home, sizeof(home))}},
	};
	fdr_sysinfo_get(&in.sys);
	char *text = NULL;
	bool ok = fdr_report_build(&in, &text) == 0;
	if (ok) {
		*report = bstrdup(text);
		free(text);
	} else {
		*error = "Couldn't build a safe report: your plugin key or stream key would have been in it.";
		fdr_log(LOG_WARNING, "debug report refused: a key would have been in it");
	}
	if (stream_key) {
		memset(stream_key, 0, strlen(stream_key));
		bfree(stream_key);
	}
	bfree(config_dir);
	for (int i = 0; i < r.n_strings; i++)
		bfree(r.strings[i]);
	obs_data_release(settings);
	return ok;
}

static void proc_debug_report(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	char *report = NULL;
	const char *error = NULL;
	bool ok = debug_report(calldata_bool(cd, "names"), &report, &error);
	calldata_set_bool(cd, "ok", ok);
	calldata_set_string(cd, "report", report ? report : "");
	calldata_set_string(cd, "error", error ? error : "");
	bfree(report);
}

static void log_to_obs(int level, const char *msg)
{
	blog(level, "[faders] %s", msg);
}

bool obs_module_load(void)
{
	loaded_ns = os_gettime_ns();
	fdr_log_set_sink(log_to_obs);
	obs_register_source(&settings_source_info);

	char *path = config_file();
	obs_data_t *saved = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	settings_source = obs_source_create_private(SOURCE_ID, "Faders", saved);
	obs_data_release(saved);
	signal_handler_connect(obs_source_get_signal_handler(settings_source), "update_properties", count_redraw, NULL);

	proc_handler_t *ph = obs_get_proc_handler();
	// Headless tests have no streaming output: point the streamed-track check at a track directly.
	proc_handler_add(ph, "void faders_watch_stream_track(in int track)", proc_watch_stream_track, NULL);
	proc_handler_add(ph, "void faders_configure(in string server, in string key, in string faders, in bool only_live)", proc_configure, NULL);
	proc_handler_add(ph, "void faders_assign(in string source, in string fader, out bool ok, out string error)", proc_assign, NULL);
	proc_handler_add(ph, "void faders_place(in string source, in string value, out bool ok, out string error)", proc_place, NULL);
	proc_handler_add(ph, "void faders_placement(in string source, out string saved, out string effective, out string state, out string platform)", proc_placement, NULL);
	proc_handler_add(ph, "void faders_frontend_event(in int event)", proc_frontend_event, NULL);
	proc_handler_add(ph, "void faders_dialog_opens(out int n)", proc_dialog_opens, NULL);
	proc_handler_add(ph, "void faders_debug_report(in bool names, out bool ok, out string report, out string error)", proc_debug_report, NULL);
	proc_handler_add(ph, "void faders_status(out string status)", proc_status, NULL);
	proc_handler_add(ph, "void faders_dialog(out string rows)", proc_dialog, NULL);
	proc_handler_add(ph, "void faders_dialog_set(in string key, in string choice, out bool ok, out int redraws_during_update)", proc_dialog_set, NULL);
	proc_handler_add(ph, "void faders_dialog_redraws(out int n)", proc_dialog_redraws, NULL);
	proc_handler_add(ph, "void faders_mixer_stats(out string json)", proc_mixer_stats, NULL);
	proc_handler_add(ph, "void faders_split_stats(out string json)", proc_split_stats, NULL);
	proc_handler_add(ph, "void faders_rebuild()", proc_rebuild, NULL);
	proc_handler_add(ph, "void faders_transition_duration(in int ms)", proc_transition_duration, NULL);

	// No-ops when there's no OBS frontend (e.g. the headless test host).
	obs_frontend_add_tools_menu_item("Faders", open_settings, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	connect_source_signals(true);
	obs_add_tick_callback(watch_status, NULL);
	fdr_log(LOG_INFO, "loaded (version %s)", FDR_VERSION_STR);
	return true;
}

void obs_module_unload(void)
{
	obs_remove_tick_callback(watch_status, NULL);
	connect_source_signals(false);
	if (settings_source)
		signal_handler_disconnect(obs_source_get_signal_handler(settings_source), "update_properties", count_redraw, NULL);
	apply(NULL);
	obs_source_release(settings_source);
	settings_source = NULL;
}
