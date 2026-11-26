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
// faders-split <scenario>: the split fader ("Two sources: one per platform") in Tools → Faders,
// headless (real libobs, the real plugin, no hub: the account is the one remembered in the
// settings, as after an earlier connection).
//
//   window     the checkbox on every fader; once ticked: the two source lists, the YouTube track,
//              "Counts as 1 fader.", the per-platform status lines; one fader only
//   free       Free: every checkbox disabled, "Needs Pro or Partner"; a tick changes nothing
//   noyoutube  no YouTube on the account: "Connect YouTube to use this"
//   onlytwo    the split fader holds its Twitch and YouTube sources and nothing else
//   roundtrip  save/load and duplicate keep it; the settings keep the fader and the track
//   reload     the plugin settings bring the split back after a restart
//   downgrade  Free turns it off and keeps the picks (YouTube source in Everything else);
//              Pro again restores it
//   counts     the split fader counts as one fader against the plan's allowance
// One PASS/FAIL line per check; exits non-zero if any fails.
#define main placement_main
#include "placement.c"
#undef main

static const char *const PRO_SETTINGS =
	"{\"enabled\":true,\"server\":\"ws://127.0.0.1:9/ingest/placement\",\"key\":\"fdr_test_placementFAKEKEY\",\"only_live\":true,"
	"\"account\":\"some_channel\",\"account_key\":\"AKEKEY\",\"account_plan\":\"%s\",\"account_platforms\":\"%s\",\"account_faders\":%d,"
	"\"faders\":[{\"value\":\"Voice\"},{\"value\":\"Game\"},{\"value\":\"Music\"}]%s}";

static bool start_as(const char *name, const char *plan, const char *platforms, int faders, const char *extra)
{
	char json[2048];
	snprintf(json, sizeof(json), PRO_SETTINGS, plan, platforms, faders, extra ? extra : "");
	if (!start(name, json))
		return false;
	configure("Voice,Game,Music", false);
	return true;
}

// The remembered account changes (as the hub's next hello would make it).
static void set_account(const char *plan, const char *platforms)
{
	obs_data_t *change = obs_data_create();
	obs_data_set_string(change, "account_plan", plan);
	obs_data_set_string(change, "account_platforms", platforms);
	obs_source_update(settings_source, change);
	obs_data_release(change);
	pump_for(100);
}

