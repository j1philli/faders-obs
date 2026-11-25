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
// faders-placement-unit: the placement model on its own (src/placement.c, no OBS): the value
// codec, saved vs effective placement, and which destinations a move may use.
// One PASS/FAIL line per check; exits non-zero if any fails.
#include "../src/placement.c"

#include <stdio.h>
#include <stdarg.h>

static int fails;
static void result(bool ok, const char *name, const char *fmt, ...)
{
	char detail[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok)
		fails++;
}

static const char *kind_name(enum fdr_value k)
{
	switch (k) {
	case FDR_VALUE_FADER:
		return "fader";
	case FDR_VALUE_REST:
		return "rest";
	case FDR_VALUE_NEW:
		return "new";
	case FDR_VALUE_KEEP:
		return "keep";
	default:
		return "invalid";
	}
}

static void codec(void)
{
	// Round trips: any fader name, including ones that look like the plugin's own values.
	const char *names[] = {"Voice", "~new", "~rest", "x:rest", "x:new", "f:Voice", "f:", "Música 🎵", "Everything elsewhere", "  spaced  "};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
		char v[128], back[128] = "";
		bool enc = fdr_value_encode(FDR_VALUE_FADER, names[i], v, sizeof(v));
		enum fdr_value k = fdr_value_decode(v, back, sizeof(back));
		char name[96];
		snprintf(name, sizeof(name), "codec round trip \"%s\"", names[i]);
		result(enc && k == FDR_VALUE_FADER && strcmp(back, names[i]) == 0, name, "encoded \"%s\" → %s \"%s\"", v, kind_name(k), back);
	}
	struct {
		const char *in;
		enum fdr_value want;
		const char *name;
	} cases[] = {
		{"x:rest", FDR_VALUE_REST, ""}, {"x:new", FDR_VALUE_NEW, ""}, {"x:keep", FDR_VALUE_KEEP, ""},
		{"f:x:rest", FDR_VALUE_FADER, "x:rest"}, {"f:~new", FDR_VALUE_FADER, "~new"},
		{"", FDR_VALUE_INVALID, ""}, {"f:", FDR_VALUE_INVALID, ""}, {"~new", FDR_VALUE_INVALID, ""}, {"~rest", FDR_VALUE_INVALID, ""},
		{"Voice", FDR_VALUE_INVALID, ""}, {"x:", FDR_VALUE_INVALID, ""}, {"x:REST", FDR_VALUE_INVALID, ""}, {"x:other", FDR_VALUE_INVALID, ""},
		{"F:Voice", FDR_VALUE_INVALID, ""}, {NULL, FDR_VALUE_INVALID, ""},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		char back[128] = "";
		enum fdr_value k = fdr_value_decode(cases[i].in, back, sizeof(back));
		char name[96];
		snprintf(name, sizeof(name), "codec decode \"%s\"", cases[i].in ? cases[i].in : "(null)");
		result(k == cases[i].want && strcmp(back, cases[i].name) == 0, name, "%s \"%s\" (want %s \"%s\")", kind_name(k), back, kind_name(cases[i].want), cases[i].name);
	}
	char v[8];
	result(!fdr_value_encode(FDR_VALUE_FADER, "A name too long for it", v, sizeof(v)) && !fdr_value_encode(FDR_VALUE_FADER, "", v, sizeof(v)) && fdr_value_encode(FDR_VALUE_REST, NULL, v, sizeof(v)) &&
		       strcmp(v, "x:rest") == 0,
	       "codec refuses what it can't encode", "too long / empty fader name refused; x:rest \"%s\"", v);
}

