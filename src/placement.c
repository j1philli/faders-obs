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
#include "placement.h"

#include <stdio.h>
#include <string.h>

#define PREFIX_FADER "f:"
#define VALUE_REST "x:rest"
#define VALUE_NEW "x:new"
#define VALUE_KEEP "x:keep"

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

// Like OBS's astrcmpi: ASCII letters compare without case, everything else byte for byte.
static int casecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = lower((unsigned char)*a), cb = lower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

int fdr_place_find(const struct fdr_fader_list *l, const char *name)
{
	if (!l || !name || !*name)
		return -1;
	for (int i = 0; i < l->n; i++)
		if (l->names[i] && casecmp(l->names[i], name) == 0)
			return i;
	return -1;
}

static bool locked(const struct fdr_fader_list *l, int i)
{
	return l->open > 0 && l->open < l->n && i >= l->open;
}

int fdr_place_effective(const struct fdr_fader_list *l, enum fdr_saved saved, const char *name, bool dropped, enum fdr_state *state)
{
	enum fdr_state st;
	int idx = -1;
	if (saved == FDR_SAVED_NEW) {
		st = FDR_STATE_NEW;
	} else if (saved == FDR_SAVED_REST || !name || !*name) {
		st = FDR_STATE_REST;
	} else {
		idx = fdr_place_find(l, name);
		if (idx < 0) {
			st = FDR_STATE_DELETED;
		} else if (locked(l, idx)) {
			st = FDR_STATE_LOCKED;
			idx = -1;
		} else if (dropped) {
			st = FDR_STATE_DROPPED;
			idx = -1;
		} else {
			st = FDR_STATE_ON_FADER;
		}
	}
	if (state)
		*state = st;
	return idx;
}

const char *fdr_state_name(enum fdr_state s)
{
	switch (s) {
	case FDR_STATE_NEW:
		return "new";
	case FDR_STATE_REST:
		return "rest";
	case FDR_STATE_ON_FADER:
		return "ok";
	case FDR_STATE_LOCKED:
		return "locked";
	case FDR_STATE_DELETED:
		return "deleted";
	case FDR_STATE_DROPPED:
		return "dropped";
	}
	return "?";
}

bool fdr_value_encode(enum fdr_value kind, const char *fader, char *out, size_t size)
{
	if (!out || !size)
		return false;
	out[0] = 0;
	int n;
	switch (kind) {
	case FDR_VALUE_FADER:
		if (!fader || !*fader)
			return false;
		n = snprintf(out, size, PREFIX_FADER "%s", fader);
		break;
	case FDR_VALUE_REST:
		n = snprintf(out, size, VALUE_REST);
		break;
	case FDR_VALUE_NEW:
		n = snprintf(out, size, VALUE_NEW);
		break;
	case FDR_VALUE_KEEP:
		n = snprintf(out, size, VALUE_KEEP);
		break;
	default:
		return false;
	}
	if (n < 0 || (size_t)n >= size) {
		out[0] = 0;
		return false;
	}
	return true;
}

enum fdr_value fdr_value_decode(const char *value, char *fader, size_t size)
{
	if (fader && size)
		fader[0] = 0;
	if (!value)
		return FDR_VALUE_INVALID;
	if (strncmp(value, PREFIX_FADER, strlen(PREFIX_FADER)) == 0) {
		const char *name = value + strlen(PREFIX_FADER);
		if (!*name)
			return FDR_VALUE_INVALID;
		if (fader && size)
			snprintf(fader, size, "%s", name);
		return FDR_VALUE_FADER;
	}
	if (strcmp(value, VALUE_REST) == 0)
		return FDR_VALUE_REST;
	if (strcmp(value, VALUE_NEW) == 0)
		return FDR_VALUE_NEW;
	if (strcmp(value, VALUE_KEEP) == 0)
		return FDR_VALUE_KEEP;
	return FDR_VALUE_INVALID;
}

enum fdr_reject fdr_place_check(const struct fdr_fader_list *l, const char *value, enum fdr_value *kind, char *name, int *index)
{
	char want[FDR_PLACE_NAME_MAX * 2];
	enum fdr_value k = fdr_value_decode(value, want, sizeof(want));
	if (kind)
		*kind = k;
	if (index)
		*index = -1;
	if (name)
		name[0] = 0;
	if (k == FDR_VALUE_INVALID)
		return FDR_REJECT_BAD_VALUE;
	if (k != FDR_VALUE_FADER)
		return FDR_OK;
	int i = fdr_place_find(l, want);
	if (i < 0)
		return FDR_REJECT_MISSING;
	if (locked(l, i))
		return FDR_REJECT_LOCKED;
	if (index)
		*index = i;
	if (name)
		snprintf(name, FDR_PLACE_NAME_MAX, "%s", l->names[i]);
	return FDR_OK;
}

const char *fdr_reject_name(enum fdr_reject r)
{
	switch (r) {
	case FDR_OK:
		return "ok";
	case FDR_REJECT_BAD_VALUE:
		return "bad value";
	case FDR_REJECT_MISSING:
		return "missing";
	case FDR_REJECT_LOCKED:
		return "locked";
	}
	return "?";
}
