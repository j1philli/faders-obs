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
// The debug report's redactor. Plain C, no regex library (MinGW cross-builds have no <regex.h>),
// no libobs, so it is tested on its own (test/redact-test.c: the 59-case table). Each rule is one
// pass over the string; the regex in each comment is the spec, the hand-written matcher is the
// implementation.
//
// Two layers:
//   1. Known secrets, by value: the plugin key, the OBS stream key and the OS user name are read
//      in-process and removed wherever they appear (exact match), then the final report is checked
//      for them again; if one survives, nothing is shown (fail closed).
//   2. Pattern rules for secrets we can't know (another service's token in a log line, a URL a
//      browser source logged, an email in a source name).
// Only free-text fields go through layer 2 (names, log lines, failure strings); typed fields
// (numbers, booleans, enums, versions) are built from an allowlist and never contain user text.
#include "redact.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char *s;
	size_t len, cap;
} buf;

static void put(buf *b, const char *p, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = (b->len + n + 1) * 2;
		char *s = realloc(b->s, cap);
		if (!s)
			abort(); // out of memory building a few KB of text: nothing sensible to do
		b->s = s;
		b->cap = cap;
	}
	memcpy(b->s + b->len, p, n);
	b->len += n;
	b->s[b->len] = 0;
}
static void puts_(buf *b, const char *p) { put(b, p, strlen(p)); }

// c is in set (strchr alone would say yes for the terminating 0).
static int in(const char *set, int c) { return c && strchr(set, c); }
static int is_word(int c) { return isalnum(c) || c == '_'; }
static int is_tok(int c) { return isalnum(c) || c == '_' || c == '-'; }
static int ieq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
			return 0;
	return 1;
}
// Already-redacted text is never matched again ("[REDACTED…]", "[user]", "[email]", "[ip]").
static size_t placeholder_len(const char *p)
{
	if (*p != '[')
		return 0;
	static const char *const ph[] = {"[REDACTED", "[user]", "[email]", "[ip]", "[host]"};
	for (size_t i = 0; i < sizeof(ph) / sizeof(*ph); i++)
		if (!strncmp(p, ph[i], strlen(ph[i]))) {
			const char *e = strchr(p, ']');
			return e ? (size_t)(e - p + 1) : 0;
		}
	return 0;
}

// A pass: m(p, start) returns how many bytes at p it replaces (0 = no match) and writes the
// replacement into rep.
typedef size_t (*matcher)(const char *p, const char *start, buf *rep, const fdr_redact_ctx *ctx);

static char *run(const char *in, matcher m, const fdr_redact_ctx *ctx, int *count)
{
	buf out = {0}, rep = {0};
	put(&out, "", 0);
	for (const char *p = in; *p;) {
		size_t ph = placeholder_len(p);
		if (ph) {
			put(&out, p, ph);
			p += ph;
			continue;
		}
		rep.len = 0;
		size_t n = m(p, in, &rep, ctx);
		if (n) { // a matcher may return text unchanged to skip over it (a URL, a done path)
			put(&out, rep.s, rep.len);
			if (count && (rep.len != n || memcmp(rep.s, p, n)))
				(*count)++;
			p += n;
		} else {
			put(&out, p, 1);
			p++;
		}
	}
	free(rep.s);
	return out.s;
}

// ── Layer 1: known values ──────────────────────────────────────────────────────────────
// The plugin key and stream key: exact, case-sensitive. The user name: case-insensitive, at
// word boundaries, only if 3+ characters (a 2-letter name would eat ordinary words).
static size_t m_secret(const char *p, const char *v, const char *placeholder, buf *rep)
{
	size_t n = v ? strlen(v) : 0;
	if (n >= 8 && !strncmp(p, v, n)) {
		puts_(rep, placeholder);
		return n;
	}
	return 0;
}
static size_t m_plugin_key(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)start;
	return m_secret(p, c->plugin_key, "[REDACTED:plugin-key]", rep);
}
static size_t m_stream_key(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)start;
	return m_secret(p, c->stream_key, "[REDACTED:stream-key]", rep);
}

static size_t m_user(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	for (size_t i = 0; i < 2; i++) {
		const char *u = c->user_names[i];
		size_t n = u ? strlen(u) : 0;
		if (n >= 3 && ieq(p, u, n) && (p == start || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[n])) {
			puts_(rep, "[user]");
			return n;
		}
	}
	return 0;
}

