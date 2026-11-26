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
#include "split.h"

#include <stdio.h>
#include <string.h>

#define PREFIX_TWITCH "tw:"
#define PREFIX_YOUTUBE "yt:"

static int split_lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

// ASCII letters without case, everything else byte for byte (as fader names are compared).
static bool same_name(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	for (;; a++, b++) {
		int ca = split_lower((unsigned char)*a), cb = split_lower((unsigned char)*b);
		if (ca != cb)
			return false;
		if (!ca)
			return true;
	}
}

static bool has_split(const char *split)
{
	return split && *split;
}

static bool is_locked(const struct fdr_fader_list *l, int i)
{
	return l->open > 0 && l->open < l->n && i >= l->open;
}

enum fdr_platform fdr_platform_parse(const char *s)
{
	if (s && !strcmp(s, "twitch"))
		return FDR_PLATFORM_TWITCH;
	if (s && !strcmp(s, "youtube"))
		return FDR_PLATFORM_YOUTUBE;
	return FDR_PLATFORM_ALL;
}

const char *fdr_platform_name(enum fdr_platform p)
{
	return p == FDR_PLATFORM_TWITCH ? "twitch" : p == FDR_PLATFORM_YOUTUBE ? "youtube" : "";
}

bool fdr_plan_has_split(const char *plan)
{
	return plan && (!strcmp(plan, "pro") || !strcmp(plan, "partner"));
}

enum fdr_split_gate fdr_split_gate(const struct fdr_account *a, const struct fdr_fader_list *faders, const char *split, const char *fader)
{
	if (!a || !fdr_plan_has_split(a->plan))
		return FDR_SPLIT_GATE_PLAN;
	if (!a->twitch || !a->youtube)
		return FDR_SPLIT_GATE_YOUTUBE;
	// Another fader has it (only one that still exists counts).
	if (has_split(split) && !same_name(split, fader) && fdr_place_find(faders, split) >= 0)
		return FDR_SPLIT_GATE_OTHER;
	return FDR_SPLIT_GATE_OK;
}

const char *fdr_split_gate_text(enum fdr_split_gate g)
{
	switch (g) {
	case FDR_SPLIT_GATE_OK:
		return "";
	case FDR_SPLIT_GATE_PLAN:
		return "Needs Pro or Partner";
	case FDR_SPLIT_GATE_YOUTUBE:
		return "Connect YouTube to use this";
	case FDR_SPLIT_GATE_OTHER:
		return "Only one fader can do this.";
	}
	return "";
}

enum fdr_split_reject fdr_split_check_tick(const struct fdr_account *a, const struct fdr_fader_list *faders, const char *split, const char *fader, int sources_on_fader)
{
	int i = fdr_place_find(faders, fader);
	if (i < 0 || is_locked(faders, i))
		return FDR_SPLIT_REJECT_MISSING;
	switch (fdr_split_gate(a, faders, split, fader)) {
	case FDR_SPLIT_GATE_PLAN:
		return FDR_SPLIT_REJECT_PLAN;
	case FDR_SPLIT_GATE_YOUTUBE:
		return FDR_SPLIT_REJECT_YOUTUBE;
	case FDR_SPLIT_GATE_OTHER:
		return FDR_SPLIT_REJECT_OTHER;
	case FDR_SPLIT_GATE_OK:
		break;
	}
	return sources_on_fader > 1 ? FDR_SPLIT_REJECT_FULL : FDR_SPLIT_OK;
}

enum fdr_split_reject fdr_split_check_move(const char *split, const char *to)
{
	return has_split(split) && same_name(split, to) ? FDR_SPLIT_REJECT_ONLY_TWO : FDR_SPLIT_OK;
}

bool fdr_split_value_encode(enum fdr_platform p, const char *fader, char *out, size_t size)
{
	if (!out || !size)
		return false;
	out[0] = 0;
	if (p == FDR_PLATFORM_ALL || !fader || !*fader)
		return false;
	int n = snprintf(out, size, "%s%s", p == FDR_PLATFORM_TWITCH ? PREFIX_TWITCH : PREFIX_YOUTUBE, fader);
	if (n < 0 || (size_t)n >= size) {
		out[0] = 0;
		return false;
	}
	return true;
}

