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
// The virtual mixer (sources mode): builds every Faders layer from per-source audio instead of
// OBS mixer tracks, plus "Everything else" = the streamed track minus the layers, so the sum of
// all layers is the stream, sample for sample. See PROTOCOL.md "Virtual mixer" for the libobs
// facts this relies on.
//
// Main thread: fader names (settings), source discovery and assignment (per-source private
// setting "faders_fader"), transition bookkeeping. Audio thread: one raw callback on the
// streamed track per tick, gating + summing per-source buffers (obs_source_get_audio_mix) into
// the faders, the residual, an oracle that drops a source whose audio shows up in the residual,
// and a push of every stem to the uplink.
#pragma once
#include <obs.h>
#include <util/dstr.h>
#include "uplink.h"

typedef struct fdr_mixer fdr_mixer_t;

// Private setting on an audio source: which fader it belongs to, by name. "" = Everything else;
// absent = automatic (FDR_AUTO: the rule table decides, see fdr_mixer_auto_fader).
#define FDR_FADER_SETTING "faders_fader"
// Test hook (private setting, bool): include the source even if the gate says no. Used to
// inject a mismatch and watch the oracle drop it.
#define FDR_FORCE_SETTING "faders_test_force"
#define FDR_MAX_FADERS (FDR_MAX_STEMS - 1)

// Needs stereo OBS audio output; returns NULL otherwise (the caller falls back to tracks mode).
// stream_track: the track OBS streams (1-6), the one the residual is measured against.
fdr_mixer_t *fdr_mixer_start(fdr_uplink_t *uplink, int stream_track);
void fdr_mixer_stop(fdr_mixer_t *m);
void fdr_mixer_set_stream_track(fdr_mixer_t *m, int stream_track);
// libobs doesn't expose a running transition's duration: the frontend's setting is passed in
// here (obs_frontend_get_transition_duration), and the mixer also learns it from the fade it
// watches (used when none was given, e.g. headless). 0 = unknown.
void fdr_mixer_set_transition_duration(fdr_mixer_t *m, int duration_ms);
// Fader names in id order (≤ FDR_MAX_FADERS, ≤ FDR_STEM_NAME_MAX chars each) and the plan's
// allowance: faders past max_stems are folded into Everything else, never dropped.
void fdr_mixer_set_faders(fdr_mixer_t *m, const char *const *names, int n, int max_stems);
// Re-read the sources and their assignments (after a change the mixer didn't see itself).
void fdr_mixer_rebuild(fdr_mixer_t *m);
// Fader names as configured, how many are on the wire (the rest folded), and the mismatch.
int fdr_mixer_fader_count(fdr_mixer_t *m);
int fdr_mixer_faders_on_wire(fdr_mixer_t *m);
const char *fdr_mixer_fader_name(fdr_mixer_t *m, int i);

// The rule table's pick for a source ("Voice", "Game", "Music", "Discord", "Alerts") or NULL
// for Everything else. What the "Auto" choice in the settings resolves to.
const char *fdr_mixer_auto_fader(obs_source_t *src);
// The fader a source is in right now, given the configured names (NULL = Everything else).
const char *fdr_mixer_effective_fader(fdr_mixer_t *m, obs_source_t *src);
// Set a source's assignment (fader name, "" for Everything else, NULL for auto) and rebuild.
void fdr_mixer_assign(fdr_mixer_t *m, obs_source_t *src, const char *fader);

// Test hook: JSON with the window's residual/fader levels, callback timing and gate decisions;
// resets the window.
void fdr_mixer_stats_json(fdr_mixer_t *m, struct dstr *out);
