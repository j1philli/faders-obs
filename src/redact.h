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
// The debug report's redactor (see redact.c). No OBS: tested on its own.
#pragma once

typedef struct fdr_redact_ctx {
	const char *plugin_key;    // settings "key" (fdr_live_…), or NULL
	const char *stream_key;    // OBS stream service "key", read in-process only to scrub it, or NULL
	const char *user_names[2]; // USER/USERNAME and the home folder's last segment, or NULL
} fdr_redact_ctx;

// What was removed, for the report's "Removed before you saw this: …" line.
typedef struct fdr_redact_counts {
	int plugin_key, stream_key, urls, tokens, emails, paths, ips;
} fdr_redact_counts;

// A new string (free() it) with every rule applied; counts (if given) says what was removed.
char *fdr_redact(const char *in, const fdr_redact_ctx *ctx, fdr_redact_counts *counts);
// 1 if a known secret is still in the finished report: then the report must not be shown.
int fdr_redact_leaks(const char *report, const fdr_redact_ctx *ctx);
