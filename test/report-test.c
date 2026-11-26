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
// faders-report-test: the debug report (src/report.c, no OBS) built from a fixed snapshot, against
// golden files (test/report/names-on.md, names-off.md), plus: with names off no source, fader or
// channel name is left anywhere (lists, warnings, log, JSON); the report fails closed when the
// plugin key or the stream key would survive; this machine's system summary is filled in.
// Every key, name and address below is made up.
//   faders-report-test <dir with the golden files>
//   FADERS_UPDATE_GOLDEN=1 faders-report-test <dir>     (rewrite them after a deliberate change)
#include "../src/redact.c"
#include "../src/logring.c"
#include "../src/sysinfo.c"
#include "../src/report.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok)
		fails++;
}

#define PLUGIN_KEY "fdr_live_Q8zK3vT1mN7pR2xW9yB4cD6fG0hJ5kL1"
#define STREAM_KEY "live_918273645_Xy7Pq2Lm9Rt4Vb6Nc8Kd1Jf3"

static const struct fdr_report_fader faders[] = {
	{"Voice", true}, {"Game", true}, {"Music", true}, {"Discord", true}, {"Alerts", false},
};
static const struct fdr_report_source sources[] = {
	{"Mic/Aux", "wasapi_input_capture", "Voice", "Voice", "ok", false, "none", 0x3, true},
	{"Game Capture", "game_capture", "Game", "Game", "ok", false, "none", 0x5, true},
	{"Spotify", "wasapi_process_output_capture", "Music", "Music", "ok", false, "none", 0x5, true},
	{"Discord", "wasapi_process_output_capture", "Discord", "Discord", "ok", false, "monitor and output", 0x3, true},
	{"Alert Box (browser)", "browser_source", "Alerts", "", "locked", false, "none", 0x1, true},
	{"Jordan's mic", "coreaudio_input_capture", "rest", "", "rest", true, "none", 0x0, false},
	{"Tip jar (jordan.lee+obs@gmail.com)", "browser_source", "new", "", "new", false, "none", 0x1, true},
	{"Say \"hi\" \\ now\tplease", "ffmpeg_source", "new", "", "new", false, "monitor only", 0x1, true},
	{"Key test " STREAM_KEY, "ffmpeg_source", "Old fader", "", "deleted", false, "none", 0x1, true},
	{"Música 🎵", "ffmpeg_source", "Music", "", "dropped", false, "none", 0x1, true},
	{"YT Music", "browser_source", "Music", "Music", "ok", false, "none", 0x4, false},
};
#define N_SOURCES ((int)(sizeof(sources) / sizeof(*sources)))
static const struct fdr_log_line log_lines[] = {
	{1795988342, FDR_LOG_INFO, "loaded (version 0.4.0)", 1},
	{1795988591, FDR_LOG_INFO, "uplink started → wss://hub.faders.tv/ingest", 2},
	{1795988592, FDR_LOG_INFO, "connected to wss://hub.faders.tv/ingest", 3},
	{1795988592, FDR_LOG_INFO, "your plan includes 4 faders: \"Alerts\" and later are folded into Everything else", 4},
	{1795989087, FDR_LOG_INFO, "\"Spotify\" → Music", 5},
	{1795989422, FDR_LOG_WARNING, "disconnected: connect to 203.0.113.7:443 failed", 6},
	{1795989423, FDR_LOG_INFO, "retrying with key " PLUGIN_KEY, 7},
	{1795989704, FDR_LOG_WARNING, "\"Alert Box (browser)\" is in your stream (Track 1) more than once", 8},
	{1795989705, FDR_LOG_INFO, "settings in /Users/jordan/Library/Application Support/obs-studio", 9},
};

static struct fdr_report_input fixture(bool names)
{
	struct fdr_report_input in = {
		.names = names,
		.now = 1795992067, // 2026-11-29 22:41:07 UTC
		.utc_offset_min = -7 * 60,
		.plugin_version = "0.4.0",
		.plugin_uptime_s = 6125,
		.update_available = "",
		.config_dir = "/Users/jordan/Library/Application Support/obs-studio/plugin_config/faders",
		.module_dir = "/Library/Application Support/obs-studio/plugins/faders.plugin/Contents/MacOS",
		.obs_version = "32.2.2",
		.obs_locale = "en-US",
		.streaming = true,
		.stream_uptime_s = 2276,
		.stream_audio_track = 1,
		.sys = {"macOS", "15.1 (24B83)", "arm64", "Apple M3 Pro", 12, 12, 36},
		.sample_rate = 48000,
		.channels = 2,
		.mixer_ok = true,
		.enabled = true,
		.only_live = true,
		.plugin_key_set = true,
		.channel = "NightOwlPlays",
		.plan = "pro",
		.platforms = "twitch,youtube",
		.listener_limit = 50,
		.max_stems = 5,
		.running = true,
		.connected = true,
		.ever_connected = true,
		.server = "wss://hub.faders.tv/ingest",
		.listeners = 3,
		.uplink_kbps = 480,
		.stems_on_wire = 5,
		.stream_track = 1,
		.stream_track_silent = false,
		.chunks_sent = 4552,
		.last_failure = "connect to 203.0.113.7:443 failed",
		.rejected_status = 0,
		.mix_mismatch = "Música 🎵",
		.mix_duplicate = "Alert Box (browser)",
		.faders = faders,
		.n_faders = 5,
		.allowance = 4,
		.sources = sources,
		.n_sources = N_SOURCES,
		.audio_total = N_SOURCES,
		// Music has two sources, one per platform; Spotify is on YouTube's track too.
		.split = {"Music", true, "on", "Spotify", "YT Music", 3, "twitch-source-on-youtube-track"},
		.log = log_lines,
		.n_log = 9,
		.log_total = 9,
		.redact = {.plugin_key = PLUGIN_KEY, .stream_key = STREAM_KEY, .user_names = {"jordan", "Jordan Lee"}},
	};
	return in;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *s = malloc((size_t)n + 1);
	size_t got = fread(s, 1, (size_t)n, f);
	s[got] = 0;
	fclose(f);
	return s;
}

