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
// Where a source goes: the placement model, without OBS (so it is unit-tested on its own).
//
// Nothing is placed for the streamer. A source's saved placement is its private setting
// "faders_fader": absent = NEW (never placed; it plays in Everything else), "" = placed in
// Everything else, a name = that fader. What it plays on (its effective placement) can differ:
// a fader past the plan's allowance is locked and plays in Everything else, a fader that no
// longer exists is deleted and plays in Everything else (the saved name is kept, so renaming it
// back restores it), and the mixer's oracle can drop a source for the stream.
//
// Moves are written as values that can't collide with a fader name, whatever it is:
//   "f:<name>"  that fader         "x:rest"  Everything else
//   "x:new"     back to NEW        "x:keep"  leave it where it is
// A move is checked against the faders as they are now: a fader that is gone or locked is refused.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define FDR_PLACE_NAME_MAX 64 // bytes, with the terminator (= FDR_STEM_NAME_MAX)
#define FDR_REST_NAME "Everything else"

enum fdr_saved {
	FDR_SAVED_NEW,   // never placed
	FDR_SAVED_REST,  // placed in Everything else
	FDR_SAVED_FADER, // placed on a fader, by name
};

enum fdr_state {
	FDR_STATE_NEW,      // never placed: plays in Everything else
	FDR_STATE_REST,     // placed in Everything else
	FDR_STATE_ON_FADER, // plays on its fader
	FDR_STATE_LOCKED,   // its fader is past the plan's allowance: plays in Everything else
	FDR_STATE_DELETED,  // its fader no longer exists: plays in Everything else
	FDR_STATE_DROPPED,  // the mixer dropped it for this stream: plays in Everything else
};

// The faders as configured, in order. `open` faders (the plan's allowance) are sent; the rest
// are locked. open <= 0 or >= n: none locked (the allowance isn't known).
struct fdr_fader_list {
	const char *const *names;
	int n;
	int open;
};

// The fader called `name` (ASCII case-insensitive), or -1.
int fdr_place_find(const struct fdr_fader_list *l, const char *name);
// Where a saved placement plays: the fader's index, or -1 for Everything else; *state says why.
int fdr_place_effective(const struct fdr_fader_list *l, enum fdr_saved saved, const char *name, bool dropped, enum fdr_state *state);
const char *fdr_state_name(enum fdr_state s); // "new", "rest", "ok", "locked", "deleted", "dropped"

enum fdr_value { FDR_VALUE_INVALID, FDR_VALUE_FADER, FDR_VALUE_REST, FDR_VALUE_NEW, FDR_VALUE_KEEP };
// Write a value; false (and out = "") if it can't be: an empty fader name, or no room.
bool fdr_value_encode(enum fdr_value kind, const char *fader, char *out, size_t size);
// Read a value. For a fader, its name goes to `fader` (cut to fit); otherwise fader = "".
enum fdr_value fdr_value_decode(const char *value, char *fader, size_t size);

enum fdr_reject {
	FDR_OK,
	FDR_REJECT_BAD_VALUE, // not a placement value
	FDR_REJECT_MISSING,   // no fader by that name (deleted, renamed, or never there)
	FDR_REJECT_LOCKED,    // past the plan's allowance
};
// Check a move. On FDR_OK: *kind, and for a fader its index and the name as the list spells it
// (name: FDR_PLACE_NAME_MAX bytes). Otherwise *index = -1.
enum fdr_reject fdr_place_check(const struct fdr_fader_list *l, const char *value, enum fdr_value *kind, char *name, int *index);
const char *fdr_reject_name(enum fdr_reject r); // "ok", "bad value", "missing", "locked"
