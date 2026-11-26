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
// The split fader's mixing (src/split.h for the model): a second tap on the track the streamer
// picked for YouTube, the split fader's YouTube stem (F@youtube, from its YouTube source), and
// YouTube's own Everything else (EE@youtube = YouTube track − shared faders − F@youtube). The
// virtual mixer (mixer.c) keeps everything else: the shared faders, the split fader's Twitch
// stem (its Twitch source, as a normal fader) and Twitch's Everything else, all from the
// streamed track, computed once. The YouTube source is never in the mixer's lanes: while the
// split is on it is mixed here; while it is off it plays in Everything else.
//
// With no split on, nothing here runs per tick and the mixer pushes exactly what it did before.
//
// Threads: the audio thread calls fdr_splitmix_tick (from the mixer's tap) and the YouTube tap;
// the mixer's rebuild calls refresh (no locks held) and parks/announce (mixer lock held); the
// mixer's rebuild thread calls poll. Lock order: mixer → splitmix → uplink.
#pragma once
#include <obs.h>
#include <util/dstr.h>
#include "uplink.h"
#include "split.h"

typedef struct fdr_splitmix fdr_splitmix_t;

fdr_splitmix_t *fdr_splitmix_create(fdr_uplink_t *uplink, int stream_track);
void fdr_splitmix_destroy(fdr_splitmix_t *s);
// The saved split: the fader's name ("" = none) and the YouTube track (0 = not picked). Takes
// effect at the next rebuild.
void fdr_splitmix_set(fdr_splitmix_t *s, const char *fader, int youtube_track);
void fdr_splitmix_set_stream_track(fdr_splitmix_t *s, int stream_track);
// Rebuild, before the mixer collects its sources: finds the split fader's sources, reads the
// account from the hub's hello, decides whether the split is on, taps or untaps the YouTube track.
// True when the stem layout must be announced again.
bool fdr_splitmix_refresh(fdr_splitmix_t *s);
// Whether the mixer leaves this source out of its faders (it then plays in Twitch's Everything
// else): the split fader's YouTube source, and anything else saved on the split fader besides its
// Twitch source. Mixer lock held.
bool fdr_splitmix_parks(fdr_splitmix_t *s, obs_source_t *src);
// The mixer's stem announcement (wire fader names). True when the split's layout was announced
// (the mixer then announces nothing itself). Mixer lock held.
bool fdr_splitmix_announce(fdr_splitmix_t *s, const char *const *names, int n_wire);
// Rebuild thread, every 50 ms: sends the split's status (track, warnings) and says whether the
// account changed (a rebuild is due).
bool fdr_splitmix_poll(fdr_splitmix_t *s);
// Audio thread, the mixer's tick, after it built its stems (interleaved; n = faders + Everything
// else) from the streamed track (planar trk_l/trk_r). True: the split's layout is on the wire and
// this pushes the tick (with F@youtube and EE@youtube); false: the mixer pushes as before.
// fading: a transition is being modelled; hold: the oracle takes no evidence now.
bool fdr_splitmix_tick(fdr_splitmix_t *s, uint64_t ts, const float *const *stems, int n, const float *trk_l, const float *trk_r, float track_peak, bool fading, bool hold);
// Test hook: the split's state and, since the last call, each platform's layers (RMS), how much
// of each platform's music leaked into the other's Everything else, and each platform's oracle
// error (max |track − Σ its layers|). Resets the window.
void fdr_splitmix_stats_json(fdr_splitmix_t *s, struct dstr *out);