bool fdr_split_value_decode(const char *value, enum fdr_platform *p, char *fader, size_t size)
{
	if (fader && size)
		fader[0] = 0;
	if (!value)
		return false;
	enum fdr_platform which;
	if (!strncmp(value, PREFIX_TWITCH, strlen(PREFIX_TWITCH)))
		which = FDR_PLATFORM_TWITCH;
	else if (!strncmp(value, PREFIX_YOUTUBE, strlen(PREFIX_YOUTUBE)))
		which = FDR_PLATFORM_YOUTUBE;
	else
		return false;
	const char *name = value + 3;
	if (!*name)
		return false;
	if (p)
		*p = which;
	if (fader && size)
		snprintf(fader, size, "%s", name);
	return true;
}

enum fdr_split_reject fdr_split_check_pick(const struct fdr_fader_list *faders, const char *split, const char *value, enum fdr_platform *p, char *fader, size_t size)
{
	char want[FDR_PLACE_NAME_MAX * 2];
	enum fdr_platform which = FDR_PLATFORM_ALL;
	if (fader && size)
		fader[0] = 0;
	if (!fdr_split_value_decode(value, &which, want, sizeof(want)))
		return FDR_SPLIT_REJECT_NOT_SPLIT;
	if (p)
		*p = which;
	if (!has_split(split) || !same_name(split, want))
		return FDR_SPLIT_REJECT_NOT_SPLIT;
	int i = fdr_place_find(faders, want);
	if (i < 0 || is_locked(faders, i))
		return FDR_SPLIT_REJECT_MISSING;
	if (fader && size)
		snprintf(fader, size, "%s", faders->names[i]);
	return FDR_SPLIT_OK;
}

const char *fdr_split_reject_name(enum fdr_split_reject r)
{
	switch (r) {
	case FDR_SPLIT_OK:
		return "ok";
	case FDR_SPLIT_REJECT_PLAN:
		return "plan";
	case FDR_SPLIT_REJECT_YOUTUBE:
		return "youtube";
	case FDR_SPLIT_REJECT_OTHER:
		return "other";
	case FDR_SPLIT_REJECT_FULL:
		return "full";
	case FDR_SPLIT_REJECT_ONLY_TWO:
		return "only two";
	case FDR_SPLIT_REJECT_NOT_SPLIT:
		return "not split";
	case FDR_SPLIT_REJECT_MISSING:
		return "missing";
	}
	return "?";
}

void fdr_split_evaluate(const struct fdr_split_input *in, struct fdr_split_state *out)
{
	out->on = false;
	out->fader = has_split(in->split) ? fdr_place_find(in->faders, in->split) : -1;
	if (out->fader < 0)
		out->why = FDR_SPLIT_NONE;
	else if (is_locked(in->faders, out->fader))
		out->why = FDR_SPLIT_LOCKED;
	else if (!fdr_plan_has_split(in->account.plan))
		out->why = FDR_SPLIT_PLAN;
	else if (!in->account.twitch || !in->account.youtube)
		out->why = FDR_SPLIT_NO_YOUTUBE;
	else if (!in->twitch_source)
		out->why = FDR_SPLIT_NO_TWITCH_SOURCE;
	else if (!in->youtube_source)
		out->why = FDR_SPLIT_NO_YOUTUBE_SOURCE;
	else if (in->youtube_track < 1 || in->youtube_track > 6)
		out->why = FDR_SPLIT_NO_TRACK;
	else if (fdr_split_wire_stems(in->faders->open > 0 && in->faders->open < in->faders->n ? in->faders->open : in->faders->n, true) > FDR_SPLIT_STEMS_MAX)
		out->why = FDR_SPLIT_TOO_MANY;
	else
		out->why = FDR_SPLIT_ON;
	out->on = out->why == FDR_SPLIT_ON;
}

