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
#include "logring.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t ring_mu = PTHREAD_MUTEX_INITIALIZER;
static struct fdr_log_line ring[FDR_LOG_LINES];
static uint64_t kept;        // under ring_mu
static uint64_t dropped;     // atomic: counted without the lock
static fdr_log_sink_t log_sink;// set once at load, before any other thread logs

#if defined(_MSC_VER)
#include <intrin.h>
#define ATOMIC_INC(p) _InterlockedIncrement64((volatile long long *)(p))
#define ATOMIC_LOAD(p) _InterlockedOr64((volatile long long *)(p), 0)
#define ATOMIC_STORE(p, v) _InterlockedExchange64((volatile long long *)(p), (long long)(v))
#else
#define ATOMIC_INC(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define ATOMIC_LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define ATOMIC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#endif

void fdr_log_set_sink(fdr_log_sink_t s)
{
	log_sink = s;
}

// Copy at most size-1 bytes of a UTF-8 string, cutting before a character that doesn't fit whole.
static void copy_utf8(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);
	if (n >= size) {
		n = size - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) // src[n] continues a character
			n--;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

// The lock is only ever held to copy a line or two dozen KB: a few quick retries get past another
// writer. A ring that stays busy longer (a report being copied out) costs the line, not a wait.
#define PUSH_TRIES 64

bool fdr_logring_push(int level, const char *msg)
{
	int tries = 0;
	while (pthread_mutex_trylock(&ring_mu) != 0) {
		if (++tries >= PUSH_TRIES) {
			ATOMIC_INC(&dropped);
			return false;
		}
	}
	struct fdr_log_line *l = &ring[kept % FDR_LOG_LINES];
	l->time_s = (int64_t)time(NULL);
	l->level = level;
	copy_utf8(l->msg, sizeof(l->msg), msg ? msg : "");
	l->seq = ++kept;
	pthread_mutex_unlock(&ring_mu);
	return true;
}

void fdr_log(int level, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fdr_logring_push(level, msg);
	if (log_sink)
		log_sink(level, msg);
}

int fdr_logring_copy(struct fdr_log_line *out, int max, uint64_t *total)
{
	pthread_mutex_lock(&ring_mu);
	uint64_t have = kept < FDR_LOG_LINES ? kept : FDR_LOG_LINES;
	int n = max < (int)have ? max : (int)have;
	for (int i = 0; i < n; i++)
		out[i] = ring[(kept - (uint64_t)n + (uint64_t)i) % FDR_LOG_LINES];
	if (total)
		*total = kept;
	pthread_mutex_unlock(&ring_mu);
	return n;
}

void fdr_logring_stats(uint64_t *k, uint64_t *d)
{
	pthread_mutex_lock(&ring_mu);
	if (k)
		*k = kept;
	pthread_mutex_unlock(&ring_mu);
	if (d)
		*d = (uint64_t)ATOMIC_LOAD(&dropped);
}

void fdr_logring_clear(void)
{
	pthread_mutex_lock(&ring_mu);
	kept = 0;
	memset(ring, 0, sizeof(ring));
	ATOMIC_STORE(&dropped, 0);
	pthread_mutex_unlock(&ring_mu);
}

const char *fdr_log_level_name(int level)
{
	return level <= FDR_LOG_ERROR ? "error" : level <= FDR_LOG_WARNING ? "warning" : level <= FDR_LOG_INFO ? "info" : "debug";
}

#ifdef FDR_LOGRING_TESTING
void fdr_logring_hold(bool on)
{
	if (on)
		pthread_mutex_lock(&ring_mu);
	else
		pthread_mutex_unlock(&ring_mu);
}
#endif
