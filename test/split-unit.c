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
// faders-split-unit <golden dir>: the split fader's model on its own (src/split.c, no OBS): who may
// tick it, what it holds, when it is on, the track warnings, and the stems and status JSON against
// golden files (test/split/). One PASS/FAIL line per check; exits non-zero if any fails.
#include "../src/placement.c"
#include "../src/split.c"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok)
		fails++;
}

static const char *golden_dir;
static char *golden(const char *file)
{
	static char buf[8192];
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", golden_dir, file);
	FILE *f = fopen(path, "rb");
	buf[0] = 0;
	if (!f)
		return buf;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	return buf;
}

static const char *const NAMES[] = {"Voice", "Game", "Music", "Discord"};
static const struct fdr_fader_list FADERS = {NAMES, 4, 4};
static const struct fdr_account FREE = {"free", true, true}, PRO = {"pro", true, true}, PARTNER = {"partner", true, true}, PRO_NO_YT = {"pro", true, false},
				   PRO_NO_TW = {"pro", false, true}, UNKNOWN = {"", false, false};

static void gate(void)
{
	struct {
		const struct fdr_account *a;
		const char *split, *fader;
		enum fdr_split_gate want;
		const char *why;
	} cases[] = {
		{&FREE, "", "Music", FDR_SPLIT_GATE_PLAN, "Free: needs Pro or Partner"},
		{&UNKNOWN, "", "Music", FDR_SPLIT_GATE_PLAN, "plan not known yet: needs Pro or Partner"},
		{&PRO, "", "Music", FDR_SPLIT_GATE_OK, "Pro with both platforms"},
		{&PARTNER, "", "Voice", FDR_SPLIT_GATE_OK, "Partner with both platforms"},
		{&PRO_NO_YT, "", "Music", FDR_SPLIT_GATE_YOUTUBE, "Pro without YouTube"},
		{&PRO_NO_TW, "", "Music", FDR_SPLIT_GATE_YOUTUBE, "Pro without Twitch"},
		{&PRO, "Music", "Voice", FDR_SPLIT_GATE_OTHER, "another fader has it: only one"},
		{&PRO, "music", "Music", FDR_SPLIT_GATE_OK, "the split fader itself (any case)"},
		{&PRO, "Gone", "Voice", FDR_SPLIT_GATE_OK, "a saved split fader that no longer exists doesn't block"},
		{&FREE, "Music", "Voice", FDR_SPLIT_GATE_PLAN, "Free says the plan first"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		enum fdr_split_gate g = fdr_split_gate(cases[i].a, &FADERS, cases[i].split, cases[i].fader);
		char name[128];
		snprintf(name, sizeof(name), "gate: %s", cases[i].why);
		result(g == cases[i].want, name, "\"%s\" (want \"%s\")", fdr_split_gate_text(g), fdr_split_gate_text(cases[i].want));
	}
	result(!strcmp(fdr_split_gate_text(FDR_SPLIT_GATE_PLAN), "Needs Pro or Partner") && !strcmp(fdr_split_gate_text(FDR_SPLIT_GATE_YOUTUBE), "Connect YouTube to use this") &&
		       !strcmp(fdr_split_gate_text(FDR_SPLIT_GATE_OTHER), "Only one fader can do this."),
	       "gate: the window's words", "\"%s\" / \"%s\" / \"%s\"", fdr_split_gate_text(FDR_SPLIT_GATE_PLAN), fdr_split_gate_text(FDR_SPLIT_GATE_YOUTUBE), fdr_split_gate_text(FDR_SPLIT_GATE_OTHER));
}

static void tick_and_moves(void)
{
	result(fdr_split_check_tick(&PRO, &FADERS, "", "Music", 0) == FDR_SPLIT_OK && fdr_split_check_tick(&PRO, &FADERS, "", "Music", 1) == FDR_SPLIT_OK,
	       "tick: an empty fader or one with one source", "ok");
	enum fdr_split_reject r = fdr_split_check_tick(&PRO, &FADERS, "", "Music", 2);
	result(r == FDR_SPLIT_REJECT_FULL, "tick: a fader with two sources is refused", "%s", fdr_split_reject_name(r));
	r = fdr_split_check_tick(&PRO, &FADERS, "Music", "Voice", 0);
	result(r == FDR_SPLIT_REJECT_OTHER, "tick: a second split fader is refused", "%s", fdr_split_reject_name(r));
	r = fdr_split_check_tick(&FREE, &FADERS, "", "Music", 0);
	enum fdr_split_reject r2 = fdr_split_check_tick(&PRO_NO_YT, &FADERS, "", "Music", 0);
	result(r == FDR_SPLIT_REJECT_PLAN && r2 == FDR_SPLIT_REJECT_YOUTUBE, "tick: Free refused; no YouTube refused", "%s / %s", fdr_split_reject_name(r), fdr_split_reject_name(r2));
	enum fdr_split_reject rp = fdr_split_check_tick(&PARTNER, &FADERS, "", "Discord", 0);
	result(rp == FDR_SPLIT_OK, "tick: Partner allowed", "%s", fdr_split_reject_name(rp));
	const char *const locked_names[] = {"Voice", "Music"};
	struct fdr_fader_list locked = {locked_names, 2, 1};
	r = fdr_split_check_tick(&PRO, &locked, "", "Music", 0);
	result(r == FDR_SPLIT_REJECT_MISSING, "tick: a locked fader can't be split", "%s", fdr_split_reject_name(r));

	// Only the two platform sources.
	result(fdr_split_check_move("Music", "music") == FDR_SPLIT_REJECT_ONLY_TWO && fdr_split_check_move("Music", "Voice") == FDR_SPLIT_OK && fdr_split_check_move("", "Music") == FDR_SPLIT_OK,
	       "move: nothing else goes onto the split fader", "to split %s, to Voice %s, no split %s", fdr_split_reject_name(fdr_split_check_move("Music", "music")),
	       fdr_split_reject_name(fdr_split_check_move("Music", "Voice")), fdr_split_reject_name(fdr_split_check_move("", "Music")));
	char v[64], f[64];
	enum fdr_platform p;
	bool tw = fdr_split_value_encode(FDR_PLATFORM_TWITCH, "Music", v, sizeof(v)) && !strcmp(v, "tw:Music") && fdr_split_value_decode(v, &p, f, sizeof(f)) && p == FDR_PLATFORM_TWITCH && !strcmp(f, "Music");
	bool yt = fdr_split_value_encode(FDR_PLATFORM_YOUTUBE, "x:rest", v, sizeof(v)) && !strcmp(v, "yt:x:rest") && fdr_split_value_decode(v, &p, f, sizeof(f)) && p == FDR_PLATFORM_YOUTUBE && !strcmp(f, "x:rest");
	bool bad = !fdr_split_value_decode("f:Music", &p, f, sizeof(f)) && !fdr_split_value_decode("tw:", &p, f, sizeof(f)) && !fdr_split_value_decode("x:rest", &p, f, sizeof(f));
	result(tw && yt && bad, "pick values: tw:<fader>, yt:<fader>", "twitch %d youtube %d bad refused %d", tw, yt, bad);
	r = fdr_split_check_pick(&FADERS, "Music", "tw:music", &p, f, sizeof(f));
	result(r == FDR_SPLIT_OK && p == FDR_PLATFORM_TWITCH && !strcmp(f, "Music"), "pick: Twitch source for the split fader", "%s \"%s\"", fdr_split_reject_name(r), f);
	r = fdr_split_check_pick(&FADERS, "Music", "yt:Voice", &p, f, sizeof(f));
	r2 = fdr_split_check_pick(&FADERS, "", "yt:Music", &p, f, sizeof(f));
	enum fdr_split_reject r3 = fdr_split_check_pick(&FADERS, "Gone", "yt:Gone", &p, f, sizeof(f));
	result(r == FDR_SPLIT_REJECT_NOT_SPLIT && r2 == FDR_SPLIT_REJECT_NOT_SPLIT && r3 == FDR_SPLIT_REJECT_MISSING, "pick: only for the split fader, which must exist", "%s / %s / %s",
	       fdr_split_reject_name(r), fdr_split_reject_name(r2), fdr_split_reject_name(r3));
}

static struct fdr_split_state eval(const struct fdr_account *a, const char *split, int track, bool tw, bool yt, const struct fdr_fader_list *l)
{
	struct fdr_split_input in = {l ? l : &FADERS, split, track, *a, tw, yt};
	struct fdr_split_state s;
	fdr_split_evaluate(&in, &s);
	return s;
}

static void effective(void)
{
	struct {
		const struct fdr_account *a;
		const char *split;
		int track;
		bool tw, yt;
		bool on;
		enum fdr_split_why why;
		const char *name;
	} cases[] = {
		{&PRO, "Music", 3, true, true, true, FDR_SPLIT_ON, "Pro, both sources, Track 3: on"},
		{&PARTNER, "music", 2, true, true, true, FDR_SPLIT_ON, "Partner: on"},
		{&FREE, "Music", 3, true, true, false, FDR_SPLIT_PLAN, "Free: off"},
		{&UNKNOWN, "Music", 3, true, true, false, FDR_SPLIT_PLAN, "plan not known: off"},
		{&PRO_NO_YT, "Music", 3, true, true, false, FDR_SPLIT_NO_YOUTUBE, "YouTube not connected: off"},
		{&PRO, "Music", 3, true, false, false, FDR_SPLIT_NO_YOUTUBE_SOURCE, "only the Twitch source: off"},
		{&PRO, "Music", 3, false, true, false, FDR_SPLIT_NO_TWITCH_SOURCE, "only the YouTube source: off"},
		{&PRO, "Music", 0, true, true, false, FDR_SPLIT_NO_TRACK, "no YouTube track picked: off"},
		{&PRO, "Music", 7, true, true, false, FDR_SPLIT_NO_TRACK, "track out of range: off"},
		{&PRO, "", 3, true, true, false, FDR_SPLIT_NONE, "no split fader"},
		{&PRO, "Gone", 3, true, true, false, FDR_SPLIT_NONE, "split fader deleted"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		struct fdr_split_state s = eval(cases[i].a, cases[i].split, cases[i].track, cases[i].tw, cases[i].yt, NULL);
		char name[128];
		snprintf(name, sizeof(name), "effective: %s", cases[i].name);
		result(s.on == cases[i].on && s.why == cases[i].why && (cases[i].why == FDR_SPLIT_NONE ? s.fader == -1 : s.fader == 2), name, "on %d why %s fader %d", s.on, fdr_split_why_name(s.why), s.fader);
	}
	const char *const locked_names[] = {"Voice", "Game", "Music"};
	struct fdr_fader_list locked = {locked_names, 3, 2};
	struct fdr_split_state s = eval(&PRO, "Music", 3, true, true, &locked);
	result(!s.on && s.why == FDR_SPLIT_LOCKED, "effective: past the plan's allowance: off", "on %d why %s", s.on, fdr_split_why_name(s.why));
	// 14 faders + 3 layers > 16.
	const char *const many[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n"};
	struct fdr_fader_list l14 = {many, 14, 14}, l13 = {many, 13, 13};
	struct fdr_split_state s14 = eval(&PRO, "a", 1, true, true, &l14), s13 = eval(&PRO, "a", 1, true, true, &l13);
	result(!s14.on && s14.why == FDR_SPLIT_TOO_MANY && s13.on, "effective: the wire's 16 layers hold 13 faders + split", "14: %s, 13: %s", fdr_split_why_name(s14.why), fdr_split_why_name(s13.why));

	// The hub's splitMaxFaders (quality message): more faders on the wire than that, and the split
	// is off (late joiners would get too little catch-up); absent (0) is no limit.
	struct fdr_split_input q3 = {&FADERS, "Music", 3, PRO, true, true, 3}, q4 = {&FADERS, "Music", 3, PRO, true, true, 4}, q0 = {&FADERS, "Music", 3, PRO, true, true, 0};
	struct fdr_split_state s_q3, s_q4, s_q0;
	fdr_split_evaluate(&q3, &s_q3);
	fdr_split_evaluate(&q4, &s_q4);
	fdr_split_evaluate(&q0, &s_q0);
	const char *const capped[] = {"Voice", "Game", "Music", "Discord"};
	struct fdr_fader_list l2 = {capped, 4, 2}; // the plan sends 2: the split counts as one of them
	struct fdr_split_input q_plan = {&l2, "Game", 3, PRO, true, true, 2};
	struct fdr_split_state s_qp;
	fdr_split_evaluate(&q_plan, &s_qp);
	result(!s_q3.on && s_q3.why == FDR_SPLIT_QUALITY && !strcmp(fdr_split_why_name(s_q3.why), "quality") && s_q4.on && s_q0.on && s_qp.on,
	       "splitMaxFaders: 4 faders over a max of 3 is off; 4, none, and 2 on the wire are on", "max 3: %s, max 4: %s, none: %s, 2 of 4 on the wire with max 2: %s", fdr_split_why_name(s_q3.why),
	       fdr_split_why_name(s_q4.why), fdr_split_why_name(s_q0.why), fdr_split_why_name(s_qp.why));
	enum fdr_split_role yt_q = fdr_split_role(&s_q3, "Music", "Music", FDR_PLATFORM_YOUTUBE, true);
	result(yt_q == FDR_SPLIT_ROLE_PARKED, "splitMaxFaders: off keeps the picks (YouTube source parked)", "%s", fdr_split_role_name(yt_q));

	// Counts as one fader: the plan counts faders; the wire carries two more layers.
	result(fdr_split_wire_stems(4, false) == 5 && fdr_split_wire_stems(4, true) == 7 && fdr_split_wire_stems(3, true) == 6, "counts as 1 fader (4 faders: 5 layers, 7 split)",
	       "%d / %d / %d", fdr_split_wire_stems(4, false), fdr_split_wire_stems(4, true), fdr_split_wire_stems(3, true));

	// Downgrade turns it off and keeps the picks; upgrade restores it (nothing saved changes:
	// the same input with the plan back is on again).
	struct fdr_split_state up = eval(&PRO, "Music", 3, true, true, NULL), down = eval(&FREE, "Music", 3, true, true, NULL), back = eval(&PRO, "Music", 3, true, true, NULL);
	enum fdr_split_role yt_down = fdr_split_role(&down, "Music", "Music", FDR_PLATFORM_YOUTUBE, true), tw_down = fdr_split_role(&down, "Music", "Music", FDR_PLATFORM_TWITCH, true);
	enum fdr_split_role yt_up = fdr_split_role(&back, "Music", "Music", FDR_PLATFORM_YOUTUBE, true), tw_up = fdr_split_role(&back, "Music", "Music", FDR_PLATFORM_TWITCH, true);
	result(up.on && !down.on && back.on && yt_down == FDR_SPLIT_ROLE_PARKED && tw_down == FDR_SPLIT_ROLE_TWITCH && yt_up == FDR_SPLIT_ROLE_YOUTUBE && tw_up == FDR_SPLIT_ROLE_TWITCH,
	       "downgrade parks the YouTube source; upgrade restores", "on %d → %d → %d; YouTube %s → %s, Twitch %s → %s", up.on, down.on, back.on, fdr_split_role_name(yt_down), fdr_split_role_name(yt_up),
	       fdr_split_role_name(tw_down), fdr_split_role_name(tw_up));
	// Only the two: an untagged source, or a second one of a platform, plays in Everything else.
	enum fdr_split_role untagged = fdr_split_role(&up, "Music", "Music", FDR_PLATFORM_ALL, false), second = fdr_split_role(&up, "Music", "music", FDR_PLATFORM_TWITCH, false),
			    other = fdr_split_role(&up, "Music", "Voice", FDR_PLATFORM_ALL, false), stale = fdr_split_role(&up, "Music", "Voice", FDR_PLATFORM_YOUTUBE, false),
			    none = fdr_split_role(&up, "", "Music", FDR_PLATFORM_TWITCH, true);
	result(untagged == FDR_SPLIT_ROLE_EXTRA && second == FDR_SPLIT_ROLE_EXTRA && other == FDR_SPLIT_ROLE_NONE && stale == FDR_SPLIT_ROLE_NONE && none == FDR_SPLIT_ROLE_NONE,
	       "the split fader holds only its two sources", "untagged %s, second Twitch %s, on Voice %s, stale tag on Voice %s, no split fader %s", fdr_split_role_name(untagged), fdr_split_role_name(second),
	       fdr_split_role_name(other), fdr_split_role_name(stale), fdr_split_role_name(none));
}

static void warnings(void)
{
	struct {
		uint32_t tw, yt;
		int stream, youtube;
		unsigned want;
		const char *name;
	} cases[] = {
		{1, 4, 1, 3, 0, "each on its own track: no warning"},
		{1, 1, 1, 3, FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK | FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK, "YouTube source on Track 1 only"},
		{1 | 4, 4, 1, 3, FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK, "Twitch source also on the YouTube track"},
		{2, 4, 1, 3, FDR_SPLIT_WARN_TWITCH_NOT_ON_TRACK, "Twitch source not on Track 1"},
		{1, 4 | 1, 1, 3, FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK, "YouTube source also on the Twitch track"},
		{0x3f, 0x3f, 2, 2, FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK | FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK, "one track for both: both doubled"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		unsigned w = fdr_split_track_warnings(cases[i].tw, cases[i].yt, cases[i].stream, cases[i].youtube);
		char name[128];
		snprintf(name, sizeof(name), "warnings: %s", cases[i].name);
		result(w == cases[i].want, name, "0x%x (want 0x%x)", w, cases[i].want);
	}
	const char *want[] = {"twitch-source-not-on-track", "youtube-source-not-on-track", "twitch-source-on-youtube-track", "youtube-source-on-twitch-track", "youtube-track-silent"};
	bool codes = true;
	for (int b = 0; b < FDR_SPLIT_WARN_COUNT; b++)
		codes = codes && !strcmp(fdr_split_warning_code(1u << b), want[b]);
	result(codes, "warnings: the wire's codes", "%s %s %s %s %s", fdr_split_warning_code(1), fdr_split_warning_code(2), fdr_split_warning_code(4), fdr_split_warning_code(8), fdr_split_warning_code(16));
}

static void wire(void)
{
	char json[4096], id[16];
	struct fdr_stem_desc stems[FDR_SPLIT_STEMS_MAX];
	const char *const three[] = {"Voice", "Game", "Music"};
	// No split: exactly today's message (golden captured from the plugin before this change).
	int n = fdr_split_layout(three, 3, -1, stems, id, sizeof(id));
	size_t len = fdr_stems_json(json, sizeof(json), stems, n);
	result(n == 4 && len == strlen(json) && !strcmp(json, golden("stems-off.json")), "stems JSON golden: split off (byte-identical)", "%s", json);
	// Split on Music: PROTOCOL-SPLIT.md §2's example, byte for byte.
	n = fdr_split_layout(three, 3, 2, stems, id, sizeof(id));
	fdr_stems_json(json, sizeof(json), stems, n);
	result(n == 6 && !strcmp(json, golden("stems-on.json")), "stems JSON golden: split on", "%s", json);
	// The split fader first, and names that need escaping.
	const char *const odd[] = {"Music", "Voice", "Say \"hi\" \\ me"};
	n = fdr_split_layout(odd, 3, 0, stems, id, sizeof(id));
	fdr_stems_json(json, sizeof(json), stems, n);
	result(n == 6 && !strcmp(json, golden("stems-on-first.json")), "stems JSON golden: split fader first, escaped names", "%s", json);
	// 13 faders + split = 16 layers; the JSON fits.
	const char *const many[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"};
	n = fdr_split_layout(many, 13, 12, stems, id, sizeof(id));
	len = fdr_stems_json(json, sizeof(json), stems, n);
	result(n == 16 && len > 0 && stems[12].platform == FDR_PLATFORM_TWITCH && stems[13].platform == FDR_PLATFORM_YOUTUBE && stems[14].rest && stems[15].rest, "stems: 13 faders + split = 16 layers",
	       "%d stems", n);
	// The status message's split object.
	char st[512];
	len = fdr_split_status_json(st, sizeof(st), 3, FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK | FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK);
	result(len == strlen(st) && !strcmp(st, golden("status-split.json")), "status JSON golden: split warnings", "%s", st);
	fdr_split_status_json(st, sizeof(st), 2, 0);
	result(!strcmp(st, ",\"split\":{\"youtubeTrack\":2,\"warnings\":[]}"), "status JSON: no warnings", "%s", st);
	result(fdr_stems_json(json, 40, stems, n) == 0, "stems JSON: refuses to cut a message short", "ok");
}

int main(int argc, char **argv)
{
	golden_dir = argc > 1 ? argv[1] : "test/split";
	gate();
	tick_and_moves();
	effective();
	warnings();
	wire();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