const char *fdr_split_why_name(enum fdr_split_why w)
{
	switch (w) {
	case FDR_SPLIT_ON:
		return "on";
	case FDR_SPLIT_NONE:
		return "none";
	case FDR_SPLIT_LOCKED:
		return "locked";
	case FDR_SPLIT_PLAN:
		return "plan";
	case FDR_SPLIT_NO_YOUTUBE:
		return "no youtube";
	case FDR_SPLIT_NO_TWITCH_SOURCE:
		return "no twitch source";
	case FDR_SPLIT_NO_YOUTUBE_SOURCE:
		return "no youtube source";
	case FDR_SPLIT_NO_TRACK:
		return "no track";
	case FDR_SPLIT_TOO_MANY:
		return "too many";
	}
	return "?";
}

enum fdr_split_role fdr_split_role(const struct fdr_split_state *s, const char *split, const char *saved_fader, enum fdr_platform platform, bool chosen)
{
	if (!has_split(split) || !same_name(split, saved_fader))
		return FDR_SPLIT_ROLE_NONE;
	if (!chosen || platform == FDR_PLATFORM_ALL)
		return FDR_SPLIT_ROLE_EXTRA;
	if (platform == FDR_PLATFORM_TWITCH)
		return FDR_SPLIT_ROLE_TWITCH;
	return s && s->on ? FDR_SPLIT_ROLE_YOUTUBE : FDR_SPLIT_ROLE_PARKED;
}

const char *fdr_split_role_name(enum fdr_split_role r)
{
	switch (r) {
	case FDR_SPLIT_ROLE_NONE:
		return "";
	case FDR_SPLIT_ROLE_TWITCH:
		return "twitch";
	case FDR_SPLIT_ROLE_YOUTUBE:
		return "youtube";
	case FDR_SPLIT_ROLE_PARKED:
		return "parked";
	case FDR_SPLIT_ROLE_EXTRA:
		return "extra";
	}
	return "?";
}

int fdr_split_wire_stems(int n_wire, bool on)
{
	return n_wire + 1 + (on ? FDR_SPLIT_EXTRA : 0);
}

unsigned fdr_split_track_warnings(uint32_t twitch_mixers, uint32_t youtube_mixers, int stream_track, int youtube_track)
{
	unsigned w = 0;
	uint32_t tw_bit = stream_track >= 1 && stream_track <= 6 ? 1u << (stream_track - 1) : 0;
	uint32_t yt_bit = youtube_track >= 1 && youtube_track <= 6 ? 1u << (youtube_track - 1) : 0;
	if (tw_bit && !(twitch_mixers & tw_bit))
		w |= FDR_SPLIT_WARN_TWITCH_NOT_ON_TRACK;
	if (yt_bit && !(youtube_mixers & yt_bit))
		w |= FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK;
	if (yt_bit && (twitch_mixers & yt_bit))
		w |= FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK;
	if (tw_bit && (youtube_mixers & tw_bit))
		w |= FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK;
	return w;
}

const char *fdr_split_warning_code(unsigned bit)
{
	switch (bit) {
	case FDR_SPLIT_WARN_TWITCH_NOT_ON_TRACK:
		return "twitch-source-not-on-track";
	case FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK:
		return "youtube-source-not-on-track";
	case FDR_SPLIT_WARN_TWITCH_ON_YOUTUBE_TRACK:
		return "twitch-source-on-youtube-track";
	case FDR_SPLIT_WARN_YOUTUBE_ON_TWITCH_TRACK:
		return "youtube-source-on-twitch-track";
	case FDR_SPLIT_WARN_YOUTUBE_TRACK_SILENT:
		return "youtube-track-silent";
	}
	return "";
}

