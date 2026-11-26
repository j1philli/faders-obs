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
// The plugin's log: every line goes to OBS's log (through the sink the plugin sets) and the last
// 64 are kept here for the debug report. Safe from any thread, the audio thread included: a line
// that would have to wait for the ring (a report being copied out) is dropped from the ring and
// counted, never waited for. No libobs, so it is tested on its own (test/logring-test.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Same values as OBS's LOG_ERROR … LOG_DEBUG.
#define FDR_LOG_ERROR 100
#define FDR_LOG_WARNING 200
#define FDR_LOG_INFO 300
#define FDR_LOG_DEBUG 400

#define FDR_LOG_LINES 64
#define FDR_LOG_LINE_MAX 240 // bytes, with the terminator; longer lines are cut between characters

struct fdr_log_line {
	int64_t time_s; // UTC, seconds since 1970
	int level;
	char msg[FDR_LOG_LINE_MAX];
	uint64_t seq; // 1, 2, …: lines ever kept, in order
};

// Where every line also goes (the plugin: blog "[faders] …"), or NULL.
typedef void (*fdr_log_sink_t)(int level, const char *msg);
void fdr_log_set_sink(fdr_log_sink_t sink);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void fdr_log(int level, const char *fmt, ...);
// Keep one line; false if the ring was busy and it was dropped.
bool fdr_logring_push(int level, const char *msg);
// Copy the newest lines (at most max), oldest first; *total (if given) = lines ever kept.
int fdr_logring_copy(struct fdr_log_line *out, int max, uint64_t *total);
// Lines kept and dropped since the start (or the last clear).
void fdr_logring_stats(uint64_t *kept, uint64_t *dropped);
void fdr_logring_clear(void);
const char *fdr_log_level_name(int level); // "error", "warning", "info", "debug"

#ifdef FDR_LOGRING_TESTING
// Tests: hold the ring as a reader would, to see that writers don't wait.
void fdr_logring_hold(bool on);
#endif
