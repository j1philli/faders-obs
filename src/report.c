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
#include "report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── A growing string ───────────────────────────────────────────────────────────────────

struct out {
	char *s;
	size_t len, cap;
};

static void out_add(struct out *o, const char *p, size_t n)
{
	if (o->len + n + 1 > o->cap) {
		size_t cap = (o->len + n + 1) * 2 + 256;
		char *s = realloc(o->s, cap);
		if (!s)
			abort(); // out of memory building a few KB of text
		o->s = s;
		o->cap = cap;
	}
	memcpy(o->s + o->len, p, n);
	o->len += n;
	o->s[o->len] = 0;
}

static void out_str(struct out *o, const char *s)
{
	out_add(o, s ? s : "", s ? strlen(s) : 0);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void out_fmt(struct out *o, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		out_add(o, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static char *dup_str(const char *s)
{
	size_t n = s ? strlen(s) : 0;
	char *d = malloc(n + 1);
	if (!d)
		abort();
	memcpy(d, s ? s : "", n + 1);
	return d;
}

// A JSON string: quotes, backslashes and control characters escaped; a backtick as ` so the
// block's ``` fence can't be closed from inside.
static void out_json(struct out *o, const char *s)
{
	out_str(o, "\"");
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '"')
			out_str(o, "\\\"");
		else if (*p == '\\')
			out_str(o, "\\\\");
		else if (*p == '\n')
			out_str(o, "\\n");
		else if (*p == '\t')
			out_str(o, "\\t");
		else if (*p == '\r')
			out_str(o, "\\r");
		else if (*p < 0x20 || *p == '`')
			out_fmt(o, "\\u%04x", *p);
		else
			out_add(o, (const char *)p, 1);
	}
	out_str(o, "\"");
}

// Text for a Markdown line or code block: control characters become spaces and backticks quotes,
// so a name can't break the layout or close a code block.
static void out_text(struct out *o, const char *s)
{
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		char c = (char)*p;
		if (*p < 0x20)
			c = ' ';
		else if (c == '`')
			c = '\'';
		out_add(o, &c, 1);
	}
}

static void out_bool(struct out *o, bool b)
{
	out_str(o, b ? "true" : "false");
}

// ── Names off: every known name replaced by its alias, wherever it appears ─────────────

struct alias {
	const char *name;
	char alias[32];
};

struct names {
	struct alias *a;
	int n, cap;
};

static void alias_add(struct names *t, const char *name, const char *alias)
{
	if (!name || !*name)
		return;
	for (int i = 0; i < t->n; i++)
		if (!strcmp(t->a[i].name, name))
			return; // the first alias wins (a source named like a fader stays a source)
	if (t->n == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 64;
		struct alias *a = realloc(t->a, sizeof(*a) * (size_t)t->cap);
		if (!a)
			abort();
		t->a = a;
	}
	t->a[t->n].name = name;
	snprintf(t->a[t->n].alias, sizeof(t->a[t->n].alias), "%s", alias);
	t->n++;
}

static const char *alias_of(const struct names *t, const char *name)
{
	for (int i = 0; name && i < t->n; i++)
		if (!strcmp(t->a[i].name, name))
			return t->a[i].alias;
	return NULL;
}

// Free text with every known name (2+ bytes, longest first at each position) replaced.
static char *anonymize(const struct names *t, const char *s)
{
	struct out o = {0};
	out_str(&o, "");
	for (const char *p = s ? s : ""; *p;) {
		size_t best = 0;
		const char *rep = NULL;
		for (int i = 0; i < t->n; i++) {
			size_t n = strlen(t->a[i].name);
			if (n >= 2 && n > best && !strncmp(p, t->a[i].name, n)) {
				best = n;
				rep = t->a[i].alias;
			}
		}
		if (rep) {
			out_str(&o, rep);
			p += best;
		} else {
			out_add(&o, p, 1);
			p++;
		}
	}
	return o.s;
}

// ── Building it ────────────────────────────────────────────────────────────────────────

struct ctx {
	const struct fdr_report_input *in;
	struct names names; // filled only with names off
	fdr_redact_counts removed;
};

// One free-text field, ready to print: anonymized (names off), then redacted. Counted once.
static char *text(struct ctx *c, const char *s)
{
	char *a = c->in->names ? dup_str(s) : anonymize(&c->names, s);
	fdr_redact_counts k;
	char *r = fdr_redact(a, &c->in->redact, &k);
	free(a);
	c->removed.plugin_key += k.plugin_key;
	c->removed.stream_key += k.stream_key;
	c->removed.urls += k.urls;
	c->removed.tokens += k.tokens;
	c->removed.emails += k.emails;
	c->removed.paths += k.paths;
	c->removed.ips += k.ips;
	return r;
}