// faders_placement with the platform: "<saved>|<effective>|<state>|<platform>".
static const char *where(obs_source_t *s)
{
	static char bufs[8][256];
	static int next;
	char *out = bufs[next++ % 8];
	calldata_t cd = {0};
	calldata_set_string(&cd, "source", obs_source_get_name(s));
	if (call("faders_placement", &cd))
		snprintf(out, sizeof(bufs[0]), "%s|%s|%s|%s", calldata_string(&cd, "saved"), calldata_string(&cd, "effective"), calldata_string(&cd, "state"),
			 calldata_string(&cd, "platform") ? calldata_string(&cd, "platform") : "(no platform)");
	else
		snprintf(out, sizeof(bufs[0]), "(no faders_placement)");
	calldata_free(&cd);
	return out;
}
// The saved platform tag ("" = none).
static const char *tag(obs_source_t *s)
{
	static char bufs[8][32];
	static int next;
	char *out = bufs[next++ % 8];
	obs_data_t *priv = obs_source_get_private_settings(s);
	snprintf(out, sizeof(bufs[0]), "%s", obs_data_get_string(priv, "faders_platform"));
	obs_data_release(priv);
	return out;
}
// The checkbox line for fader index i ("" when there is none): "[x] Two sources: …".
static const char *checkbox(int i)
{
	char key[32];
	snprintf(key, sizeof(key), "split:%d", i);
	return info(key);
}
static bool checkbox_on(int i)
{
	return strncmp(checkbox(i), "[x]", 3) == 0;
}
static bool checkbox_disabled(int i, const char *why)
{
	const char *c = checkbox(i);
	return strstr(c, "(disabled)") != NULL && (!why || strstr(c, why) != NULL);
}
static bool tick(int i, bool on)
{
	char key[32];
	snprintf(key, sizeof(key), "split:%d", i);
	bool ok = pick(key, on ? "on" : "off");
	pump_for(150);
	dialog();
	return ok;
}
// A split list's line: "<key>\t<label>\t<shown>\t<choices>".
static const char *list_line(const char *key)
{
	return dropdown(key);
}
static const char *shown_in(const char *key)
{
	static char out[256];
	out[0] = 0;
	const char *line = list_line(key);
	// "<label>\t<shown>\t<choices>" after the key was cut off by info().
	const char *t1 = strchr(line, '\t');
	const char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
	if (t1 && t2)
		snprintf(out, sizeof(out), "%.*s", (int)(t2 - t1 - 1), t1 + 1);
	return out;
}
static bool offers(const char *key, const char *choice)
{
	const char *line = list_line(key);
	const char *t2 = strchr(line, '\t');
	t2 = t2 ? strchr(t2 + 1, '\t') : NULL;
	if (!t2)
		return false;
	char want[256];
	snprintf(want, sizeof(want), "|%s|", choice);
	char have[8192];
	snprintf(have, sizeof(have), "|%s|", t2 + 1);
	return strstr(have, want) != NULL;
}
static bool pick_split(const char *key, const char *choice)
{
	bool ok = pick(key, choice);
	pump_for(150);
	dialog();
	return ok;
}
static const char *settings_string(const char *key)
{
	static char out[256];
	obs_data_t *s = obs_source_get_settings(settings_source);
	snprintf(out, sizeof(out), "%s", obs_data_get_string(s, key));
	obs_data_release(s);
	return out;
}
static long long settings_int(const char *key)
{
	obs_data_t *s = obs_source_get_settings(settings_source);
	long long v = obs_data_get_int(s, key);
	obs_data_release(s);
	return v;
}