// ── Layer 2: patterns ──────────────────────────────────────────────────────────────────

// Shannon entropy in bits per character.
static double entropy(const char *p, size_t n)
{
	int f[256] = {0};
	for (size_t i = 0; i < n; i++)
		f[(unsigned char)p[i]]++;
	double h = 0;
	for (int i = 0; i < 256; i++)
		if (f[i]) {
			double q = (double)f[i] / n;
			h -= q * log2(q);
		}
	return h;
}
// Looks like a random secret: ≥ 16 hex digits (dashes allowed: UUIDs, hashes, hex tokens) with
// letters and digits, or ≥ 20 chars of [A-Za-z0-9_+/=-] with letters and digits and ≥ 4.0 bits/char.
static int secret_like(const char *p, size_t n)
{
	int d = 0, a = 0, hex = 1;
	for (size_t i = 0; i < n; i++) {
		hex &= isxdigit((unsigned char)p[i]) || p[i] == '-';
		d += isdigit((unsigned char)p[i]) != 0;
		a += isalpha((unsigned char)p[i]) != 0;
	}
	if (n >= 16 && hex && d && a)
		return 1;
	if (n < 20)
		return 0;
	d = a = 0;
	for (size_t i = 0; i < n; i++) {
		if (isdigit((unsigned char)p[i]))
			d++;
		else if (isalpha((unsigned char)p[i]))
			a++;
		else if (!strchr("_+/=-", p[i]))
			return 0;
	}
	return d && a && entropy(p, n) >= 4.0;
}

// URL: (?i)\b([a-z][a-z0-9+.-]+)://(userinfo@)?(host[:port])(/path)?(\?query)?(#frag)?
//   userinfo → [REDACTED]@ · query → ?[REDACTED] · fragment → #[REDACTED]
//   rtmp/rtmps/srt/rist/rtsp: everything after the first path segment → /[REDACTED]
//   http(s)/ws(s) and others: a path segment that is secret_like → [REDACTED]
//   trailing . , ; : ! ? ) ] are punctuation, not part of the URL.
static size_t m_url(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (!isalpha((unsigned char)*p) || (p > start && (isalnum((unsigned char)p[-1]) || p[-1] == '+' || p[-1] == '.' || p[-1] == '-')))
		return 0;
	const char *q = p;
	while (isalnum((unsigned char)*q) || *q == '+' || *q == '.' || *q == '-')
		q++;
	if (q - p < 2 || strncmp(q, "://", 3) != 0)
		return 0;
	size_t scheme_n = q - p;
	const char *e = q + 3;
	while (*e && !isspace((unsigned char)*e) && !strchr("\"'<>`|", *e))
		e++;
	while (e > q + 3 && in(".,;:!?)", e[-1]))
		e--;
	static const char *const streaming[] = {"rtmp", "rtmps", "rtmpt", "srt", "rist", "rtsp"};
	int stream = 0;
	for (size_t i = 0; i < sizeof(streaming) / sizeof(*streaming); i++)
		stream |= scheme_n == strlen(streaming[i]) && ieq(p, streaming[i], scheme_n);
	put(rep, p, scheme_n + 3);
	const char *a = q + 3, *ae = a;
	while (ae < e && *ae != '/' && *ae != '?' && *ae != '#')
		ae++;
	const char *at = NULL;
	for (const char *x = a; x < ae; x++)
		if (*x == '@')
			at = x;
	if (at) {
		puts_(rep, "[REDACTED]@");
		a = at + 1;
	}
	put(rep, a, ae - a);
	const char *x = ae;
	int seg = 0;
	while (x < e && *x == '/') {
		const char *s = x + 1, *se = s;
		while (se < e && *se != '/' && *se != '?' && *se != '#')
			se++;
		if (stream && seg >= 1) {
			puts_(rep, "/[REDACTED]");
			while (se < e && *se != '?' && *se != '#')
				se++;
			x = se;
			break;
		}
		puts_(rep, "/");
		if (secret_like(s, se - s))
			puts_(rep, "[REDACTED]");
		else
			put(rep, s, se - s);
		x = se;
		seg++;
	}
	if (x < e && *x == '?') { // to the end: SRT stream ids contain '#'
		puts_(rep, "?[REDACTED]");
		x = e;
	}
	if (x < e && *x == '#')
		puts_(rep, "#[REDACTED]");
	return e - p;
}