static char *alias_text(struct ctx *c, const char *what, int i)
{
	(void)c;
	char a[32];
	snprintf(a, sizeof(a), "%s %d", what, i + 1);
	return dup_str(a);
}

// A fader's name as shown: with names off, the alias of the fader it is (or of a fader that's gone).
static char *fader_text(struct ctx *c, const char *name)
{
	if (c->in->names || !name || !*name)
		return text(c, name);
	for (int f = 0; f < c->in->n_faders; f++)
		if (!strcmp(c->in->faders[f].name, name))
			return alias_text(c, "Fader", f);
	const char *a = alias_of(&c->names, name);
	return a ? dup_str(a) : text(c, name);
}

// A source's name as shown (the problems): with names off, the alias of that source.
static char *source_text(struct ctx *c, const char *name)
{
	if (c->in->names || !name || !*name)
		return text(c, name);
	for (int i = 0; i < c->in->n_sources; i++)
		if (!strcmp(c->in->sources[i].name, name))
			return alias_text(c, "Source", i);
	return text(c, name);
}

static void utc_tm(int64_t t, struct tm *tm)
{
	time_t tt = (time_t)t;
#ifdef _WIN32
	gmtime_s(tm, &tt);
#else
	gmtime_r(&tt, tm);
#endif
}

static void duration(struct out *o, int64_t s)
{
	if (s < 0)
		s = 0;
	if (s < 60)
		out_fmt(o, "%lld s", (long long)s);
	else if (s < 3600)
		out_fmt(o, "%lld m", (long long)(s / 60));
	else if (s < 86400)
		out_fmt(o, "%lld h %lld m", (long long)(s / 3600), (long long)(s % 3600 / 60));
	else
		out_fmt(o, "%lld d %lld h", (long long)(s / 86400), (long long)(s % 86400 / 3600));
}

static const char *plan_name(const char *plan)
{
	if (!plan || !*plan)
		return NULL;
	if (!strcmp(plan, "free"))
		return "Free";
	if (!strcmp(plan, "pro"))
		return "Pro";
	if (!strcmp(plan, "partner"))
		return "Partner";
	return plan;
}

static const char *platforms_name(const char *p)
{
	if (!p || !*p)
		return NULL;
	if (!strcmp(p, "twitch,youtube") || !strcmp(p, "youtube,twitch"))
		return "Twitch and YouTube";
	if (!strcmp(p, "youtube"))
		return "YouTube";
	if (!strcmp(p, "twitch"))
		return "Twitch";
	return p;
}

static void removed_line(struct out *o, const fdr_redact_counts *r)
{
	struct out parts = {0};
	out_str(&parts, "");
	const char *sep = "";
#define PART(cond, ...)                         \
	do {                                    \
		if (cond) {                     \
			out_str(&parts, sep);   \
			out_fmt(&parts, __VA_ARGS__); \
			sep = ", ";             \
		}                               \
	} while (0)
	PART(r->plugin_key, "your plugin key");
	PART(r->stream_key, "your stream key");
	PART(r->paths, "%d folder path%s (your user name)", r->paths, r->paths == 1 ? "" : "s");
	PART(r->emails, "%d email address%s", r->emails, r->emails == 1 ? "" : "es");
	PART(r->urls, "%d web address part%s", r->urls, r->urls == 1 ? "" : "s");
	PART(r->tokens, "%d token%s", r->tokens, r->tokens == 1 ? "" : "s");
	PART(r->ips, "%d IP address%s", r->ips, r->ips == 1 ? "" : "es");
#undef PART
	if (parts.len)
		out_fmt(o, "Nothing was uploaded. Removed before you saw this: %s.\n", parts.s);
	else
		out_str(o, "Nothing was uploaded. Nothing needed removing.\n");
	free(parts.s);
}

struct kind_count {
	const char *kind;
	int n;
};

static int cmp_kind(const void *a, const void *b)
{
	const struct kind_count *x = a, *y = b;
	return x->n != y->n ? y->n - x->n : strcmp(x->kind, y->kind);
}

static bool is_state(const struct fdr_report_source *s, const char *state)
{
	return s->state && !strcmp(s->state, state);
}

static bool is_rest(const struct fdr_report_source *s)
{
	return is_state(s, "new") || is_state(s, "rest") || is_state(s, "deleted") || is_state(s, "parked") || is_state(s, "extra");
}