static void scenario_window(void)
{
	if (!start_as("split-window", "pro", "twitch,youtube", 4, NULL))
		return;
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *ytm = make("browser_source", "YT Music");
	obs_source_t *mic = make("wasapi_input_capture", "Mic");
	place(mic, "Voice");
	dialog();
	bool boxes = *checkbox(0) && *checkbox(1) && *checkbox(2) && !checkbox_on(0) && !checkbox_on(2) && strstr(checkbox(2), "Two sources: one per platform") && !checkbox_disabled(0, NULL) &&
		     !checkbox_disabled(2, NULL);
	result(boxes, "window 1 every fader has \"Two sources: one per platform\"", "Voice \"%s\" | Game \"%s\" | Music \"%s\"", checkbox(0), checkbox(1), checkbox(2));
	result(!*list_line("twitch:2") && !*list_line("youtube:2") && !*list_line("split_youtube_track"), "window 2 no source lists until it's ticked", "twitch \"%s\" youtube \"%s\" track \"%s\"",
	       list_line("twitch:2"), list_line("youtube:2"), list_line("split_youtube_track"));

	bool ticked = tick(2, true);
	result(ticked && checkbox_on(2) && !strcmp(settings_string("split_fader"), "Music") && checkbox_disabled(0, "Only one fader can do this.") && checkbox_disabled(1, "Only one fader can do this.") &&
		       !checkbox_disabled(2, NULL),
	       "window 3 ticked on Music: the others say only one", "ticked %d, saved \"%s\"; Voice \"%s\" | Game \"%s\" | Music \"%s\"", ticked, settings_string("split_fader"), checkbox(0), checkbox(1),
	       checkbox(2));
	// A tick on another fader changes nothing.
	tick(0, true);
	result(!strcmp(settings_string("split_fader"), "Music") && !checkbox_on(0), "window 4 a second split fader is refused", "saved \"%s\", Voice \"%s\", feedback \"%s\"", settings_string("split_fader"),
	       checkbox(0), info("move_feedback"));

	bool lists = !strcmp(shown_in("twitch:2"), "(none)") && !strcmp(shown_in("youtube:2"), "(none)") && offers("twitch:2", "Spotify") && offers("twitch:2", "YT Music") && offers("youtube:2", "Spotify") &&
		     !offers("twitch:2", "Mic") && strstr(list_line("twitch:2"), "Twitch source") && strstr(list_line("youtube:2"), "YouTube source");
	result(lists, "window 5 a Twitch source and a YouTube source list, each with (none)", "twitch [%s] youtube [%s]", list_line("twitch:2"), list_line("youtube:2"));
	const char *track = list_line("split_youtube_track");
	bool tracks = strstr(track, "Which OBS track goes to YouTube?") && strstr(track, "Track 1") && strstr(track, "Track 6") && !strstr(track, "Track 7") && !strncmp(shown_in("split_youtube_track"), "Pick", 4);
	result(tracks, "window 6 \"Which OBS track goes to YouTube?\": Track 1-6, none picked", "[%s]", track);
	result(strstr(info("split_info"), "Counts as 1 fader.") != NULL, "window 7 \"Counts as 1 fader.\"", "\"%s\"", info("split_info"));
	// Only Pro and Partner, both platforms, both sources and the track: says what's missing.
	result(strstr(info("split_info"), "pick") != NULL || strstr(info("split_info"), "Pick") != NULL, "window 8 says what's still missing", "\"%s\"", info("split_info"));

	bool p1 = pick_split("twitch:2", "Spotify");
	bool p2 = pick_split("youtube:2", "YT Music");
	bool p3 = pick_split("split_youtube_track", "Track 3");
	result(p1 && p2 && p3 && !strcmp(shown_in("twitch:2"), "Spotify") && !strcmp(shown_in("youtube:2"), "YT Music") && !strcmp(shown_in("split_youtube_track"), "Track 3") && settings_int("split_youtube_track") == 3 &&
		       !strcmp(saved(spotify), "Music") && !strcmp(tag(spotify), "twitch") && !strcmp(saved(ytm), "Music") && !strcmp(tag(ytm), "youtube"),
	       "window 9 picks are saved on the sources, the track in the settings", "Spotify %s/%s, YT Music %s/%s, track %lld; shown %s / %s / %s", saved(spotify), tag(spotify), saved(ytm), tag(ytm),
	       settings_int("split_youtube_track"), shown_in("twitch:2"), shown_in("youtube:2"), shown_in("split_youtube_track"));
	result(!strcmp(where(spotify), "Music|Music|ok|twitch") && !strcmp(where(ytm), "Music|Music|ok|youtube"), "window 10 both play on Music, one per platform", "Spotify %s, YT Music %s", where(spotify), where(ytm));

	// Status lines. make() puts sources on Track 1 only: YT Music isn't on Track 3, and it's on
	// the Twitch track as well.
	const char *s = info("split_status");
	result(strstr(s, "YouTube source isn't on Track 3.") != NULL, "window 11 \"YouTube source isn't on Track 3.\"", "\"%s\"", s);
	obs_source_set_audio_mixers(ytm, 4);
	obs_source_set_audio_mixers(spotify, 1 | 4);
	dialog();
	s = info("split_status");
	result(!strstr(s, "isn't on Track") && strstr(s, "Twitch source is also on the YouTube track: YouTube viewers will hear it twice.") != NULL, "window 12 \"Twitch source is also on the YouTube track\"",
	       "\"%s\"", s);
	obs_source_set_audio_mixers(spotify, 1);
	dialog();
	result(!*info("split_status") || !strstr(info("split_status"), "Track"), "window 13 no status line when both are right", "\"%s\"", info("split_status"));
	result(strstr(info("split_info"), "Music · Twitch") != NULL && strstr(info("split_info"), "Music · YouTube") != NULL, "window 14 viewers get \"Music · Twitch\" and \"Music · YouTube\"", "\"%s\"",
	       info("split_info"));

	// Unticking: the Twitch source stays on Music, the YouTube source goes to Everything else.
	tick(2, false);
	result(!checkbox_on(2) && !*settings_string("split_fader") && !strcmp(saved(spotify), "Music") && !*tag(spotify) && !strcmp(saved(ytm), "") && !*tag(ytm) && in_block("Music", "Spotify") &&
		       !checkbox_disabled(0, NULL),
	       "window 15 untick: Twitch source stays, YouTube source to Everything else", "Spotify %s/%s, YT Music \"%s\"/%s, Music [%s], Voice \"%s\"", saved(spotify), tag(spotify), saved(ytm), tag(ytm),
	       block("Music"), checkbox(0));
	drop(spotify);
	drop(ytm);
	drop(mic);
}