// JWT: \beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]* → [REDACTED:jwt]
static size_t m_jwt(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (strncmp(p, "eyJ", 3) || (p > start && is_tok((unsigned char)p[-1])))
		return 0;
	const char *q = p;
	for (int part = 0; part < 3; part++) {
		const char *s = q;
		while (is_tok((unsigned char)*q))
			q++;
		if (part < 2 && (q - s < 8 || *q != '.'))
			return 0;
		if (part < 2)
			q++;
	}
	puts_(rep, "[REDACTED:jwt]");
	return q - p;
}

// Known token shapes:
//   \bfdr_(live|test)_[A-Za-z0-9]{8,}          → fdr_live_[REDACTED]   (Faders plugin key)
//   \blive_[0-9]{3,}_[A-Za-z0-9]{10,}           → [REDACTED:stream-key] (Twitch)
//   \b[a-z0-9]{4}(-[a-z0-9]{4}){3,4}\b, ≥1 digit → [REDACTED:stream-key] (YouTube)
//   \b(sk|rk|ghp|gho|xox[abpr])[-_][A-Za-z0-9_-]{16,} → [REDACTED:token] (Kick, GitHub, Slack…)
static size_t m_shapes(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (p > start && is_word((unsigned char)p[-1]))
		return 0;
	const char *q;
	if (!strncmp(p, "fdr_live_", 9) || !strncmp(p, "fdr_test_", 9)) {
		for (q = p + 9; isalnum((unsigned char)*q); q++)
			;
		if (q - p - 9 >= 8) {
			put(rep, p, 9);
			puts_(rep, "[REDACTED]");
			return q - p;
		}
	}
	if (!strncmp(p, "live_", 5)) {
		for (q = p + 5; isdigit((unsigned char)*q); q++)
			;
		const char *d = q;
		if (d - p - 5 >= 3 && *q == '_') {
			for (q++; isalnum((unsigned char)*q); q++)
				;
			if (q - d - 1 >= 10) {
				puts_(rep, "[REDACTED:stream-key]");
				return q - p;
			}
		}
	}
	{ // YouTube
		int groups = 0, digit = 0;
		q = p;
		for (;;) {
			int k = 0;
			while (k < 4 && (islower((unsigned char)q[k]) || isdigit((unsigned char)q[k])))
				digit |= isdigit((unsigned char)q[k]), k++;
			if (k < 4)
				break;
			groups++;
			q += 4;
			if (*q != '-' || groups == 5)
				break;
			q++;
		}
		if (q > p && q[-1] == '-') // a trailing '-' isn't part of it
			q--;
		if (groups >= 4 && digit && !is_tok((unsigned char)*q)) {
			puts_(rep, "[REDACTED:stream-key]");
			return q - p;
		}
	}
	static const char *const pre[] = {"sk", "rk", "ghp", "gho", "xoxa", "xoxb", "xoxp", "xoxr"};
	for (size_t i = 0; i < sizeof(pre) / sizeof(*pre); i++) {
		size_t n = strlen(pre[i]);
		if (strncmp(p, pre[i], n) || (p[n] != '_' && p[n] != '-'))
			continue;
		for (q = p + n + 1; is_tok((unsigned char)*q); q++)
			;
		if (q - p - n - 1 >= 16) {
			puts_(rep, "[REDACTED:token]");
			return q - p;
		}
	}
	return 0;
}

