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
// faders-logring-test: the plugin's recent-log ring (src/logring.c, no OBS) that the debug report
// quotes. It keeps the last 64 lines, cuts long ones between characters, and never blocks the
// caller: the audio thread logs through it, so a line that would have to wait is dropped and
// counted instead. One PASS/FAIL line per check; exits non-zero if any fails.
#define FDR_LOGRING_TESTING
#include "../src/logring.c"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static struct fdr_log_line lines[FDR_LOG_LINES];

static char sunk[256];
static int sunk_level, sunk_n;
static void sink(int level, const char *msg)
{
	sunk_level = level;
	snprintf(sunk, sizeof(sunk), "%s", msg);
	sunk_n++;
}

static void basics(void)
{
	fdr_logring_clear();
	fdr_log_set_sink(sink);
	fdr_log(FDR_LOG_INFO, "loaded (version %s)", "0.4.0");
	fdr_log(FDR_LOG_WARNING, "disconnected: %s", "WinHTTP 12030");
	uint64_t total = 0;
	int n = fdr_logring_copy(lines, FDR_LOG_LINES, &total);
	result(n == 2 && total == 2 && !strcmp(lines[0].msg, "loaded (version 0.4.0)") && lines[0].level == FDR_LOG_INFO && !strcmp(lines[1].msg, "disconnected: WinHTTP 12030") &&
		       lines[1].level == FDR_LOG_WARNING && lines[0].time_s > 1700000000,
	       "lines are kept in order with level and time", "%d line(s), first \"%s\" at %lld", n, n ? lines[0].msg : "", n ? (long long)lines[0].time_s : 0LL);
	result(sunk_n == 2 && sunk_level == FDR_LOG_WARNING && !strcmp(sunk, "disconnected: WinHTTP 12030"), "every line also goes to the sink (OBS's log)", "%d sunk, last \"%s\"", sunk_n, sunk);
	fdr_log_set_sink(NULL);
	result(!strcmp(fdr_log_level_name(FDR_LOG_ERROR), "error") && !strcmp(fdr_log_level_name(FDR_LOG_WARNING), "warning") && !strcmp(fdr_log_level_name(FDR_LOG_INFO), "info") &&
		       !strcmp(fdr_log_level_name(FDR_LOG_DEBUG), "debug"),
	       "level names", "");

	// More than 64: the oldest go.
	fdr_logring_clear();
	for (int i = 0; i < 100; i++)
		fdr_log(FDR_LOG_INFO, "line %d", i);
	n = fdr_logring_copy(lines, FDR_LOG_LINES, &total);
	result(n == 64 && total == 100 && !strcmp(lines[0].msg, "line 36") && !strcmp(lines[63].msg, "line 99"), "keeps the last 64 lines", "%d kept of %llu, first \"%s\" last \"%s\"", n, (unsigned long long)total,
	       lines[0].msg, lines[n - 1].msg);
	// A smaller copy gets the newest.
	struct fdr_log_line few[5];
	n = fdr_logring_copy(few, 5, NULL);
	result(n == 5 && !strcmp(few[0].msg, "line 95") && !strcmp(few[4].msg, "line 99"), "a short copy gets the newest lines", "first \"%s\"", n ? few[0].msg : "");

	// Long lines are cut between characters, never inside one.
	fdr_logring_clear();
	char longmsg[1024] = "";
	while (strlen(longmsg) < 600)
		strcat(longmsg, "é🎵a");
	fdr_log(FDR_LOG_INFO, "%s", longmsg);
	n = fdr_logring_copy(lines, FDR_LOG_LINES, NULL);
	size_t len = n ? strlen(lines[0].msg) : 0;
	bool whole = n == 1 && len > 100 && len < FDR_LOG_LINE_MAX;
	for (size_t i = 0; whole && i < len;) {
		unsigned char c = (unsigned char)lines[0].msg[i];
		size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
		whole = k && i + k <= len;
		i += k ? k : 1;
	}
	result(whole, "a long line is cut between characters", "%zu bytes kept", len);
}

// The audio thread must never wait: while a reader holds the ring, a line is dropped, at once.
static void never_blocks(void)
{
	fdr_logring_clear();
	fdr_logring_hold(true);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	bool kept = fdr_logring_push(FDR_LOG_WARNING, "from the audio thread");
	clock_gettime(CLOCK_MONOTONIC, &b);
	fdr_logring_hold(false);
	double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
	uint64_t pushed = 0, dropped = 0;
	fdr_logring_stats(&pushed, &dropped);
	int n = fdr_logring_copy(lines, FDR_LOG_LINES, NULL);
	result(!kept && dropped == 1 && n == 0 && ms < 5.0, "a busy ring drops the line instead of waiting", "kept %d, dropped %llu, %d line(s), push took %.3f ms", kept, (unsigned long long)dropped, n, ms);
}

#define WRITERS 4
#define PER_WRITER 20000
static volatile bool stop_reading;
static void *writer(void *arg)
{
	int id = (int)(intptr_t)arg;
	for (int i = 0; i < PER_WRITER; i++)
		fdr_log(FDR_LOG_INFO, "writer %d line %06d end", id, i);
	return NULL;
}
static int torn, out_of_order, snapshots;
static void *reader(void *arg)
{
	(void)arg;
	static struct fdr_log_line snap[FDR_LOG_LINES];
	while (!stop_reading) {
		int n = fdr_logring_copy(snap, FDR_LOG_LINES, NULL);
		int last[WRITERS];
		for (int w = 0; w < WRITERS; w++)
			last[w] = -1;
		for (int i = 0; i < n; i++) {
			int w, k;
			char tail[8];
			if (sscanf(snap[i].msg, "writer %d line %d %7s", &w, &k, tail) != 3 || strcmp(tail, "end") != 0 || w < 0 || w >= WRITERS || strlen(snap[i].msg) != 24) {
				torn++;
				continue;
			}
			if (k <= last[w])
				out_of_order++;
			last[w] = k;
		}
		snapshots++;
	}
	return NULL;
}

static void concurrency(void)
{
	fdr_logring_clear();
	stop_reading = false;
	pthread_t r, w[WRITERS];
	pthread_create(&r, NULL, reader, NULL);
	for (int i = 0; i < WRITERS; i++)
		pthread_create(&w[i], NULL, writer, (void *)(intptr_t)i);
	for (int i = 0; i < WRITERS; i++)
		pthread_join(w[i], NULL);
	stop_reading = true;
	pthread_join(r, NULL);
	uint64_t pushed = 0, dropped = 0, total = 0;
	fdr_logring_stats(&pushed, &dropped);
	int n = fdr_logring_copy(lines, FDR_LOG_LINES, &total);
	result(torn == 0 && out_of_order == 0 && snapshots > 0 && pushed + dropped == WRITERS * PER_WRITER && total == pushed && n == 64,
	       "4 writers and a reader at once: no torn or reordered lines", "%d snapshot(s), torn %d, out of order %d; kept %llu + dropped %llu = %d; %d in the ring", snapshots, torn, out_of_order,
	       (unsigned long long)pushed, (unsigned long long)dropped, WRITERS * PER_WRITER, n);
}

int main(void)
{
	basics();
	never_blocks();
	concurrency();
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
