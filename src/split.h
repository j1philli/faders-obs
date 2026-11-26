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
// The split fader ("Two sources: one per platform"), without OBS, so it is unit-tested on its own.
//
// One fader per channel, whatever its name, can take two sources: one for Twitch and one for
// YouTube. Viewers then get two sliders for it, and each platform gets its own Everything else
// (see the wire contract, per-platform/PROTOCOL-SPLIT.md). It counts as one fader for the plan,
// and only Pro and Partner have it.
//
// Saved: the plugin settings hold the split fader's name and the YouTube track (1-6, 0 = not
// picked yet); each of its two sources holds the fader's name in "faders_fader" (as any placed
// source does) plus its platform in "faders_platform". Every other placement has no platform.
//
// Effective: the split is on only while the plan is Pro or Partner, the account has Twitch and
// YouTube, both sources are picked and the YouTube track is picked. Otherwise the fader is a
// normal fader holding the Twitch source, and the YouTube source plays in Everything else
// ("parked"). Nothing saved changes, so an upgrade brings the split back as it was.
//
// The split fader holds only its two sources: any other source saved on it plays in Everything
// else, and moving one there is refused.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "placement.h"

#define FDR_PLATFORM_SETTING "faders_platform" // per source: "twitch" / "youtube"; absent = all
#define FDR_SPLIT_FADER_KEY "split_fader"      // plugin settings: the split fader's name, "" = none
#define FDR_SPLIT_TRACK_KEY "split_youtube_track" // plugin settings: 1-6, 0 = not picked
#define FDR_SPLIT_STEMS_MAX 16                 // = FDR_MAX_STEMS (the wire's limit)
// A split adds two layers to the wire: the fader's YouTube source and YouTube's Everything else.
#define FDR_SPLIT_EXTRA 2

enum fdr_platform {
	FDR_PLATFORM_ALL,
	FDR_PLATFORM_TWITCH,
	FDR_PLATFORM_YOUTUBE,
};
enum fdr_platform fdr_platform_parse(const char *s); // "twitch", "youtube"; anything else: ALL
const char *fdr_platform_name(enum fdr_platform p);  // "twitch", "youtube", ""

// The account as the hub's hello says it (or as remembered from the last one). plan "" = unknown.
struct fdr_account {
	const char *plan;
	bool twitch, youtube;
};
bool fdr_plan_has_split(const char *plan); // "pro", "partner"

// Whether fader `fader` may be ticked "Two sources: one per platform", given the saved split fader
// (`split`, "" = none; a name not in `faders` doesn't count).
enum fdr_split_gate {
	FDR_SPLIT_GATE_OK,
	FDR_SPLIT_GATE_PLAN,    // "Needs Pro or Partner"
	FDR_SPLIT_GATE_YOUTUBE, // "Connect YouTube to use this"
	FDR_SPLIT_GATE_OTHER,   // "Only one fader can do this."
};
enum fdr_split_gate fdr_split_gate(const struct fdr_account *a, const struct fdr_fader_list *faders, const char *split, const char *fader);
const char *fdr_split_gate_text(enum fdr_split_gate g);

// Ticking it: as the gate, and a fader with more than one source on it is refused (it would hold
// only two, one per platform: the streamer moves the others first). One source becomes its Twitch
// source.
enum fdr_split_reject {
	FDR_SPLIT_OK,
	FDR_SPLIT_REJECT_PLAN,
	FDR_SPLIT_REJECT_YOUTUBE,
	FDR_SPLIT_REJECT_OTHER,
	FDR_SPLIT_REJECT_FULL,     // the fader has 2+ sources
	FDR_SPLIT_REJECT_ONLY_TWO, // a move of another source onto the split fader
	FDR_SPLIT_REJECT_NOT_SPLIT, // a platform pick for a fader that isn't the split fader
	FDR_SPLIT_REJECT_MISSING,  // no such fader, or it is locked
};
enum fdr_split_reject fdr_split_check_tick(const struct fdr_account *a, const struct fdr_fader_list *faders, const char *split, const char *fader, int sources_on_fader);
// A move of a source to fader `to` (a plain "f:<name>" move): refused when it is the split fader.
enum fdr_split_reject fdr_split_check_move(const char *split, const char *to);
// A platform pick: "tw:<fader>" / "yt:<fader>". Allowed only for the split fader, open and present.
enum fdr_split_reject fdr_split_check_pick(const struct fdr_fader_list *faders, const char *split, const char *value, enum fdr_platform *p, char *fader, size_t size);
const char *fdr_split_reject_name(enum fdr_split_reject r);
bool fdr_split_value_encode(enum fdr_platform p, const char *fader, char *out, size_t size);
bool fdr_split_value_decode(const char *value, enum fdr_platform *p, char *fader, size_t size);

