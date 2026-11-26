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
// The Faders uplink: encodes N stems as Opus and sends 500 ms chunks to the Faders hub (see
// PROTOCOL.md), pausing while nobody is listening. The stems come from the virtual mixer
// (mixer.c pushes every stem once per audio tick with fdr_uplink_push_tick).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "split.h"

typedef struct fdr_uplink fdr_uplink_t;

#define FDR_MAX_STEMS 16
#define FDR_STEM_NAME_MAX 32

typedef struct fdr_uplink_config {
	const char *server; // ws(s)://host/ingest/<channel>
	const char *key;    // fdr_live_…, sent as a Bearer token
	void (*update_available)(const char *version); // the hub's newer release; called outside the mutex
} fdr_uplink_config;

typedef struct fdr_uplink_status {
	bool running;
	bool connected;
	int listeners;
	int uplink_kbps;
	unsigned long long chunks_sent;
	char last_error[160];
	// Why the last connection attempt failed, kept until one succeeds (last_error is cleared
	// on every attempt): "" before any failure.
	char failure[160];
	int rejected_status;       // that failure's HTTP status when the hub refused it (401: key not known), else 0
	bool ever_connected;       // this uplink has connected at least once
	// From the hub's hello (0.4.0+ hubs): who the key belongs to. "" / 0 until it arrives.
	char channel[256];         // the channel's display name (the hub sends at most 60 characters)
	char plan[24];             // "free", "pro", …
	int listener_limit;        // viewers the plan lets mix at once; 0 = unknown or unlimited
	char platforms[32];        // "twitch", "youtube" or "twitch,youtube"
	int stream_track;          // OBS track the stream's audio encoder uses (1-6), 0 when not watched
	bool stream_track_silent;  // that track has been silent for a while although the layers aren't
	int stems;                 // layers on the wire (the faders plus "Everything else")
	int max_stems;             // layers the channel's plan allows (from the hub); 0 = not told
	char mix_mismatch[64];     // a source sent to Faders but missing from the stream; "" = fine
	char mix_duplicate[64];    // a source in the stream more than once (its fader carries one copy); "" = fine
	bool split;                // the stems include a split fader (two more layers: PROTOCOL-SPLIT.md)
	int split_track;           // while split: the OBS track that goes to YouTube; 0 = no split
	unsigned split_warnings;   // while split: FDR_SPLIT_WARN_* bits, sent with the status
} fdr_uplink_status;

fdr_uplink_t *fdr_uplink_start(const fdr_uplink_config *cfg);
void fdr_uplink_stop(fdr_uplink_t *u);
void fdr_uplink_get_status(fdr_uplink_t *u, fdr_uplink_status *out);
// Watch the level of the track OBS streams (1-6; 0 stops), fed by the mixer's tap. If it stays
// silent while the Faders layers have sound, the stream reaching viewers has no audio (a common
// track-setup mistake) and the extension can't sync: the status says so and the hub is told.
void fdr_uplink_watch_stream_track(fdr_uplink_t *u, int track);

// set_stems: the stem list (names in id order; `rest` = the last one is "Everything
// else"), on the main thread; takes effect at the next chunk boundary and is announced to the
// hub. push_tick: from the audio thread, one interleaved stereo float buffer per stem for this
// tick (n must equal the current stem count, else the tick is dropped); track_peak is the
// streamed track's peak for the silence check. set_mismatch: the mixer's oracle verdict (a
// source name, or "" when fine), reported to the hub.
void fdr_uplink_set_stems(fdr_uplink_t *u, const char *const *names, int n, bool rest);
// The same with each stem's split metadata (src/split.h): set_stems is set_layout with no split.
void fdr_uplink_set_layout(fdr_uplink_t *u, const struct fdr_stem_desc *stems, int n);
// While split: the YouTube track and the warnings (FDR_SPLIT_WARN_*), sent in the status message as
// "split" (PROTOCOL-SPLIT.md §3). youtube_track 0: no split, and no "split" in the status.
void fdr_uplink_set_split_status(fdr_uplink_t *u, int youtube_track, unsigned warnings);
void fdr_uplink_push_tick(fdr_uplink_t *u, uint64_t ts, const float *const *stems, int n, size_t frames, float track_peak);
void fdr_uplink_set_mismatch(fdr_uplink_t *u, const char *source_name);
// A source the oracle found in the stream more than once: shown in OBS, not sent to the hub.
void fdr_uplink_set_duplicate(fdr_uplink_t *u, const char *source_name);
int fdr_uplink_max_stems(fdr_uplink_t *u);
