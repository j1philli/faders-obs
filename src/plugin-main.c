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
	if (st->stems > 0)
		dstr_catf(&v->meta, "%s%d faders + Everything else", v->meta.len ? " · " : "", st->stems - 1);
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
	     (st.max_stems > 0 && obs_data_get_int(s, "account_faders") != st.max_stems) || strcmp(obs_data_get_string(s, "account_plan"), st.plan) != 0)) {
		obs_data_set_string(s, "account", st.channel);
		obs_data_set_string(s, "account_key", tail);
		obs_data_set_string(s, "account_plan", st.plan);
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
	if (os_atomic_exchange_bool(&redraw_wanted, false))
		obs_queue_task(OBS_TASK_UI, redraw_window, NULL, false);
	uint64_t now = os_gettime_ns();
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

// Find a track that carries this source to a named fader. Earlier tracks that
// resolve to Everything else don't prevent a later track from carrying it.
static int settings_named_track_for(const struct dialog_model *m, uint32_t mixers)
{
	int stream = stream_audio_track();
	for (int t = 1; t <= 6; t++) {
		if (t == (stream ? stream : 1) || !(mixers & (1u << (t - 1))))
			continue;
		char key[16];
		snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
		const char *v = obs_data_get_string(m->settings, key);
		if (strcmp(v, CHOICE_NONE) != 0 && strcmp(v, CHOICE_REST) != 0 && name_index(m, v) >= 0)
			return t;
	}
	return 0;
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
	if (it->fader < 0)
		it->via = settings_named_track_for(m, it->mixers);
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
			cat_escaped(&where, m->names[src->fader]);
		}
		for (int k = 0; k < m->n_items; k++) {
			const struct dialog_item *tr = &m->items[k];
			int t = tr->track ? atoi(tr->id + strlen(ITEM_TRACK)) : 0;
			if (!t || tr->fader < 0 || !(src->mixers & (1u << (t - 1))) || (seen & (1u << tr->fader)))
				continue;
			seen |= 1u << tr->fader;
			dstr_cat(&where, count ? ", " : "");
			cat_escaped(&where, m->names[tr->fader]);
			dstr_catf(&where, " (with Track %d)", t);
			count++;
		}
		if (count > 1) {
			dstr_cat(out, "\"");
			cat_escaped(out, src->label);
			dstr_catf(out, "\" is on %d faders: %s.<br>", count, where.array);
		}
		dstr_free(&where);
	}
	if (out->len)
		dstr_cat(out, "Viewers hear it once for each fader, so it's louder for them than in your stream. Take it off one of "
			      "those faders, or untick that track for it in Edit → Advanced Audio Properties.");
}

static void add_assignment_rows(obs_properties_t *group, obs_data_t *settings)
{
	static struct dialog_model m; // large: off the stack (dialog code runs on the UI thread only)
	build_dialog_model(&m, settings);
	struct dstr dup = {0};
	duplicate_warning(&m, &dup);
	if (dup.len)
		add_callout(group, "dup_warning", TONE_WARN, "Some sources are on more than one fader", dup.array);
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
		cat_escaped(&rest, m.items[i].label);
	}
	struct dstr text = {0};
	dstr_printf(&text, "<b>Everything else:</b> %s. Any source not on a fader above goes here, along with alerts and transitions.", rest.len ? rest.array : "nothing right now");
	add_hint(group, "rest_info", text.array);
	dstr_free(&rest);
	dstr_free(&text);
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
	int allowance = st.max_stems > 0 ? st.max_stems : 0;
	const char *plan = plan_label(st.plan);
	if (!allowance && !strcmp(obs_data_get_string(settings, "account_key"), key_tail(obs_data_get_string(settings, "key")))) {
		allowance = (int)obs_data_get_int(settings, "account_faders");
		plan = plan_label(obs_data_get_string(settings, "account_plan"));
	}
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

	// ── What goes on each fader: its sources and OBS tracks, one dropdown each ────────
	obs_properties_t *assign = obs_properties_create();
	add_assignment_rows(assign, settings);
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

struct rename {
	const char *from, *to;
	const char (*before)[FDR_STEM_NAME_MAX];
	int n_before;
};