// Why the split is (not) on.
enum fdr_split_why {
	FDR_SPLIT_ON,
	FDR_SPLIT_NONE,           // no split fader saved (or it no longer exists)
	FDR_SPLIT_LOCKED,         // it is past the plan's allowance
	FDR_SPLIT_PLAN,           // the plan isn't Pro or Partner (or isn't known yet)
	FDR_SPLIT_NO_YOUTUBE,     // the account has no YouTube (or no Twitch)
	FDR_SPLIT_NO_TWITCH_SOURCE,
	FDR_SPLIT_NO_YOUTUBE_SOURCE,
	FDR_SPLIT_NO_TRACK,       // the YouTube track isn't picked
	FDR_SPLIT_TOO_MANY,       // faders + 3 layers wouldn't fit the wire's 16
};
struct fdr_split_input {
	const struct fdr_fader_list *faders; // as configured, with the plan's allowance
	const char *split;                   // saved split fader ("" = none)
	int youtube_track;                   // 0 = not picked
	struct fdr_account account;
	bool twitch_source, youtube_source;  // picked (and present)
};
struct fdr_split_state {
	bool on;
	enum fdr_split_why why;
	int fader; // its index in the list (also while off), -1 = none
};
void fdr_split_evaluate(const struct fdr_split_input *in, struct fdr_split_state *out);
const char *fdr_split_why_name(enum fdr_split_why w);

// What a saved source on the split fader does. `platform` is its tag; `chosen`: it is the one
// source of that platform the split uses (the first one, if a duplicate made two).
enum fdr_split_role {
	FDR_SPLIT_ROLE_NONE,    // not on the split fader: placement.h decides
	FDR_SPLIT_ROLE_TWITCH,  // plays on the fader (as the Twitch stem while the split is on)
	FDR_SPLIT_ROLE_YOUTUBE, // the YouTube stem while on
	FDR_SPLIT_ROLE_PARKED,  // the YouTube source while off: plays in Everything else, kept
	FDR_SPLIT_ROLE_EXTRA,   // anything else saved on it: plays in Everything else
};
enum fdr_split_role fdr_split_role(const struct fdr_split_state *s, const char *split, const char *saved_fader, enum fdr_platform platform, bool chosen);
const char *fdr_split_role_name(enum fdr_split_role r); // "", "twitch", "youtube", "parked", "extra"

// Layers on the wire for n_wire faders: one each, Everything else, and two more while split.
// The plan counts faders (the split fader is one).
int fdr_split_wire_stems(int n_wire, bool on);

// Track warnings, from the two sources' OBS track bits (bit 0 = Track 1). stream_track is the
// track Twitch gets (the one OBS streams), youtube_track the picked one.
enum {
	FDR_SPLIT_WARN_TWITCH_NOT_ON_TRACK = 1 << 0,
	FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK = 1 << 1,
	FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK = 1 << 2,
	FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK = 1 << 3,
	FDR_SPLIT_WARN_YOUTUBE_TRACK_SILENT = 1 << 4,
	FDR_SPLIT_WARN_COUNT = 5,
};
unsigned fdr_split_track_warnings(uint32_t twitch_mixers, uint32_t youtube_mixers, int stream_track, int youtube_track);
const char *fdr_split_warning_code(unsigned bit); // the wire's stable code for one bit

// ── The wire (PROTOCOL-SPLIT.md §2–3) ────────────────────────────────────────────────────
// One stem as announced. fader: the split group's id ("f<index>"), NULL for every other stem.
struct fdr_stem_desc {
	const char *name;
	bool rest;
	const char *fader;
	enum fdr_platform platform;
};
// The stem list for n_wire fader names: today's (split < 0: faders, then Everything else) or the
// split's (split = the split fader's index: its Twitch stem in its place, its YouTube stem right
// after it, then the other faders, then Everything else for Twitch and for YouTube). fader_id:
// the group id's storage (≥ 8 bytes). Returns the number of stems.
int fdr_split_layout(const char *const *names, int n_wire, int split, struct fdr_stem_desc *out, char *fader_id, size_t id_size);
// {"type":"stems","stems":[…]} for that list. Returns the length, or 0 if it doesn't fit.
size_t fdr_stems_json(char *out, size_t size, const struct fdr_stem_desc *stems, int n);
// ,"split":{"youtubeTrack":N,"warnings":[…]} (the status message's split object, with its comma).
size_t fdr_split_status_json(char *out, size_t size, int youtube_track, unsigned warnings);