static void scenario_free(void)
{
	if (!start_as("split-free", "free", "twitch,youtube", 3, NULL))
		return;
	dialog();
	result(checkbox_disabled(0, "Needs Pro or Partner") && checkbox_disabled(1, "Needs Pro or Partner") && checkbox_disabled(2, "Needs Pro or Partner"), "free 1 disabled: \"Needs Pro or Partner\"",
	       "Voice \"%s\" | Music \"%s\"", checkbox(0), checkbox(2));
	tick(2, true);
	result(!*settings_string("split_fader") && !checkbox_on(2), "free 2 a tick on Free changes nothing", "saved \"%s\", feedback \"%s\"", settings_string("split_fader"), info("move_feedback"));
	set_account("partner", "twitch,youtube");
	dialog();
	bool ok = !checkbox_disabled(2, NULL) && tick(2, true) && !strcmp(settings_string("split_fader"), "Music");
	result(ok, "free 3 Partner may tick it", "Music \"%s\", saved \"%s\"", checkbox(2), settings_string("split_fader"));
}

static void scenario_noyoutube(void)
{
	if (!start_as("split-noyoutube", "pro", "twitch", 3, NULL))
		return;
	dialog();
	result(checkbox_disabled(2, "Connect YouTube to use this") && checkbox_disabled(0, "Connect YouTube to use this"), "noyoutube 1 disabled: \"Connect YouTube to use this\"", "Music \"%s\"", checkbox(2));
	tick(2, true);
	result(!*settings_string("split_fader"), "noyoutube 2 a tick changes nothing", "saved \"%s\"", settings_string("split_fader"));
	set_account("pro", "twitch,youtube");
	dialog();
	result(!checkbox_disabled(2, NULL), "noyoutube 3 enabled once YouTube is connected", "Music \"%s\"", checkbox(2));
}