// The report equals the golden file (or rewrites it with FADERS_UPDATE_GOLDEN set).
static void golden(const char *dir, const char *file, const char *report, const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (getenv("FADERS_UPDATE_GOLDEN") && report) {
		FILE *f = fopen(path, "wb");
		fputs(report, f);
		fclose(f);
	}
	char *want = read_file(path);
	bool ok = want && report && strcmp(want, report) == 0;
	int line = 1;
	const char *a = want ? want : "", *b = report ? report : "";
	while (*a && *a == *b) {
		line += *a == '\n';
		a++, b++;
	}
	result(ok, name, ok ? "matches %s" : "differs from %s at line %d: want \"%.60s\" got \"%.60s\"", path, line, a, b);
	free(want);
}

// Whether `s` contains `needle` anywhere (the names-off check).
static bool has(const char *s, const char *needle)
{
	return s && needle && *needle && strstr(s, needle) != NULL;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "test/report";

	struct fdr_report_input on = fixture(true);
	char *with_names = NULL;
	int r_on = fdr_report_build(&on, &with_names);
	golden(dir, "names-on.md", with_names, "golden: names on (the default)");
	struct fdr_report_input off = fixture(false);
	char *no_names = NULL;
	int r_off = fdr_report_build(&off, &no_names);
	golden(dir, "names-off.md", no_names, "golden: names off");

	// Names on: every name is there, the secrets are not.
	bool on_ok = r_on == 0 && has(with_names, "NightOwlPlays") && has(with_names, "Mic/Aux") && has(with_names, "\"Voice\"") && !has(with_names, PLUGIN_KEY) && !has(with_names, STREAM_KEY) &&
		     !has(with_names, "jordan") && !has(with_names, "203.0.113.7") && has(with_names, "Nothing was uploaded") && has(with_names, "your plugin key");
	result(on_ok, "names on: names in, keys, user name, email and IP out", "%s", on_ok ? "" : "see the golden file");

	// Names off: no source, fader or channel name anywhere: lists, problems, log lines, JSON.
	char why[1024] = "";
	const char *names[] = {"NightOwlPlays", "Voice", "Game", "Music", "Discord", "Alerts", "Old fader", "Mic/Aux", "Game Capture", "Spotify", "Alert Box", "Jordan", "Tip jar", "Say ", "Key test", "Música", "🎵", "YT Music"};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (has(no_names, names[i]))
			snprintf(why + strlen(why), sizeof(why) - strlen(why), "\"%s\" ", names[i]);
	bool off_ok = r_off == 0 && !*why && has(no_names, "Source 1") && has(no_names, "Fader 1") && has(no_names, "[channel]") && !has(no_names, PLUGIN_KEY) && !has(no_names, STREAM_KEY);
	result(off_ok, "names off: no name is left anywhere", "%s%s", *why ? "still there: " : "", why);

	// The split: one line, and the same facts in the JSON, names on and off.
	bool split_on = has(with_names, "- **Per-platform fader**: Music, on · Twitch: Spotify · YouTube: YT Music on Track 3 · Twitch source is also on the YouTube track") &&
			has(with_names, "\"split\": {\"fader\": \"Music\", \"ticked\": true, \"on\": true, \"why\": \"on\", \"twitch_source\": \"Spotify\", \"youtube_source\": \"YT Music\", \"youtube_track\": 3, \"warnings\": [\"twitch-source-on-youtube-track\"]}");
	bool split_off_names = has(no_names, "- **Per-platform fader**: Fader 3, on · Twitch: Source 3 · YouTube: Source 11 on Track 3") &&
			       has(no_names, "\"twitch_source\": \"Source 3\", \"youtube_source\": \"Source 11\"");
	result(split_on && split_off_names, "split: one line and the JSON, names on and off", "names on %d, names off %d", split_on, split_off_names);
	// Off (downgraded): why, and the YouTube source waits in Everything else.
	struct fdr_report_source parked[N_SOURCES];
	memcpy(parked, sources, sizeof(parked));
	parked[N_SOURCES - 1].plays = "";
	parked[N_SOURCES - 1].state = "parked";
	struct fdr_report_input down = fixture(true);
	down.plan = "free";
	down.sources = parked;
	down.split = (struct fdr_report_split){"Music", false, "plan", "Spotify", "YT Music", 3, ""};
	char *down_report = NULL;
	fdr_report_build(&down, &down_report);
	result(has(down_report, "- **Per-platform fader**: Music, off (needs Pro or Partner) · Twitch: Spotify · YouTube: YT Music on Track 3\n") &&
		       has(down_report, "YT Music (Music's YouTube source, until it's on)") && has(down_report, "\"on\": false, \"why\": \"plan\"") && !has(down_report, "**Music**: Spotify, YT Music"),
	       "split off: why, and the YouTube source in Everything else", "%s", "");
	free(down_report);
	struct fdr_report_input none = fixture(true);
	none.split = (struct fdr_report_split){"", false, "none", "", "", 0, ""};
	char *none_report = NULL;
	fdr_report_build(&none, &none_report);
	result(has(none_report, "- **Per-platform fader**: none\n") && has(none_report, "\"split\": {\"fader\": \"\", \"ticked\": false, \"on\": false, \"why\": \"none\""), "no split: says none", "%s", "");
	free(none_report);

	// The same snapshot gives the same bytes.
	char *again = NULL;
	fdr_report_build(&on, &again);
	result(again && with_names && strcmp(again, with_names) == 0, "deterministic", "");
	free(again);

	// Fail closed: a key in a field that isn't redacted (typed fields are copied as they are)
	// would survive; the report is refused rather than shown.
	struct fdr_report_source leaky[N_SOURCES];
	memcpy(leaky, sources, sizeof(leaky));
	leaky[1].kind = "x_" PLUGIN_KEY;
	struct fdr_report_input bad = fixture(true);
	bad.sources = leaky;
	char *refused = (char *)"untouched";
	int r_bad = fdr_report_build(&bad, &refused);
	struct fdr_report_input bad2 = fixture(false);
	bad2.obs_version = "32.2.2-" STREAM_KEY;
	char *refused2 = (char *)"untouched";
	int r_bad2 = fdr_report_build(&bad2, &refused2);
	// The split's warnings are typed (copied as they are): a key there is refused too.
	struct fdr_report_input bad3 = fixture(true);
	bad3.split.warnings = "x-" PLUGIN_KEY;
	char *refused3 = (char *)"untouched";
	int r_bad3 = fdr_report_build(&bad3, &refused3);
	result(r_bad != 0 && refused == NULL && r_bad2 != 0 && refused2 == NULL && r_bad3 != 0 && refused3 == NULL, "fails closed when a key would survive", "plugin key: %d %s; stream key: %d %s; split warnings: %d %s",
	       r_bad, refused ? "TEXT RETURNED" : "no text", r_bad2, refused2 ? "TEXT RETURNED" : "no text", r_bad3, refused3 ? "TEXT RETURNED" : "no text");

	// Many sources: the list stops at 200 and says so; the report stays small.
	static struct fdr_report_source many[1000];
	static char many_names[1000][32];
	for (int i = 0; i < 1000; i++) {
		snprintf(many_names[i], sizeof(many_names[i]), "Overlay %d", i + 1);
		many[i] = (struct fdr_report_source){many_names[i], "browser_source", "new", "", "new", false, "none", 1, true};
	}
	struct fdr_report_input big = fixture(true);
	big.sources = many;
	big.n_sources = 1000;
	big.audio_total = 1000;
	char *big_report = NULL;
	fdr_report_build(&big, &big_report);
	size_t big_len = big_report ? strlen(big_report) : 0;
	result(big_report && has(big_report, "\"audio_total\": 1000") && has(big_report, "\"listed\": 200") && has(big_report, "\"truncated\": true") && has(big_report, "Overlay 200\"") &&
		       !has(big_report, "Overlay 201\"") && big_len < 64 * 1024,
	       "1000 sources: 200 listed, under 64 KB", "%zu bytes", big_len);
	free(big_report);

	// This machine's system summary.
	struct fdr_sysinfo sys;
	fdr_sysinfo_get(&sys);
	result(*sys.os && *sys.os_version && *sys.arch && sys.cores_logical > 0 && sys.cores_physical > 0 && sys.ram_gb > 0, "system summary of this machine", "%s %s, %s, %s, %d cores / %d threads, %d GB", sys.os,
	       sys.os_version, sys.arch, sys.cpu, sys.cores_physical, sys.cores_logical, sys.ram_gb);

	free(with_names);
	free(no_names);
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