static const char *split_why(const char *why)
{
	if (!why || !*why || !strcmp(why, "none"))
		return "its fader is gone";
	if (!strcmp(why, "plan"))
		return "needs Pro or Partner";
	if (!strcmp(why, "no youtube"))
		return "YouTube isn't connected";
	if (!strcmp(why, "no twitch source"))
		return "no Twitch source picked";
	if (!strcmp(why, "no youtube source"))
		return "no YouTube source picked";
	if (!strcmp(why, "no track"))
		return "no YouTube track picked";
	if (!strcmp(why, "locked"))
		return "its fader isn't in the plan";
	if (!strcmp(why, "too many"))
		return "too many faders";
	return why;
}

// One warning code (split.c) in words, as Tools → Faders says it.
static void split_warning(struct out *o, const char *code, size_t n, int stream_track, int youtube_track)
{
	if (n == 26 && !strncmp(code, "twitch-source-not-on-track", n))
		out_fmt(o, "Twitch source isn't on Track %d", stream_track);
	else if (n == 27 && !strncmp(code, "youtube-source-not-on-track", n))
		out_fmt(o, "YouTube source isn't on Track %d", youtube_track);
	else if (n == 30 && !strncmp(code, "twitch-source-on-youtube-track", n))
		out_str(o, "Twitch source is also on the YouTube track: YouTube viewers will hear it twice");
	else if (n == 30 && !strncmp(code, "youtube-source-on-twitch-track", n))
		out_str(o, "YouTube source is also on the Twitch track: Twitch viewers will hear it twice");
	else if (n == 20 && !strncmp(code, "youtube-track-silent", n))
		out_fmt(o, "Track %d, picked for YouTube, is silent", youtube_track);
	else
		out_fmt(o, "%.*s", (int)n, code);
}