// Header / key=value secrets:
//   (?i)\b(authorization|cookie|set-cookie)\s*:\s*.*$            → $1: [REDACTED]   (rest of line)
//   (?i)\b(bearer|basic)\s+[A-Za-z0-9._~+/=-]{8,}               → $1 [REDACTED]
//   (?i)\b(NAME)"?\s*[:=]\s*("[^"]*"|'[^']*'|[^\s&,;]+)        → $1…[REDACTED]
//   NAME ∈ key stream_key streamkey token access_token refresh_token id_token auth password passwd
//          pwd secret client_secret api_key apikey x-api-key session sessionid session_id sid sig
//          signature passphrase streamid   (not "code": it would eat "error code: 1006")
static size_t m_kv(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (p > start && (is_word((unsigned char)p[-1]) || p[-1] == '-'))
		return 0;
	static const char *const line[] = {"authorization", "set-cookie", "cookie"};
	for (size_t i = 0; i < 3; i++) {
		size_t n = strlen(line[i]);
		const char *q = p + n;
		if (!ieq(p, line[i], n) || is_word((unsigned char)*q))
			continue;
		while (*q == ' ')
			q++;
		if (*q != ':')
			continue;
		put(rep, p, n);
		puts_(rep, ": [REDACTED]");
		while (*q && *q != '\n')
			q++;
		return q - p;
	}
	static const char *const scheme[] = {"bearer", "basic"};
	for (size_t i = 0; i < 2; i++) {
		size_t n = strlen(scheme[i]);
		const char *q = p + n;
		if (!ieq(p, scheme[i], n) || *q != ' ')
			continue;
		while (*q == ' ')
			q++;
		const char *v = q;
		while (isalnum((unsigned char)*q) || in("._~+/=-", *q))
			q++;
		if (q - v >= 8) {
			put(rep, p, v - p);
			puts_(rep, "[REDACTED]");
			return q - p;
		}
	}
	static const char *const names[] = {"stream_key", "streamkey", "access_token", "refresh_token", "id_token", "client_secret", "api_key",
					    "apikey", "x-api-key", "session_id", "sessionid", "session", "passphrase", "password", "passwd", "pwd",
					    "secret", "signature", "sig", "token", "auth", "streamid", "sid", "key"};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
		size_t n = strlen(names[i]);
		const char *q = p + n;
		if (!ieq(p, names[i], n) || is_word((unsigned char)*q) || *q == '-')
			continue;
		if (*q == '"' || *q == '\'')
			q++;
		while (*q == ' ')
			q++;
		if (*q != '=' && *q != ':')
			continue;
		q++;
		while (*q == ' ')
			q++;
		const char *v = q;
		if (*q == '"' || *q == '\'') {
			char quote = *q++;
			while (*q && *q != quote)
				q++;
			if (*q)
				q++;
		} else {
			while (*q && !isspace((unsigned char)*q) && !in("&,;", *q))
				q++;
		}
		if (q == v || placeholder_len(v))
			return 0;
		put(rep, p, v - p);
		puts_(rep, "[REDACTED]");
		return q - p;
	}
	return 0;
}

// Email: [A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,} → [email]
static size_t m_email(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (p > start && (isalnum((unsigned char)p[-1]) || in("._%+-", p[-1])))
		return 0;
	const char *q = p;
	while (isalnum((unsigned char)*q) || in("._%+-", *q))
		q++;
	if (q == p || *q != '@')
		return 0;
	const char *d = ++q, *last_dot = NULL;
	while (isalnum((unsigned char)*q) || *q == '-' || (*q == '.' && isalnum((unsigned char)q[1])))
		last_dot = *q == '.' ? q : last_dot, q++;
	if (!last_dot || last_dot == d || q - last_dot - 1 < 2)
		return 0;
	for (const char *t = last_dot + 1; t < q; t++)
		if (!isalpha((unsigned char)*t))
			return 0;
	puts_(rep, "[email]");
	return q - p;
}

// Home folders (the user name is the segment after them):
//   (?i)\b[A-Z]:[\\/]+(Users|Documents and Settings)[\\/]+([^\\/\s"']+) → …[user]
//   /Users/([^/\s"']+) (not "Shared") · /home/([^/\s"']+) · /var/home/([^/\s"']+) → …[user]
//   \\\\host\\share (UNC)                                              → \\[host]\…
static size_t m_home(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	const char *q = NULL;
	if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/') && (p == start || !isalnum((unsigned char)p[-1]))) {
		q = p + 2;
		while (*q == '\\' || *q == '/')
			q++;
		size_t n = ieq(q, "Users", 5) ? 5 : ieq(q, "Documents and Settings", 22) ? 22 : 0;
		if (!n || (q[n] != '\\' && q[n] != '/'))
			return 0;
		q += n;
		while (*q == '\\' || *q == '/')
			q++;
	} else if (*p == '/' && (p == start || !is_word((unsigned char)p[-1]))) {
		static const char *const roots[] = {"/Users/", "/home/", "/var/home/"};
		for (size_t i = 0; i < 3 && !q; i++)
			if (!strncmp(p, roots[i], strlen(roots[i])))
				q = p + strlen(roots[i]);
		if (!q || !strncmp(q, "Shared", 6))
			return 0;
	} else if (p[0] == '\\' && p[1] == '\\' && (p == start || isspace((unsigned char)p[-1]) || in("\"'(=", p[-1]))) {
		// UNC only at the start of a token: "\\host\share", or JSON-escaped "\\\\host\\share".
		// Inside a path, "\\" is a JSON-escaped separator, not a host.
		q = p + 2;
		if (q[0] == '\\' && q[1] == '\\')
			q += 2;
		const char *h = q;
		if (!isalnum((unsigned char)*h) || placeholder_len(h))
			return 0;
		while (*q && *q != '\\' && !isspace((unsigned char)*q))
			q++;
		if (*q != '\\')
			return 0;
		put(rep, p, h - p);
		puts_(rep, "[host]");
		return q - p;
	} else {
		return 0;
	}
	const char *u = q;
	while (*q && !strchr("\\/\"'\n\t", *q) && !(q[0] == ' ' && (q[1] == '(' || q[1] == ' ')))
		q++;
	if (q == u)
		return 0;
	put(rep, p, u - p);
	if (placeholder_len(u)) // done already: skip it whole, unchanged
		put(rep, u, q - u);
	else
		puts_(rep, "[user]");
	return q - p;
}