static void scenario_onlytwo(void)
{
	if (!start_as("split-onlytwo", "pro", "twitch,youtube", 4, NULL))
		return;
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *ytm = make("browser_source", "YT Music");
	obs_source_t *clip = make("ffmpeg_source", "Clip");
	obs_source_t *other = make("ffmpeg_source", "Other");
	place(spotify, "Music");
	place(clip, "Music");
	dialog();
	tick(2, true);
	result(!*settings_string("split_fader") && strstr(info("move_feedback"), "Music") != NULL, "onlytwo 1 a fader with two sources can't be ticked", "saved \"%s\", feedback \"%s\"",
	       settings_string("split_fader"), info("move_feedback"));
	place_value(clip, "x:rest");
	dialog();
	tick(2, true);
	result(!strcmp(settings_string("split_fader"), "Music") && !strcmp(tag(spotify), "twitch") && !strcmp(shown_in("twitch:2"), "Spotify") && !strcmp(shown_in("youtube:2"), "(none)"),
	       "onlytwo 2 its one source becomes the Twitch source", "saved \"%s\", Spotify %s/%s, twitch list \"%s\"", settings_string("split_fader"), saved(spotify), tag(spotify), shown_in("twitch:2"));
	result(!*add_key("Music"), "onlytwo 3 no Add list on the split fader", "add key \"%s\"", add_key("Music"));
	bool moved = place_value(other, "f:Music");
	result(!moved && strcmp(saved(other), "(new)") == 0 && strstr(place_error, "Twitch") && strstr(place_error, "YouTube"), "onlytwo 4 moving another source onto it is refused, with why",
	       "taken %d, saved %s, \"%s\"", moved, saved(other), place_error);
	bool tw_by_value = place_value(ytm, "yt:Music");
	result(tw_by_value && !strcmp(saved(ytm), "Music") && !strcmp(tag(ytm), "youtube"), "onlytwo 5 yt:Music picks the YouTube source",
	       "taken %d (%s), YT Music %s/%s", tw_by_value, place_error, saved(ytm), tag(ytm));
	bool wrong = place_value(other, "tw:Voice");
	result(!wrong && strcmp(saved(other), "(new)") == 0, "onlytwo 6 a platform pick for another fader is refused", "taken %d \"%s\"", wrong, place_error);
	// A new Twitch pick replaces the old one, which goes to Everything else.
	dialog();
	pick_split("twitch:2", "Other");
	result(!strcmp(saved(other), "Music") && !strcmp(tag(other), "twitch") && !strcmp(saved(spotify), "") && !*tag(spotify) && rest_has("Spotify"), "onlytwo 7 a new Twitch pick replaces the old one",
	       "Other %s/%s, Spotify \"%s\"/%s; Everything else: %s", saved(other), tag(other), saved(spotify), tag(spotify), info("rest_info"));
	// (none) takes it off.
	pick_split("youtube:2", "(none)");
	result(!strcmp(saved(ytm), "") && !*tag(ytm) && !strcmp(shown_in("youtube:2"), "(none)"), "onlytwo 8 (none) puts the pick in Everything else", "YT Music \"%s\"/%s", saved(ytm), tag(ytm));
	// Moving a pick elsewhere drops its platform.
	place_value(other, "f:Voice");
	result(!strcmp(saved(other), "Voice") && !*tag(other), "onlytwo 9 a pick moved to another fader loses its platform", "Other %s/%s", saved(other), tag(other));
	drop(spotify);
	drop(ytm);
	drop(clip);
	drop(other);
}

static void setup_split(obs_source_t *tw, obs_source_t *yt, int track)
{
	dialog();
	tick(2, true);
	place_value(tw, "tw:Music");
	place_value(yt, "yt:Music");
	obs_data_t *change = obs_data_create();
	obs_data_set_int(change, "split_youtube_track", track);
	obs_source_update(settings_source, change);
	obs_data_release(change);
	pump_for(100);
	dialog();
}