static void model(void)
{
	const char *const names[] = {"Voice", "Game", "~new", "Alerts"};
	// The plan allows 3: Alerts is locked.
	struct fdr_fader_list l = {names, 4, 3};
	enum fdr_state st;
	int idx;
	struct {
		enum fdr_saved saved;
		const char *name;
		bool dropped;
		int want_idx;
		enum fdr_state want;
		const char *why;
	} cases[] = {
		{FDR_SAVED_NEW, NULL, false, -1, FDR_STATE_NEW, "never placed: NEW, Everything else"},
		{FDR_SAVED_REST, NULL, false, -1, FDR_STATE_REST, "placed in Everything else"},
		{FDR_SAVED_FADER, "Voice", false, 0, FDR_STATE_ON_FADER, "on Voice"},
		{FDR_SAVED_FADER, "voice", false, 0, FDR_STATE_ON_FADER, "stored in another case"},
		{FDR_SAVED_FADER, "~new", false, 2, FDR_STATE_ON_FADER, "a fader named ~new is a fader"},
		{FDR_SAVED_FADER, "Alerts", false, -1, FDR_STATE_LOCKED, "past the plan: locked"},
		{FDR_SAVED_FADER, "Music", false, -1, FDR_STATE_DELETED, "fader gone: deleted"},
		{FDR_SAVED_FADER, "Game", true, -1, FDR_STATE_DROPPED, "dropped by the oracle"},
		{FDR_SAVED_FADER, "", false, -1, FDR_STATE_REST, "an empty name is Everything else"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		idx = fdr_place_effective(&l, cases[i].saved, cases[i].name, cases[i].dropped, &st);
		char name[96];
		snprintf(name, sizeof(name), "effective: %s", cases[i].why);
		result(idx == cases[i].want_idx && st == cases[i].want, name, "index %d state %s (want %d %s)", idx, fdr_state_name(st), cases[i].want_idx, fdr_state_name(cases[i].want));
	}
	// Unknown allowance (0 or more than the faders): nothing locked.
	struct fdr_fader_list open = {names, 4, 0};
	idx = fdr_place_effective(&open, FDR_SAVED_FADER, "Alerts", false, &st);
	result(idx == 3 && st == FDR_STATE_ON_FADER, "effective: unknown plan locks nothing", "index %d state %s", idx, fdr_state_name(st));
}

static void validate(void)
{
	const char *const names[] = {"Voice", "Game", "~new", "Alerts"};
	struct fdr_fader_list l = {names, 4, 3};
	struct {
		const char *value;
		enum fdr_reject want;
		enum fdr_value kind;
		int idx;
	} cases[] = {
		{"f:Voice", FDR_OK, FDR_VALUE_FADER, 0},
		{"f:game", FDR_OK, FDR_VALUE_FADER, 1},
		{"f:~new", FDR_OK, FDR_VALUE_FADER, 2},
		{"x:rest", FDR_OK, FDR_VALUE_REST, -1},
		{"x:new", FDR_OK, FDR_VALUE_NEW, -1},
		{"x:keep", FDR_OK, FDR_VALUE_KEEP, -1},
		{"f:Alerts", FDR_REJECT_LOCKED, FDR_VALUE_FADER, -1},
		{"f:Music", FDR_REJECT_MISSING, FDR_VALUE_FADER, -1},
		{"f:Everything else", FDR_REJECT_MISSING, FDR_VALUE_FADER, -1},
		{"~new", FDR_REJECT_BAD_VALUE, FDR_VALUE_INVALID, -1},
		{"Voice", FDR_REJECT_BAD_VALUE, FDR_VALUE_INVALID, -1},
		{"", FDR_REJECT_BAD_VALUE, FDR_VALUE_INVALID, -1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		enum fdr_value kind;
		int idx = 99;
		char name[FDR_PLACE_NAME_MAX] = "";
		enum fdr_reject r = fdr_place_check(&l, cases[i].value, &kind, name, &idx);
		char label[96];
		snprintf(label, sizeof(label), "check \"%s\"", cases[i].value);
		bool ok = r == cases[i].want && (r != FDR_OK || (kind == cases[i].kind && idx == cases[i].idx));
		result(ok, label, "%s, %s index %d \"%s\"", fdr_reject_name(r), kind_name(kind), idx, name);
	}
	// The name a move stores is the list's spelling, not the request's.
	enum fdr_value kind;
	int idx;
	char name[FDR_PLACE_NAME_MAX] = "";
	fdr_place_check(&l, "f:GAME", &kind, name, &idx);
	result(strcmp(name, "Game") == 0, "check stores the list's spelling", "\"%s\"", name);
}

int main(void)
{
	codec();
	model();
	validate();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
