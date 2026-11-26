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
// The debug report: what a streamer pastes into an email to hello@faders.tv when something is
// wrong. Markdown a person can read, with the same facts as JSON underneath. Faders never uploads
// it; nothing here touches the network.
//
// Built from a snapshot the plugin takes (struct fdr_report_input), so this file has no libobs
// and is tested against golden files (test/report-test.c). Fields are an allowlist:
//   - typed fields (numbers, booleans, OBS source kind ids, versions, the fixed hub URL) are
//     copied as they are;
//   - free text (source, fader and channel names, the last failure, log lines, folders) goes
//     through the redactor (redact.h), and with names off, every source, fader and channel name
//     is first replaced by "Source N", "Fader N" and "[channel]", wherever it appears: lists,
//     problems, log lines and the JSON.
// Fail closed: if the plugin key or the stream key is anywhere in the finished text, there is no
// report, only an error.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "logring.h"
#include "redact.h"
#include "sysinfo.h"

struct fdr_report_fader {
	const char *name; // free text
	bool on_wire;     // within the plan's allowance
};

struct fdr_report_source {
	const char *name;       // free text
	const char *kind;       // OBS source id (typed)
	const char *saved;      // "new", "rest", or the fader it was put on (free text)
	const char *plays;      // the fader it plays on, "" = Everything else (free text)
	const char *state;      // "new", "rest", "ok", "locked", "deleted", "dropped" (placement.h), or on the
	                        // split fader "parked" / "extra" (split.h): both play in Everything else
	bool muted;
	const char *monitoring; // "none", "monitor only", "monitor and output"
	uint32_t tracks;        // OBS tracks it is ticked for (bit 0 = Track 1)
	bool on_stream_track;   // ticked for the track OBS streams
};

// The split fader ("Two sources: one per platform", split.h).
struct fdr_report_split {
	const char *fader;          // free text: the fader it's ticked on, "" = none
	bool on;                    // in effect now
	const char *why;            // typed: split.c's fdr_split_why_name ("on", "plan", "no youtube", …)
	const char *twitch_source;  // free text, "" = not picked
	const char *youtube_source; // free text, "" = not picked
	int youtube_track;          // 1-6, 0 = not picked
	const char *warnings;       // typed: the wire's warning codes, ","-joined, "" = none
};

struct fdr_report_input {
	bool names;            // include source, fader and channel names (the default)
	int64_t now;           // UTC seconds since 1970
	int utc_offset_min;    // local time = UTC + this

	const char *plugin_version;
	int64_t plugin_uptime_s;
	const char *update_available; // a newer release the hub told us about, or ""
	const char *config_dir, *module_dir; // free text: folders hold the user name

	const char *obs_version, *obs_locale;
	bool streaming;
	int64_t stream_uptime_s;
	int stream_audio_track; // the track the stream's encoder uses, 0 if not known

	struct fdr_sysinfo sys;
	int sample_rate, channels;
	bool mixer_ok;

	bool enabled, only_live, plugin_key_set;

	const char *channel;   // free text, "" if not known
	const char *plan;      // "free", "pro", "partner" or ""
	const char *platforms; // "twitch", "youtube", "twitch,youtube" or ""
	int listener_limit, max_stems;

	bool running, connected, ever_connected;
	const char *server; // the built-in hub URL, or "custom"
	int listeners, uplink_kbps, stems_on_wire, stream_track;
	bool stream_track_silent;
	unsigned long long chunks_sent;
	const char *last_failure; // free text, "" if none
	int rejected_status;
	const char *mix_mismatch, *mix_duplicate; // source names (free text), "" if none
	struct fdr_report_split split;

	const struct fdr_report_fader *faders;
	int n_faders;
	int allowance; // faders the plan sends; 0 = not known
	const struct fdr_report_source *sources;
	int n_sources;   // every audio input
	int audio_total; // the same, or more if some couldn't be read
	int past_limit;  // audio inputs past what the mixer takes

	const struct fdr_log_line *log;
	int n_log;
	uint64_t log_total;

	fdr_redact_ctx redact;
};

#define FDR_REPORT_MAX_LISTED 200 // sources in the JSON list
#define FDR_REPORT_MAX_NAMED 20   // sources named on the Everything else line

// 0: *out is the report (malloc'd; free() it). Otherwise *out is NULL and nothing may be shown:
// a known secret survived (fail closed).
int fdr_report_build(const struct fdr_report_input *in, char **out);