static void scenario_roundtrip(void)
{
	if (!start_as("split-roundtrip", "pro", "twitch,youtube", 4, NULL))
		return;
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *ytm = make("browser_source", "YT Music");
	setup_split(spotify, ytm, 3);
	const char *file = settings_file();
	result(strstr(file, "\"split_fader\": \"Music\"") || strstr(file, "\"split_fader\":\"Music\""), "roundtrip 1 settings.json keeps the split fader", "%s", strstr(file, "split") ? strstr(file, "split") : file);
	result(strstr(file, "\"split_youtube_track\": 3") || strstr(file, "\"split_youtube_track\":3"), "roundtrip 2 settings.json keeps the YouTube track", "%s", strstr(file, "split_youtube") ? strstr(file, "split_youtube") : "-");
	// Saved and loaded with the scene collection.
	obs_data_t *a = obs_save_source(spotify), *b = obs_save_source(ytm);
	drop(spotify);
	drop(ytm);
	obs_wait_for_destroy_queue();
	spotify = obs_load_source(a);
	ytm = obs_load_source(b);
	obs_data_release(a);
	obs_data_release(b);
	dialog();
	result(spotify && ytm && !strcmp(where(spotify), "Music|Music|ok|twitch") && !strcmp(where(ytm), "Music|Music|ok|youtube") && !strcmp(shown_in("twitch:2"), "Spotify") && !strcmp(shown_in("youtube:2"), "YT Music"),
	       "roundtrip 3 save and load keep both picks", "Spotify %s, YT Music %s", where(spotify), where(ytm));
	// Duplicate: the copy can't be a second Twitch source; it plays in Everything else.
	obs_source_t *copy = obs_source_duplicate(spotify, "Spotify 2", false);
	dialog();
	result(copy && !strcmp(where(spotify), "Music|Music|ok|twitch") && !strcmp(where(copy), "Music||extra|twitch") && rest_has("Spotify 2"),
	       "roundtrip 4 a duplicate keeps the original's slot; the copy is extra", "Spotify %s, copy %s; Everything else: %s", where(spotify), copy ? where(copy) : "-", info("rest_info"));
	fetch_stats();
	result(!strcmp(mixer_fader_of("Spotify"), "Music") && !strcmp(mixer_fader_of("Spotify 2"), "") && !strcmp(mixer_fader_of("YT Music"), ""), "roundtrip 5 the mixer agrees (YouTube is mixed by the second tap)",
	       "Spotify \"%s\", copy \"%s\", YT Music \"%s\"", mixer_fader_of("Spotify"), mixer_fader_of("Spotify 2"), mixer_fader_of("YT Music"));
	drop(copy);
	drop(spotify);
	drop(ytm);
}

static void scenario_reload(void)
{
	if (!start_as("split-reload", "pro", "twitch,youtube", 4, ",\"split_fader\":\"Music\",\"split_youtube_track\":5"))
		return;
	obs_data_t *a = obs_data_create_from_json("{\"id\":\"ffmpeg_source\",\"name\":\"Tw\",\"settings\":{},\"private_settings\":{\"faders_fader\":\"Music\",\"faders_platform\":\"twitch\"}}");
	obs_data_t *b = obs_data_create_from_json("{\"id\":\"ffmpeg_source\",\"name\":\"Yt\",\"settings\":{},\"private_settings\":{\"faders_fader\":\"Music\",\"faders_platform\":\"youtube\"}}");
	obs_source_t *tw = obs_load_source(a), *yt = obs_load_source(b);
	obs_data_release(a);
	obs_data_release(b);
	dialog();
	result(checkbox_on(2) && !strcmp(shown_in("twitch:2"), "Tw") && !strcmp(shown_in("youtube:2"), "Yt") && !strcmp(shown_in("split_youtube_track"), "Track 5") && !strcmp(where(yt), "Music|Music|ok|youtube"),
	       "reload 1 a restart brings the split back", "Music \"%s\", twitch \"%s\", youtube \"%s\", track \"%s\", Yt %s", checkbox(2), shown_in("twitch:2"), shown_in("youtube:2"),
	       shown_in("split_youtube_track"), where(yt));
	// Renaming the fader keeps it split.
	configure("Voice,Game,Tunes", false);
	dialog();
	result(!strcmp(settings_string("split_fader"), "Tunes") && checkbox_on(2) && !strcmp(saved(tw), "Tunes") && !strcmp(tag(tw), "twitch"), "reload 2 renaming the split fader keeps it", "saved \"%s\", Tw %s/%s",
	       settings_string("split_fader"), saved(tw), tag(tw));
	drop(tw);
	drop(yt);
}