int fdr_report_build(const struct fdr_report_input *in, char **result)
{
	*result = NULL;
	struct ctx c = {.in = in};
	int nf = in->n_faders, ns = in->n_sources;

	// Names off: aliases for faders, sources, saved faders that are gone, the channel.
	if (!in->names) {
		char a[32];
		for (int i = 0; i < nf; i++) {
			snprintf(a, sizeof(a), "Fader %d", i + 1);
			alias_add(&c.names, in->faders[i].name, a);
		}
		for (int i = 0; i < ns; i++) {
			snprintf(a, sizeof(a), "Source %d", i + 1);
			alias_add(&c.names, in->sources[i].name, a);
		}
		int gone = 0;
		for (int i = 0; i < ns; i++) {
			const char *saved = in->sources[i].saved;
			if (saved && *saved && strcmp(saved, "new") && strcmp(saved, "rest") && !alias_of(&c.names, saved)) {
				snprintf(a, sizeof(a), "Removed fader %d", ++gone);
				alias_add(&c.names, saved, a);
			}
		}
		alias_add(&c.names, in->channel, "[channel]");
	}

	// Every free-text field, processed once.
	char **fader = calloc((size_t)nf + 1, sizeof(char *));
	char **src = calloc((size_t)ns + 1, sizeof(char *));
	char **saved = calloc((size_t)ns + 1, sizeof(char *));
	char **plays = calloc((size_t)ns + 1, sizeof(char *));
	char **logmsg = calloc((size_t)in->n_log + 1, sizeof(char *));
	if (!fader || !src || !saved || !plays || !logmsg)
		abort();
	// Names off: each source and fader is its own alias, even when a source shares its name with
	// a fader; only free text (log lines, failures) is replaced by name.
	for (int i = 0; i < nf; i++)
		fader[i] = in->names ? text(&c, in->faders[i].name) : alias_text(&c, "Fader", i);
	for (int i = 0; i < ns; i++) {
		src[i] = in->names ? text(&c, in->sources[i].name) : alias_text(&c, "Source", i);
		const char *sv = in->sources[i].saved ? in->sources[i].saved : "new";
		saved[i] = !strcmp(sv, "new") || !strcmp(sv, "rest") ? dup_str(sv) : fader_text(&c, sv);
		plays[i] = fader_text(&c, in->sources[i].plays);
	}
	char *channel = in->names || !in->channel || !*in->channel ? text(&c, in->channel) : dup_str("[channel]");
	char *failure = text(&c, in->last_failure);
	char *mismatch = source_text(&c, in->mix_mismatch);
	char *duplicate = source_text(&c, in->mix_duplicate);
	const struct fdr_report_split *sp = &in->split;
	char *split_fader = fader_text(&c, sp->fader);
	char *split_tw = source_text(&c, sp->twitch_source);
	char *split_yt = source_text(&c, sp->youtube_source);
	bool split_ticked = sp->fader && *sp->fader;
	char *config_dir = text(&c, in->config_dir);
	char *module_dir = text(&c, in->module_dir);
	for (int i = 0; i < in->n_log; i++)
		logmsg[i] = text(&c, in->log[i].msg);

	struct tm utc, local;
	utc_tm(in->now, &utc);
	utc_tm(in->now + (int64_t)in->utc_offset_min * 60, &local);
	int off = in->utc_offset_min, off_abs = off < 0 ? -off : off;
	char utc_s[32], local_s[32], utc_iso[32], local_iso[64], zone[16];
	strftime(utc_s, sizeof(utc_s), "%Y-%m-%d %H:%M:%S", &utc);
	strftime(local_s, sizeof(local_s), "%Y-%m-%d %H:%M:%S", &local);
	strftime(utc_iso, sizeof(utc_iso), "%Y-%m-%dT%H:%M:%SZ", &utc);
	snprintf(zone, sizeof(zone), "%c%02d:%02d", off < 0 ? '-' : '+', off_abs / 60, off_abs % 60);
	char local_base[32];
	strftime(local_base, sizeof(local_base), "%Y-%m-%dT%H:%M:%S", &local);
	snprintf(local_iso, sizeof(local_iso), "%s%s", local_base, zone);
	int track = in->stream_track ? in->stream_track : in->stream_audio_track ? in->stream_audio_track : 1;

	// Counts over every audio input.
	int n_new = 0, n_muted = 0, n_monitor_only = 0, n_off_track = 0;
	struct kind_count *kinds = calloc((size_t)ns + 1, sizeof(*kinds));
	int n_kinds = 0;
	if (!kinds)
		abort();
	for (int i = 0; i < ns; i++) {
		const struct fdr_report_source *s = &in->sources[i];
		n_new += is_state(s, "new");
		n_muted += s->muted;
		n_monitor_only += s->monitoring && !strcmp(s->monitoring, "monitor only");
		n_off_track += !s->on_stream_track;
		const char *k = s->kind ? s->kind : "";
		int j = 0;
		while (j < n_kinds && strcmp(kinds[j].kind, k))
			j++;
		if (j == n_kinds)
			kinds[n_kinds++] = (struct kind_count){k, 0};
		kinds[j].n++;
	}
	qsort(kinds, (size_t)n_kinds, sizeof(*kinds), cmp_kind);

	struct out o = {0};
	out_str(&o, "## Faders debug info\n\n");
	out_fmt(&o, "Generated %s (UTC%s) · %s UTC\n", local_s, zone, utc_s);
	removed_line(&o, &c.removed);
	out_str(&o, in->names ? "Source, fader and channel names are included.\n\n"
			      : "Names are left out: sources are \"Source 1\", \"Source 2\"…, faders \"Fader 1\"…, the channel \"[channel]\".\n\n");

	// ── Summary ──
	out_fmt(&o, "- **Faders**: %s, running ", in->plugin_version);
	duration(&o, in->plugin_uptime_s);
	if (in->update_available && *in->update_available)
		out_fmt(&o, ", %s is available", in->update_available);
	out_fmt(&o, "\n- **OBS**: %s, ", in->obs_version);
	if (in->streaming) {
		out_str(&o, "live ");
		duration(&o, in->stream_uptime_s);
		out_fmt(&o, ", stream audio Track %d\n", track);
	} else {
		out_str(&o, "not live\n");
	}
	out_fmt(&o, "- **System**: %s %s, %s%s%s, %d cores / %d threads, %d GB\n", in->sys.os, in->sys.os_version, in->sys.arch, *in->sys.cpu ? ", " : "", in->sys.cpu, in->sys.cores_physical,
		in->sys.cores_logical, in->sys.ram_gb);
	if (in->mixer_ok)
		out_fmt(&o, "- **Audio**: %d kHz %s: OK for Faders\n", in->sample_rate / 1000, in->channels == 2 ? "stereo" : "mono");
	else
		out_fmt(&o, "- **Audio**: %d Hz, %d channel(s): Faders needs 48 kHz stereo\n", in->sample_rate, in->channels);
	out_str(&o, "- **Account**: ");
	if (*channel || plan_name(in->plan)) {
		const char *sep = "";
		if (*channel) {
			out_text(&o, channel);
			sep = " · ";
		}
		if (plan_name(in->plan)) {
			out_fmt(&o, "%s%s", sep, plan_name(in->plan));
			sep = " · ";
		}
		if (platforms_name(in->platforms)) {
			out_fmt(&o, "%s%s", sep, platforms_name(in->platforms));
			sep = " · ";
		}
		if (in->connected && in->listener_limit > 0)
			out_fmt(&o, "%s%d of %d viewers mixing", sep, in->listeners, in->listener_limit);
		else if (in->connected)
			out_fmt(&o, "%s%d viewer%s mixing", sep, in->listeners, in->listeners == 1 ? "" : "s");
		out_str(&o, "\n");
	} else {
		out_str(&o, "not known yet\n");
	}
	out_str(&o, "- **Connection**: ");
	if (!in->enabled)
		out_str(&o, "off (Send audio to Faders is unticked)");
	else if (!in->plugin_key_set)
		out_str(&o, "no plugin key");
	else if (!in->running)
		out_str(&o, in->only_live && !in->streaming ? "waiting to go live (connects only while live)" : "not running");
	else if (in->connected)
		out_fmt(&o, "connected, %d layers, %d kbps up", in->stems_on_wire, in->uplink_kbps);
	else if (in->rejected_status)
		out_fmt(&o, "refused by the hub (HTTP %d)", in->rejected_status);
	else if (*failure)
		out_str(&o, "can't reach the hub, retrying");
	else
		out_str(&o, "connecting");
	out_fmt(&o, " · %s\n", in->server && *in->server ? in->server : "custom");
	if (*failure) {
		out_str(&o, "- **Last failure**: ");
		out_text(&o, failure);
		out_str(&o, "\n");
	}
	out_str(&o, "- **Problems**: ");
	{
		const char *sep = "";
		if (in->stream_track_silent) {
			out_fmt(&o, "Track %d, which OBS streams, is silent", track);
			sep = "; ";
		}
		if (*mismatch) {
			out_fmt(&o, "%s\"", sep);
			out_text(&o, mismatch);
			out_str(&o, "\" isn't in the stream the way it's sent (plays in Everything else)");
			sep = "; ";
		}
		if (*duplicate) {
			out_fmt(&o, "%s\"", sep);
			out_text(&o, duplicate);
			out_str(&o, "\" is in the stream twice");
			sep = "; ";
		}
		out_str(&o, *sep ? "\n" : "none\n");
	}
	out_str(&o, "- **Per-platform fader**: ");
	if (!split_ticked) {
		out_str(&o, "none\n");
	} else {
		out_text(&o, split_fader);
		if (sp->on)
			out_str(&o, ", on");
		else if (sp->why && !strcmp(sp->why, "quality") && sp->max_faders > 0)
			out_fmt(&o, ", off (too many faders for two sources at this audio quality: max %d with a split)", sp->max_faders);
		else if (sp->why && !strcmp(sp->why, "quality"))
			out_str(&o, ", off (no split at this audio quality)");
		else
			out_fmt(&o, ", off (%s)", split_why(sp->why));
		out_str(&o, " · Twitch: ");
		out_text(&o, *split_tw ? split_tw : "not picked");
		out_str(&o, " · YouTube: ");
		out_text(&o, *split_yt ? split_yt : "not picked");
		if (sp->youtube_track)
			out_fmt(&o, " on Track %d", sp->youtube_track);
		else
			out_str(&o, ", no track picked");
		for (const char *w = sp->warnings ? sp->warnings : ""; *w;) {
			size_t n = strcspn(w, ",");
			out_str(&o, " · ");
			split_warning(&o, w, n, track, sp->youtube_track);
			w += n + (w[n] == ',');
		}
		out_str(&o, "\n");
	}

	// ── Faders ──
	out_str(&o, "\n**Faders**");
	if (in->allowance > 0)
		out_fmt(&o, " (%s includes %d)", plan_name(in->plan) ? plan_name(in->plan) : "your plan", in->allowance);
	out_str(&o, "\n\n");
	for (int f = 0; f < nf; f++) {
		out_str(&o, "- **");
		out_text(&o, fader[f]);
		out_str(&o, in->faders[f].on_wire ? "**: " : "** — not in your plan, plays in Everything else: ");
		int k = 0;
		for (int i = 0; i < ns; i++) {
			const struct fdr_report_source *s = &in->sources[i];
			bool here = (is_state(s, "ok") || is_state(s, "locked") || is_state(s, "dropped")) && s->saved && !strcmp(s->saved, in->faders[f].name);
			if (!here)
				continue;
			out_str(&o, k++ ? ", " : "");
			out_text(&o, src[i]);
			if (is_state(s, "dropped"))
				out_str(&o, " (dropped for this stream)");
			else if (split_ticked && !strcmp(sp->fader, in->faders[f].name) && sp->twitch_source && !strcmp(s->name, sp->twitch_source))
				out_str(&o, " (Twitch)");
			else if (split_ticked && !strcmp(sp->fader, in->faders[f].name) && sp->youtube_source && !strcmp(s->name, sp->youtube_source))
				out_str(&o, " (YouTube)");
		}
		out_str(&o, k ? "\n" : "nothing\n");
	}
	{
		int k = 0, rest = 0;
		for (int i = 0; i < ns; i++)
			rest += is_rest(&in->sources[i]);
		out_fmt(&o, "- **Everything else**: %d source%s", rest, rest == 1 ? "" : "s");
		if (n_new)
			out_fmt(&o, ", %d new", n_new);
		for (int i = 0; i < ns && k < FDR_REPORT_MAX_NAMED; i++) {
			const struct fdr_report_source *s = &in->sources[i];
			if (!is_rest(s))
				continue;
			out_str(&o, k++ ? ", " : " — ");
			out_text(&o, src[i]);
			if (is_state(s, "new")) {
				out_str(&o, " (new)");
			} else if (is_state(s, "deleted")) {
				out_str(&o, " (its fader \"");
				out_text(&o, saved[i]);
				out_str(&o, "\" is gone)");
			} else if (is_state(s, "parked")) {
				out_str(&o, " (");
				out_text(&o, saved[i]);
				out_str(&o, "'s YouTube source, until it's on)");
			} else if (is_state(s, "extra")) {
				out_str(&o, " (");
				out_text(&o, saved[i]);
				out_str(&o, " takes one source per platform)");
			}
		}
		if (rest > k)
			out_fmt(&o, "%s%d more", k ? ", and " : " — ", rest - k);
		if (in->past_limit > 0)
			out_fmt(&o, "; %d more past the mixer's limit", in->past_limit);
		out_str(&o, "\n");
	}

	// ── Sources ──
	out_fmt(&o, "\n**Audio sources**: %d", in->audio_total);
	for (int j = 0; j < n_kinds; j++)
		out_fmt(&o, "%s%d %s", j ? ", " : " — ", kinds[j].n, kinds[j].kind);
	out_fmt(&o, "\nNot placed yet %d · muted %d · monitor only %d · not on Track %d %d\n", n_new, n_muted, n_monitor_only, track, n_off_track);

	// ── Log ──
	out_fmt(&o, "\n**Recent plugin log** (last %d of %llu, UTC)\n\n```text\n", in->n_log, (unsigned long long)in->log_total);
	for (int i = 0; i < in->n_log; i++) {
		struct tm t;
		utc_tm(in->log[i].time_s, &t);
		char ts[16];
		strftime(ts, sizeof(ts), "%H:%M:%S", &t);
		int lv = in->log[i].level;
		out_fmt(&o, "%s %s ", ts, lv <= FDR_LOG_ERROR ? "ERROR" : lv <= FDR_LOG_WARNING ? "WARN" : lv <= FDR_LOG_INFO ? "INFO" : "DEBUG");
		out_text(&o, logmsg[i]);
		out_str(&o, "\n");
	}
	out_str(&o, "```\n");

	// ── JSON ──
	out_str(&o, "\n<details><summary>Full details (JSON)</summary>\n\n```json\n{\n");
	out_str(&o, " \"schema\": 1,\n");
	out_fmt(&o, " \"generated\": {\"local\": \"%s\", \"utc\": \"%s\"},\n", local_iso, utc_iso);
	out_fmt(&o, " \"names\": %s,\n", in->names ? "true" : "false");
	out_fmt(&o, " \"redacted\": {\"plugin_key\": %d, \"stream_key\": %d, \"paths\": %d, \"emails\": %d, \"urls\": %d, \"tokens\": %d, \"ips\": %d},\n", c.removed.plugin_key, c.removed.stream_key, c.removed.paths,
		c.removed.emails, c.removed.urls, c.removed.tokens, c.removed.ips);
	out_str(&o, " \"plugin\": {\"version\": ");
	out_json(&o, in->plugin_version);
	out_fmt(&o, ", \"uptime_s\": %lld, \"config_dir\": ", (long long)in->plugin_uptime_s);
	out_json(&o, config_dir);
	out_str(&o, ", \"module_dir\": ");
	out_json(&o, module_dir);
	out_str(&o, ", \"update_available\": ");
	if (in->update_available && *in->update_available)
		out_json(&o, in->update_available);
	else
		out_str(&o, "null");
	out_str(&o, "},\n \"obs\": {\"version\": ");
	out_json(&o, in->obs_version);
	out_str(&o, ", \"locale\": ");
	out_json(&o, in->obs_locale);
	out_str(&o, ", \"streaming\": ");
	out_bool(&o, in->streaming);
	out_fmt(&o, ", \"stream_uptime_s\": %lld, \"stream_audio_track\": %d},\n", (long long)in->stream_uptime_s, in->stream_audio_track);
	out_str(&o, " \"system\": {\"os\": ");
	out_json(&o, in->sys.os);
	out_str(&o, ", \"os_version\": ");
	out_json(&o, in->sys.os_version);
	out_str(&o, ", \"arch\": ");
	out_json(&o, in->sys.arch);
	out_str(&o, ", \"cpu\": ");
	out_json(&o, in->sys.cpu);
	out_fmt(&o, ", \"cores_physical\": %d, \"cores_logical\": %d, \"ram_gb\": %d},\n", in->sys.cores_physical, in->sys.cores_logical, in->sys.ram_gb);
	out_fmt(&o, " \"audio\": {\"sample_rate\": %d, \"channels\": %d, \"mixer_ok\": %s},\n", in->sample_rate, in->channels, in->mixer_ok ? "true" : "false");
	out_fmt(&o, " \"settings\": {\"enabled\": %s, \"only_live\": %s, \"plugin_key_set\": %s},\n", in->enabled ? "true" : "false", in->only_live ? "true" : "false", in->plugin_key_set ? "true" : "false");
	out_str(&o, " \"account\": {\"channel\": ");
	out_json(&o, channel);
	out_str(&o, ", \"plan\": ");
	out_json(&o, in->plan);
	out_str(&o, ", \"platforms\": [");
	{
		const char *p = in->platforms ? in->platforms : "";
		for (int k = 0; *p; k++) {
			size_t n = strcspn(p, ",");
			char one[32];
			snprintf(one, sizeof(one), "%.*s", (int)(n < sizeof(one) ? n : sizeof(one) - 1), p);
			out_str(&o, k ? ", " : "");
			out_json(&o, one);
			p += n + (p[n] == ',');
		}
	}
	out_fmt(&o, "], \"listener_limit\": %d, \"max_stems\": %d},\n", in->listener_limit, in->max_stems);
	out_str(&o, " \"connection\": {\"running\": ");
	out_bool(&o, in->running);
	out_str(&o, ", \"connected\": ");
	out_bool(&o, in->connected);
	out_str(&o, ", \"ever_connected\": ");
	out_bool(&o, in->ever_connected);
	out_str(&o, ", \"server\": ");
	out_json(&o, in->server && *in->server ? in->server : "custom");
	out_fmt(&o, ", \"listeners\": %d, \"uplink_kbps\": %d, \"chunks_sent\": %llu, \"stems_on_wire\": %d, \"stream_track\": %d, \"stream_track_silent\": %s, \"last_failure\": ", in->listeners, in->uplink_kbps,
		in->chunks_sent, in->stems_on_wire, in->stream_track, in->stream_track_silent ? "true" : "false");
	out_json(&o, failure);
	out_fmt(&o, ", \"rejected_status\": %d},\n", in->rejected_status);
	out_str(&o, " \"problems\": {\"mix_mismatch\": ");
	out_json(&o, mismatch);
	out_str(&o, ", \"mix_duplicate\": ");
	out_json(&o, duplicate);
	out_str(&o, "},\n \"split\": {\"fader\": ");
	out_json(&o, split_fader);
	out_fmt(&o, ", \"ticked\": %s, \"on\": %s, \"why\": ", split_ticked ? "true" : "false", sp->on ? "true" : "false");
	out_json(&o, sp->why && *sp->why ? sp->why : "none");
	out_str(&o, ", \"twitch_source\": ");
	out_json(&o, split_tw);
	out_str(&o, ", \"youtube_source\": ");
	out_json(&o, split_yt);
	out_fmt(&o, ", \"youtube_track\": %d, \"split_max_faders\": %d, \"warnings\": [", sp->youtube_track, sp->max_faders);
	for (const char *w = sp->warnings ? sp->warnings : "", *sep = ""; *w; sep = ", ") {
		size_t n = strcspn(w, ",");
		char one[64];
		snprintf(one, sizeof(one), "%.*s", (int)(n < sizeof(one) ? n : sizeof(one) - 1), w);
		out_str(&o, sep);
		out_json(&o, one);
		w += n + (w[n] == ',');
	}
	out_str(&o, "]},\n \"faders\": [");
	for (int f = 0; f < nf; f++) {
		out_str(&o, f ? ",\n  {\"name\": " : "\n  {\"name\": ");
		out_json(&o, fader[f]);
		out_str(&o, ", \"on_wire\": ");
		out_bool(&o, in->faders[f].on_wire);
		out_str(&o, ", \"sources\": [");
		int k = 0;
		for (int i = 0; i < ns; i++) {
			const struct fdr_report_source *s = &in->sources[i];
			if (!(is_state(s, "ok") || is_state(s, "locked") || is_state(s, "dropped")) || !s->saved || strcmp(s->saved, in->faders[f].name))
				continue;
			out_str(&o, k++ ? ", " : "");
			out_json(&o, src[i]);
		}
		out_str(&o, "]}");
	}
	out_fmt(&o, "%s],\n \"allowance\": %d,\n", nf ? "\n " : "", in->allowance);
	int listed = ns < FDR_REPORT_MAX_LISTED ? ns : FDR_REPORT_MAX_LISTED;
	out_fmt(&o, " \"sources\": {\"audio_total\": %d, \"listed\": %d, \"truncated\": %s, \"past_limit\": %d, \"new\": %d, \"muted\": %d, \"monitor_only\": %d, \"not_on_stream_track\": %d,\n  \"by_kind\": {",
		in->audio_total, listed, listed < ns ? "true" : "false", in->past_limit, n_new, n_muted, n_monitor_only, n_off_track);
	for (int j = 0; j < n_kinds; j++) {
		out_str(&o, j ? ", " : "");
		out_json(&o, kinds[j].kind);
		out_fmt(&o, ": %d", kinds[j].n);
	}
	out_str(&o, "},\n  \"list\": [");
	for (int i = 0; i < listed; i++) {
		const struct fdr_report_source *s = &in->sources[i];
		out_fmt(&o, "%s   {\"i\": %d, \"name\": ", i ? ",\n" : "\n", i + 1);
		out_json(&o, src[i]);
		out_str(&o, ", \"kind\": ");
		out_json(&o, s->kind);
		out_str(&o, ", \"saved\": ");
		out_json(&o, saved[i]);
		out_str(&o, ", \"plays\": ");
		out_json(&o, *plays[i] ? plays[i] : "Everything else");
		out_str(&o, ", \"state\": ");
		out_json(&o, s->state);
		out_str(&o, ", \"tracks\": [");
		for (int t = 0, k = 0; t < 6; t++)
			if (s->tracks & (1u << t))
				out_fmt(&o, "%s%d", k++ ? ", " : "", t + 1);
		out_str(&o, "]");
		if (s->muted)
			out_str(&o, ", \"muted\": true");
		if (s->monitoring && strcmp(s->monitoring, "none"))
			out_str(&o, ", \"monitoring\": "), out_json(&o, s->monitoring);
		out_str(&o, "}");
	}
	out_fmt(&o, "%s]},\n", listed ? "\n  " : "");
	out_fmt(&o, " \"log\": {\"total\": %llu, \"lines\": [", (unsigned long long)in->log_total);
	for (int i = 0; i < in->n_log; i++) {
		struct tm t;
		utc_tm(in->log[i].time_s, &t);
		char ts[16];
		strftime(ts, sizeof(ts), "%H:%M:%S", &t);
		out_fmt(&o, "%s  {\"t\": \"%s\", \"level\": \"%s\", \"msg\": ", i ? ",\n" : "\n", ts, fdr_log_level_name(in->log[i].level));
		out_json(&o, logmsg[i]);
		out_str(&o, "}");
	}
	out_fmt(&o, "%s]}\n}\n```\n\n</details>\n", in->n_log ? "\n " : "");

	for (int i = 0; i < nf; i++)
		free(fader[i]);
	for (int i = 0; i < ns; i++) {
		free(src[i]);
		free(saved[i]);
		free(plays[i]);
	}
	for (int i = 0; i < in->n_log; i++)
		free(logmsg[i]);
	free(fader);
	free(src);
	free(saved);
	free(plays);
	free(logmsg);
	free(kinds);
	free(channel);
	free(failure);
	free(mismatch);
	free(duplicate);
	free(split_fader);
	free(split_tw);
	free(split_yt);
	free(config_dir);
	free(module_dir);
	free(c.names.a);

	// Fail closed: the keys must not be anywhere in it, typed fields included.
	if (fdr_redact_leaks(o.s, &in->redact)) {
		free(o.s);
		return -1;
	}
	*result = o.s;
	return 0;
}
