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
// The Faders uplink: taps three OBS mixer tracks, encodes them as Opus, and sends 500 ms chunks
// to the Faders hub (see PROTOCOL.md). Pauses its upload while nobody is listening.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct fdr_uplink fdr_uplink_t;

typedef struct fdr_uplink_config {
	const char *server; // ws(s)://host/ingest/<channel>
	const char *key;    // fdr_live_…, sent as a Bearer token
	int tracks[3];      // OBS mixer tracks (1-6) for voice, game, music
} fdr_uplink_config;

typedef struct fdr_uplink_status {
	bool running;
	bool connected;
	int listeners;
	int uplink_kbps;
	unsigned long long chunks_sent;
	char last_error[160];
	char update_available[24]; // newer published version the hub told us about, or ""
	int stream_track;          // OBS track the stream's audio encoder uses (1-6), 0 when not watched
	bool stream_track_silent;  // that track has been silent for a while although our tracks aren't

} fdr_uplink_status;

fdr_uplink_t *fdr_uplink_start(const fdr_uplink_config *cfg);
void fdr_uplink_stop(fdr_uplink_t *u);
void fdr_uplink_get_status(fdr_uplink_t *u, fdr_uplink_status *out);
// Watch the level of the track OBS streams (1-6; 0 stops). If it stays silent while the Faders
// tracks have sound, the stream reaching viewers has no audio (a common track-setup mistake) and
// the extension can't sync: the status says so and the hub is told, for the portal.
void fdr_uplink_watch_stream_track(fdr_uplink_t *u, int track);