static void scenario_downgrade(void)
{
	if (!start_as("split-downgrade", "pro", "twitch,youtube", 4, NULL))
		return;
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *ytm = make("browser_source", "YT Music");
	obs_source_set_audio_mixers(ytm, 4);
	setup_split(spotify, ytm, 3);
	result(!strcmp(where(ytm), "Music|Music|ok|youtube"), "downgrade 1 on while Pro", "YT Music %s", where(ytm));
	set_account("free", "twitch,youtube");
	dialog();
	result(!strcmp(where(spotify), "Music|Music|ok|twitch") && !strcmp(where(ytm), "Music||parked|youtube") && !strcmp(tag(ytm), "youtube") && !strcmp(settings_string("split_fader"), "Music") &&
		       settings_int("split_youtube_track") == 3,
	       "downgrade 2 Free: off, Twitch source on the fader, YouTube source in Everything else, picks kept", "Spotify %s, YT Music %s, saved \"%s\" track %lld", where(spotify), where(ytm),
	       settings_string("split_fader"), settings_int("split_youtube_track"));
	result(checkbox_on(2) && strstr(info("split_info"), "Pro or Partner") != NULL && rest_has("YT Music"), "downgrade 3 the window says why and keeps the picks shown", "Music \"%s\"; \"%s\"; Everything else: %s",
	       checkbox(2), info("split_info"), info("rest_info"));
	set_account("pro", "twitch");
	dialog();
	result(!strcmp(where(ytm), "Music||parked|youtube") && strstr(info("split_info"), "YouTube") != NULL, "downgrade 4 YouTube disconnected: off too", "YT Music %s; \"%s\"", where(ytm), info("split_info"));
	set_account("pro", "twitch,youtube");
	dialog();
	result(!strcmp(where(ytm), "Music|Music|ok|youtube") && !strcmp(where(spotify), "Music|Music|ok|twitch"), "downgrade 5 upgrade restores it", "Spotify %s, YT Music %s", where(spotify), where(ytm));
	drop(spotify);
	drop(ytm);
}

static void scenario_counts(void)
{
	// Allowance 2: Voice and Music (split) are in; Game, third, is locked.
	char json[2048];
	snprintf(json, sizeof(json), PRO_SETTINGS, "pro", "twitch,youtube", 2, "");
	if (!start("split-counts", json))
		return;
	configure("Voice,Music,Game", false);
	obs_source_t *spotify = make("wasapi_process_output_capture", "Spotify");
	obs_source_t *ytm = make("browser_source", "YT Music");
	obs_source_t *game = make("game_capture", "Game Capture");
	place(game, "Game");
	dialog();
	tick(1, true);
	place_value(spotify, "tw:Music");
	place_value(ytm, "yt:Music");
	dialog();
	result(!strcmp(settings_string("split_fader"), "Music") && !strcmp(where(ytm), "Music||parked|youtube") && !strcmp(placement(game), "Game||locked") && strstr(info("split_info"), "Counts as 1 fader."),
	       "counts 1 the split fader is one of the plan's 2 faders", "split \"%s\", YT Music %s, Game %s, \"%s\"", settings_string("split_fader"), where(ytm), placement(game), info("split_info"));
	drop(spotify);
	drop(ytm);
	drop(game);
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	if (!strcmp(which, "window"))
		scenario_window();
	else if (!strcmp(which, "free"))
		scenario_free();
	else if (!strcmp(which, "noyoutube"))
		scenario_noyoutube();
	else if (!strcmp(which, "onlytwo"))
		scenario_onlytwo();
	else if (!strcmp(which, "roundtrip"))
		scenario_roundtrip();
	else if (!strcmp(which, "reload"))
		scenario_reload();
	else if (!strcmp(which, "downgrade"))
		scenario_downgrade();
	else if (!strcmp(which, "counts"))
		scenario_counts();
	else {
		fprintf(stderr, "usage: %s window|free|noyoutube|onlytwo|roundtrip|reload|downgrade|counts\n", argv[0]);
		return 2;
	}
	if (!settings_source) {
		result(false, "start", "the plugin didn't load");
	}
	return finish();
}