int fdr_split_layout(const char *const *names, int n_wire, int split, struct fdr_stem_desc *out, char *fader_id, size_t id_size)
{
	bool on = split >= 0 && split < n_wire && fdr_split_wire_stems(n_wire, true) <= FDR_SPLIT_STEMS_MAX;
	int n = 0;
	if (fader_id && id_size)
		fader_id[0] = 0;
	if (on && fader_id)
		snprintf(fader_id, id_size, "f%d", split);
	for (int f = 0; f < n_wire && n < FDR_SPLIT_STEMS_MAX - 1; f++) {
		if (on && f == split) {
			out[n++] = (struct fdr_stem_desc){names[f], false, fader_id, FDR_PLATFORM_TWITCH};
			out[n++] = (struct fdr_stem_desc){names[f], false, fader_id, FDR_PLATFORM_YOUTUBE};
		} else {
			out[n++] = (struct fdr_stem_desc){names[f], false, NULL, FDR_PLATFORM_ALL};
		}
	}
	out[n++] = (struct fdr_stem_desc){FDR_REST_NAME, true, NULL, on ? FDR_PLATFORM_TWITCH : FDR_PLATFORM_ALL};
	if (on)
		out[n++] = (struct fdr_stem_desc){FDR_REST_NAME, true, NULL, FDR_PLATFORM_YOUTUBE};
	return n;
}

// A bounded writer: everything past the end is counted, not written.
struct writer {
	char *out;
	size_t size, len;
};
static void put(struct writer *w, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++, w->len++)
		if (w->len + 1 < w->size)
			w->out[w->len] = s[i];
}
static void puts_(struct writer *w, const char *s)
{
	put(w, s, strlen(s));
}
static void putf(struct writer *w, const char *fmt, int v)
{
	char buf[128];
	snprintf(buf, sizeof(buf), fmt, v);
	puts_(w, buf);
}
// A JSON string's contents: quotes and backslashes escaped, control characters dropped.
static void put_escaped(struct writer *w, const char *s)
{
	for (const char *c = s ? s : ""; *c; c++) {
		if (*c == '"' || *c == '\\') {
			put(w, "\\", 1);
			put(w, c, 1);
		} else if ((unsigned char)*c >= 0x20) {
			put(w, c, 1);
		}
	}
}
static size_t writer_end(struct writer *w)
{
	if (w->len + 1 > w->size) {
		if (w->size)
			w->out[0] = 0;
		return 0;
	}
	w->out[w->len] = 0;
	return w->len;
}

size_t fdr_stems_json(char *out, size_t size, const struct fdr_stem_desc *stems, int n)
{
	struct writer w = {out, size, 0};
	puts_(&w, "{\"type\":\"stems\",\"stems\":[");
	for (int s = 0; s < n; s++) {
		putf(&w, s ? ",{\"id\":%d,\"name\":\"" : "{\"id\":%d,\"name\":\"", s);
		put_escaped(&w, stems[s].rest ? FDR_REST_NAME : stems[s].name);
		puts_(&w, "\"");
		if (stems[s].rest)
			puts_(&w, ",\"rest\":true");
		if (stems[s].fader) {
			puts_(&w, ",\"fader\":\"");
			put_escaped(&w, stems[s].fader);
			puts_(&w, "\"");
		}
		if (stems[s].platform != FDR_PLATFORM_ALL) {
			puts_(&w, ",\"platform\":\"");
			puts_(&w, fdr_platform_name(stems[s].platform));
			puts_(&w, "\"");
		}
		puts_(&w, "}");
	}
	puts_(&w, "]}");
	return writer_end(&w);
}

size_t fdr_split_status_json(char *out, size_t size, int youtube_track, unsigned warnings)
{
	struct writer w = {out, size, 0};
	putf(&w, ",\"split\":{\"youtubeTrack\":%d,\"warnings\":[", youtube_track);
	bool first = true;
	for (int b = 0; b < FDR_SPLIT_WARN_COUNT; b++) {
		if (!(warnings & (1u << b)))
			continue;
		puts_(&w, first ? "\"" : ",\"");
		puts_(&w, fdr_split_warning_code(1u << b));
		puts_(&w, "\"");
		first = false;
	}
	puts_(&w, "]}");
	return writer_end(&w);
}