static bool rename_source(void *param, obs_source_t *s)
{
	const struct rename *r = param;
	uint32_t flags = obs_source_get_output_flags(s);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE) || obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT)
		return true;
	obs_data_t *priv = obs_source_get_private_settings(s);
	const char *on = NULL;
	if (obs_data_has_user_value(priv, FDR_FADER_SETTING)) {
		on = obs_data_get_string(priv, FDR_FADER_SETTING);
	} else {
		// Never placed: it sits where the plugin's starting guess put it under the old names.
		on = fdr_mixer_auto_fader(s);
		bool exists = false, voice = false;
		for (int i = 0; on && i < r->n_before; i++) {
			exists = exists || astrcmpi(r->before[i], on) == 0;
			voice = voice || astrcmpi(r->before[i], "Voice") == 0;
		}
		if (on && !exists && strcmp(on, "Discord") == 0 && voice)
			on = "Voice";
	}
	if (on && astrcmpi(on, r->from) == 0)
		obs_data_set_string(priv, FDR_FADER_SETTING, r->to);
	obs_data_release(priv);
	return true;
}

// A fader renamed in place (same position, same number of faders, the old name gone and the new
// one new) keeps what was on it: sources and tracks are stored by fader name, so they follow.
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
			struct rename r = {last_names[i], names[i], last_names, n};
			obs_enum_sources(rename_source, &r);
			for (int t = 1; t <= 6; t++) {
				char key[16];
				snprintf(key, sizeof(key), TRACK_PREFIX "%d", t);
				if (astrcmpi(obs_data_get_string(settings, key), r.from) == 0)
					obs_data_set_string(settings, key, r.to);
			}
			blog(LOG_INFO, "[faders] fader \"%s\" renamed to \"%s\": its sources and tracks follow", r.from, r.to);
		}
	}
	memcpy(last_names, names, sizeof(last_names));
	n_last_names = n;
}

static void src_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
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
	save_settings(settings);
	// A change of names or assignments while running doesn't need a restart.
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
	if (reassigned || renamed)
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
			dstr_catf(&out, "%s\t", key);
			strip_tags(obs_property_description(p), &out);
			dstr_cat(&out, "\n");
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
	signal_handler_connect(obs_source_get_signal_handler(settings_source), "update_properties", count_redraw, NULL);

	proc_handler_t *ph = obs_get_proc_handler();
	// Headless tests have no streaming output: point the streamed-track check at a track directly.
	proc_handler_add(ph, "void faders_watch_stream_track(in int track)", proc_watch_stream_track, NULL);
	proc_handler_add(ph, "void faders_configure(in string server, in string key, in string faders, in bool only_live)", proc_configure, NULL);
	proc_handler_add(ph, "void faders_assign(in string source, in string fader)", proc_assign, NULL);
	proc_handler_add(ph, "void faders_assign_track(in int track, in string fader)", proc_assign_track, NULL);
	proc_handler_add(ph, "void faders_status(out string status)", proc_status, NULL);
	proc_handler_add(ph, "void faders_dialog(out string rows)", proc_dialog, NULL);
	proc_handler_add(ph, "void faders_dialog_set(in string key, in string choice, out bool ok, out int redraws_during_update)", proc_dialog_set, NULL);
	proc_handler_add(ph, "void faders_dialog_redraws(out int n)", proc_dialog_redraws, NULL);
	proc_handler_add(ph, "void faders_mixer_stats(out string json)", proc_mixer_stats, NULL);
	proc_handler_add(ph, "void faders_rebuild()", proc_rebuild, NULL);
	proc_handler_add(ph, "void faders_transition_duration(in int ms)", proc_transition_duration, NULL);

	// No-ops when there's no OBS frontend (e.g. the headless test host).
	obs_frontend_add_tools_menu_item("Faders", open_settings, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_add_tick_callback(watch_status, NULL);
	blog(LOG_INFO, "[faders] loaded (version %s)", FDR_VERSION_STR);
	return true;
}

void obs_module_unload(void)
{
	obs_remove_tick_callback(watch_status, NULL);
	if (settings_source)
		signal_handler_disconnect(obs_source_get_signal_handler(settings_source), "update_properties", count_redraw, NULL);
	apply(NULL);
	obs_source_release(settings_source);
	settings_source = NULL;
}
