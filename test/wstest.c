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
// The transport's shutdown contract (ws.h): once fdr_ws_destroy() returns, no callback is running
// and none will start. The uplink frees itself right after destroying its socket, so a callback
// still in flight would be using freed memory.
//
// Against test/ws-flood.mjs (a server that never stops sending), each round connects, lets
// message callbacks run (each one lingers a few milliseconds, as a callback waiting for the
// uplink's lock does), destroys the socket and marks the owner dead at once. A callback that
// finds its owner dead, on entry or on its way out, broke the contract.
//   faders-wstest ws://127.0.0.1:18797 [rounds]
#include "../src/ws.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct owner {
	atomic_bool alive;
	atomic_int messages;
};

static atomic_int after_destroy; // callbacks that ran, or were still running, after destroy returned

static void touch(struct owner *o)
{
	if (!atomic_load(&o->alive))
		atomic_fetch_add(&after_destroy, 1);
}

static void on_open(void *param)
{
	touch(param);
}
static void on_text(void *param, const char *text)
{
	(void)text;
	struct owner *o = param;
	touch(o);
	atomic_fetch_add(&o->messages, 1);
	usleep(3000);
	touch(o);
}
static void on_close(void *param, int http_status, const char *reason)
{
	if (getenv("WSTEST_DEBUG"))
		fprintf(stderr, "close: %d %s\n", http_status, reason);
	touch(param);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s ws://host:port [rounds]\n", argv[0]);
		return 2;
	}
	int rounds = argc > 2 ? atoi(argv[2]) : 40;
	static const fdr_ws_callbacks cb = {on_open, on_text, on_close};
	int received = 0, connected = 0;
	for (int r = 0; r < rounds; r++) {
		// Owners are never freed: a late callback is counted, not a crash.
		struct owner *o = calloc(1, sizeof(*o));
		atomic_store(&o->alive, true);
		fdr_ws_t *ws = fdr_ws_connect(argv[1], NULL, &cb, o);
		if (!ws) {
			fprintf(stderr, "connect failed\n");
			return 2;
		}
		for (int i = 0; i < 400 && atomic_load(&o->messages) < 3; i++)
			usleep(5000);
		usleep(1000 * (unsigned)(r % 7)); // land at different points of a callback
		fdr_ws_destroy(ws);
		atomic_store(&o->alive, false);
		if (atomic_load(&o->messages) > 0)
			connected++;
		received += atomic_load(&o->messages);
		usleep(20000); // long enough for a callback that was in flight to finish
	}
	usleep(200000);
	int late = atomic_load(&after_destroy);
	bool ok = late == 0 && connected == rounds;
	printf("%s  %d rounds, %d connected, %d messages, %d callback(s) ran after fdr_ws_destroy returned\n", ok ? "PASS" : "FAIL", rounds, connected, received, late);
	return ok ? 0 : 1;
}