// IPv4 (not loopback): \b(\d{1,3}\.){3}\d{1,3}\b, each ≤ 255, not followed by another .digit → [ip]
static size_t m_ip(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (!isdigit((unsigned char)*p) || (p > start && (isalnum((unsigned char)p[-1]) || p[-1] == '.')))
		return 0;
	const char *q = p;
	int oct[4];
	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)*q))
			return 0;
		int v = 0, k = 0;
		while (isdigit((unsigned char)*q) && k < 4)
			v = v * 10 + (*q++ - '0'), k++;
		if (k > 3 || v > 255)
			return 0;
		oct[i] = v;
		if (i < 3 && *q++ != '.')
			return 0;
	}
	if (isalnum((unsigned char)*q) || (*q == '.' && isdigit((unsigned char)q[1])))
		return 0;
	if (oct[0] == 127 || (oct[0] == 0 && oct[1] == 0 && oct[2] == 0 && oct[3] == 0))
		return 0;
	puts_(rep, "[ip]");
	return q - p;
}

// Backstop: any run of [A-Za-z0-9_+/=-] that is secret_like (≥ 20 chars, letters and digits,
// ≥ 4.0 bits/char) → [REDACTED:token]
static size_t m_entropy(const char *p, const char *start, buf *rep, const fdr_redact_ctx *c)
{
	(void)c;
	if (p > start && (isalnum((unsigned char)p[-1]) || in("_+/=-", p[-1])))
		return 0;
	const char *q = p;
	while (isalnum((unsigned char)*q) || in("+.-", *q))
		q++;
	if (q - p >= 2 && !strncmp(q, "://", 3)) { // a URL: the URL pass already judged it, skip it whole
		while (*q && !isspace((unsigned char)*q) && !in("\"'<>`|", *q))
			q++;
		put(rep, p, q - p);
		return q - p;
	}
	q = p;
	while (isalnum((unsigned char)*q) || in("_+/=-", *q))
		q++;
	if (!secret_like(p, q - p))
		return 0;
	puts_(rep, "[REDACTED:token]");
	return q - p;
}

char *fdr_redact(const char *in, const fdr_redact_ctx *ctx, fdr_redact_counts *counts)
{
	fdr_redact_counts dummy;
	if (!counts)
		counts = &dummy;
	memset(counts, 0, sizeof(*counts));
	struct {
		matcher m;
		int *count;
	} passes[] = {
		{m_plugin_key, &counts->plugin_key}, {m_stream_key, &counts->stream_key},  {m_url, &counts->urls},    {m_jwt, &counts->tokens}, {m_shapes, &counts->tokens},
		{m_kv, &counts->tokens},    {m_email, &counts->emails}, {m_home, &counts->paths}, {m_ip, &counts->ips},
		{m_entropy, &counts->tokens}, {m_user, &counts->paths},
	};
	size_t len = in ? strlen(in) : 0;
	char *cur = malloc(len + 1);
	if (!cur)
		abort();
	memcpy(cur, in ? in : "", len + 1);
	for (size_t i = 0; i < sizeof(passes) / sizeof(*passes); i++) {
		char *next = run(cur, passes[i].m, ctx, passes[i].count);
		free(cur);
		cur = next;
	}
	return cur;
}

int fdr_redact_leaks(const char *report, const fdr_redact_ctx *ctx)
{
	const char *known[] = {ctx->plugin_key, ctx->stream_key};
	for (size_t i = 0; i < 2; i++)
		if (known[i] && strlen(known[i]) >= 8 && strstr(report, known[i]))
			return 1;
	return 0;
}
