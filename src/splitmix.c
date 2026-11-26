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
#include "splitmix.h"
#include "mixer.h"
#include "logring.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <pthread.h>
#include <math.h>
#include <string.h>

#define RATE 48000
#define FRAMES AUDIO_OUTPUT_FRAMES
#define CH 2
#define MAX_STEMS FDR_MAX_STEMS
#define POLL_NS 500000000ULL
// YouTube's oracle, as the mixer's (mixer.c): ~0.5 s windows; the YouTube source's contribution
// anti-correlated with EE@youtube (r < −0.5) for 3 windows in a row means YouTube doesn't air it
// the way it is included: it is dropped (left in EE@youtube) and the streamer is warned.
#define ORACLE_TICKS 24
#define ORACLE_R 0.5
#define ORACLE_RUNS 3
#define ORACLE_MIN_ENERGY 1e-6
// YouTube's track silent (below −80 dBFS) for this long while the layers have sound (above −60).
#define TRACK_SOUND_PEAK 0.0001f
#define LANE_SOUND_PEAK 0.001f
#define SILENT_NS 10000000000ULL
#define LEAK_MIN_ENERGY 1e-3

struct split_stats {
	uint64_t ticks, missing; // ticks finalized; ticks with no YouTube track to go with them
	double n;
	double shared_sq, ftw_sq, fyt_sq, eetw_sq, eeyt_sq;
	double eetw_x_fyt, eeyt_x_ftw; // for the leak gains
	float err_tw, err_yt;
};

struct fdr_splitmix {
	fdr_uplink_t *uplink;
	pthread_mutex_t mu;     // everything below, except the tap fields
	pthread_mutex_t tap_mu; // one refresh at a time; never taken by the audio thread

	// Saved (plugin settings) and the stream track.
	char fader[FDR_STEM_NAME_MAX];
	int youtube_track;
	int stream_track;
	// The wire's fader names, as last announced.
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n_wire;
	// The account (hub hello), as last read.
	char plan[24];
	bool twitch, youtube;
	int max_faders; // the hub's splitMaxFaders (0 = no limit)
	// The split fader's sources (the first of each platform), found by refresh.
	obs_weak_source_t *tw, *yt;
	bool yt_force; // test hook: FDR_FORCE_SETTING on the YouTube source
	bool on;
	enum fdr_split_why why;
	// The announced layout: wire_index = the split fader's index (-1: today's layout).
	int wire_index;
	int layout_n;
	char fader_id[8];

	// Tap (tap_mu): the track tapped for YouTube, 0 = none.
	int tapped;
	int tap_track; // mirror of `tapped` under mu, for the audio thread and stats

	// Audio thread.
	uint64_t yt_ts;
	bool have_yt;
	float yt_trk[CH][FRAMES];
	bool pending; // the mixer's tick came first: kept until YouTube's track for it arrives
	uint64_t pend_ts;
	int pend_n;
	float pend[MAX_STEMS][FRAMES * CH];
	float pend_trk[CH][FRAMES];
	float pend_peak;
	bool pend_fading, pend_hold;
	uint64_t was_ats;
	bool was_active;
	// YouTube's oracle.
	double xy, xx, yy;
	int oracle_ticks, run;
	bool dropped;
	// YouTube's track silence.
	uint64_t on_since_ns, yt_heard_ns, lanes_heard_ns;

	unsigned warnings;
	uint64_t last_poll_ns;
	struct split_stats st;
	// Sources the audio thread got this tick, released once mu is free (as mixer.c does): the
	// last release destroys a source, which takes libobs's source lock, and a rebuild holds that
	// lock while it waits for mu.
	obs_source_t *release[2];
	int n_release;
};

static bool same_name(const char *a, const char *b)
{
	return a && b && astrcmpi(a, b) == 0;
}

static inline bool near(uint64_t a, uint64_t b)
{
	return (a > b ? a - b : b - a) <= 4;
}

fdr_splitmix_t *fdr_splitmix_create(fdr_uplink_t *uplink, int stream_track)
{
	struct fdr_splitmix *s = bzalloc(sizeof(*s));
	s->uplink = uplink;
	pthread_mutex_init(&s->mu, NULL);
	pthread_mutex_init(&s->tap_mu, NULL);
	s->stream_track = stream_track >= 1 && stream_track <= 6 ? stream_track : 1;
	s->wire_index = -1;
	return s;
}

static void on_youtube(void *param, size_t mix_idx, struct audio_data *data);
static void finalize_pending(struct fdr_splitmix *s, bool have_yt);
static int take_release(struct fdr_splitmix *s, obs_source_t *out[2]);
static void release_all(obs_source_t *const *list, int n);

static void tap(struct fdr_splitmix *s, int track)
{
	struct audio_convert_info conv = {.samples_per_sec = RATE, .format = AUDIO_FORMAT_FLOAT_PLANAR, .speakers = SPEAKERS_STEREO, .allow_clipping = true};
	if (s->tapped == track)
		return;
	if (s->tapped)
		obs_remove_raw_audio_callback((size_t)(s->tapped - 1), on_youtube, s);
	if (track)
		obs_add_raw_audio_callback((size_t)(track - 1), &conv, on_youtube, s);
	if (track)
		fdr_log(LOG_INFO, "per-platform fader: YouTube follows Track %d", track);
	else
		fdr_log(LOG_INFO, "per-platform fader: YouTube's track no longer followed");
	s->tapped = track;
	pthread_mutex_lock(&s->mu);
	// A tick waiting for YouTube's track goes out without it: every tick reaches the uplink, or
	// the stems would drift against the chunks' clock.
	if (s->pending && s->wire_index >= 0)
		finalize_pending(s, false);
	s->tap_track = track;
	s->have_yt = false;
	s->pending = false;
	obs_source_t *rel[2];
	int n_rel = take_release(s, rel);
	pthread_mutex_unlock(&s->mu);
	release_all(rel, n_rel);
}

void fdr_splitmix_destroy(fdr_splitmix_t *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->tap_mu);
	tap(s, 0);
	pthread_mutex_unlock(&s->tap_mu);
	obs_weak_source_release(s->tw);
	obs_weak_source_release(s->yt);
	pthread_mutex_destroy(&s->mu);
	pthread_mutex_destroy(&s->tap_mu);
	bfree(s);
}

void fdr_splitmix_set(fdr_splitmix_t *s, const char *fader, int youtube_track)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->mu);
	snprintf(s->fader, sizeof(s->fader), "%s", fader ? fader : "");
	s->youtube_track = youtube_track >= 1 && youtube_track <= 6 ? youtube_track : 0;
	pthread_mutex_unlock(&s->mu);
}

void fdr_splitmix_set_stream_track(fdr_splitmix_t *s, int stream_track)
{
	if (!s || stream_track < 1 || stream_track > 6)
		return;
	pthread_mutex_lock(&s->mu);
	s->stream_track = stream_track;
	pthread_mutex_unlock(&s->mu);
}

static void read_account(struct fdr_splitmix *s, char plan[24], bool *twitch, bool *youtube, int *max_faders)
{
	fdr_uplink_status st;
	fdr_uplink_get_status(s->uplink, &st);
	*max_faders = st.split_max_faders;
	// As the hub's last hello said (kept across a reconnect, which says it again).
	snprintf(plan, 24, "%s", st.plan);
	*twitch = strstr(st.platforms, "twitch") != NULL;
	*youtube = strstr(st.platforms, "youtube") != NULL;
}

// ── Finding the split fader's sources ───────────────────────────────────────────────────

struct find {
	const char *fader;
	obs_source_t *tw, *yt;
};

static bool find_cb(void *param, obs_source_t *src)
{
	struct find *f = param;
	uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE) || obs_source_get_type(src) != OBS_SOURCE_TYPE_INPUT)
		return true;
	obs_data_t *priv = obs_source_get_private_settings(src);
	if (obs_data_has_user_value(priv, FDR_FADER_SETTING) && same_name(obs_data_get_string(priv, FDR_FADER_SETTING), f->fader)) {
		enum fdr_platform p = fdr_platform_parse(obs_data_get_string(priv, FDR_PLATFORM_SETTING));
		if (p == FDR_PLATFORM_TWITCH && !f->tw)
			f->tw = obs_source_get_ref(src);
		else if (p == FDR_PLATFORM_YOUTUBE && !f->yt)
			f->yt = obs_source_get_ref(src);
	}
	obs_data_release(priv);
	return true;
}

bool fdr_splitmix_refresh(fdr_splitmix_t *s)
{
	if (!s)
		return false;
	pthread_mutex_lock(&s->tap_mu);
	char plan[24];
	bool twitch, youtube;
	int max_faders;
	read_account(s, plan, &twitch, &youtube, &max_faders);

	pthread_mutex_lock(&s->mu);
	char fader[FDR_STEM_NAME_MAX];
	snprintf(fader, sizeof(fader), "%s", s->fader);
	pthread_mutex_unlock(&s->mu);

	struct find f = {fader, NULL, NULL};
	if (*fader)
		obs_enum_sources(find_cb, &f);
	bool force = false;
	if (f.yt) {
		obs_data_t *priv = obs_source_get_private_settings(f.yt);
		force = obs_data_get_bool(priv, FDR_FORCE_SETTING);
		obs_data_release(priv);
	}

	pthread_mutex_lock(&s->mu);
	obs_weak_source_t *old_tw = s->tw, *old_yt = s->yt;
	s->tw = f.tw ? obs_source_get_weak_source(f.tw) : NULL;
	s->yt = f.yt ? obs_source_get_weak_source(f.yt) : NULL;
	s->yt_force = force;
	snprintf(s->plan, sizeof(s->plan), "%s", plan);
	s->twitch = twitch;
	s->youtube = youtube;
	s->max_faders = max_faders;
	const char *names[FDR_MAX_FADERS];
	for (int i = 0; i < s->n_wire; i++)
		names[i] = s->names[i];
	struct fdr_fader_list list = {names, s->n_wire, s->n_wire};
	struct fdr_split_input in = {&list, s->fader, s->youtube_track, {s->plan, twitch, youtube}, f.tw != NULL, f.yt != NULL, max_faders};
	struct fdr_split_state state;
	fdr_split_evaluate(&in, &state);
	if (state.on && !s->on) {
		// Turning on starts YouTube's oracle over (a source dropped before is given another go).
		s->dropped = false;
		s->run = 0;
		s->xy = s->xx = s->yy = 0;
		s->oracle_ticks = 0;
		s->on_since_ns = os_gettime_ns();
		s->was_active = false;
	}
	if (state.why != s->why || state.on != s->on)
		fdr_log(LOG_INFO, "per-platform fader \"%s\": %s", s->fader, state.on ? "on" : fdr_split_why_name(state.why));
	s->on = state.on;
	s->why = state.why;
	bool relayout = s->on != (s->wire_index >= 0);
	int want_tap = s->on ? s->youtube_track : 0;
	pthread_mutex_unlock(&s->mu);

	obs_weak_source_release(old_tw);
	obs_weak_source_release(old_yt);
	obs_source_release(f.tw);
	obs_source_release(f.yt);
	// Never under mu: libobs calls the tap with its own audio lock held, and the tap takes mu.
	tap(s, want_tap);
	pthread_mutex_unlock(&s->tap_mu);
	return relayout;
}

bool fdr_splitmix_parks(fdr_splitmix_t *s, obs_source_t *src)
{
	if (!s || !src)
		return false;
	obs_data_t *priv = obs_source_get_private_settings(src);
	char saved[FDR_STEM_NAME_MAX * 2];
	snprintf(saved, sizeof(saved), "%s", obs_data_get_string(priv, FDR_FADER_SETTING));
	enum fdr_platform p = fdr_platform_parse(obs_data_get_string(priv, FDR_PLATFORM_SETTING));
	obs_data_release(priv);
	pthread_mutex_lock(&s->mu);
	bool chosen = (p == FDR_PLATFORM_TWITCH && s->tw && obs_weak_source_references_source(s->tw, src)) ||
		      (p == FDR_PLATFORM_YOUTUBE && s->yt && obs_weak_source_references_source(s->yt, src));
	enum fdr_split_role role = fdr_split_role(NULL, s->fader, saved, p, chosen);
	pthread_mutex_unlock(&s->mu);
	// Only the chosen Twitch source plays on the fader in the mixer's lanes.
	return role != FDR_SPLIT_ROLE_NONE && role != FDR_SPLIT_ROLE_TWITCH;
}

bool fdr_splitmix_announce(fdr_splitmix_t *s, const char *const *names, int n_wire)
{
	if (!s)
		return false;
	pthread_mutex_lock(&s->mu);
	// A tick waiting for YouTube's track goes out in the layout it was made in, before this one.
	if (s->pending && s->wire_index >= 0)
		finalize_pending(s, false);
	obs_source_t *rel[2];
	int n_rel = take_release(s, rel);
	if (n_wire > FDR_MAX_FADERS)
		n_wire = FDR_MAX_FADERS;
	for (int i = 0; i < n_wire; i++)
		snprintf(s->names[i], sizeof(s->names[i]), "%s", names[i]);
	s->n_wire = n_wire;
	int split = -1;
	for (int i = 0; s->on && *s->fader && i < n_wire && split < 0; i++)
		if (same_name(s->names[i], s->fader))
			split = i;
	if (split >= 0 && fdr_split_wire_stems(n_wire, true) > MAX_STEMS)
		split = -1;
	s->wire_index = split;
	s->pending = false;
	if (split < 0) {
		s->layout_n = n_wire + 1;
		pthread_mutex_unlock(&s->mu);
		release_all(rel, n_rel);
		fdr_uplink_set_split_status(s->uplink, 0, 0);
		return false;
	}
	const char *ptrs[FDR_MAX_FADERS];
	for (int i = 0; i < n_wire; i++)
		ptrs[i] = s->names[i];
	struct fdr_stem_desc stems[MAX_STEMS];
	s->layout_n = fdr_split_layout(ptrs, n_wire, split, stems, s->fader_id, sizeof(s->fader_id));
	fdr_uplink_set_layout(s->uplink, stems, s->layout_n);
	s->last_poll_ns = 0; // the status follows at once
	pthread_mutex_unlock(&s->mu);
	release_all(rel, n_rel);
	return true;
}

// ── Status: warnings ────────────────────────────────────────────────────────────────────

// Not locked (the release can destroy the source: see `release` above).
static uint32_t mixers_of(obs_weak_source_t *w)
{
	obs_source_t *src = w ? obs_weak_source_get_source(w) : NULL;
	uint32_t m = src ? obs_source_get_audio_mixers(src) : 0;
	obs_source_release(src);
	obs_weak_source_release(w);
	return m;
}

bool fdr_splitmix_poll(fdr_splitmix_t *s)
{
	if (!s)
		return false;
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&s->mu);
	bool due = !s->last_poll_ns || now - s->last_poll_ns >= POLL_NS;
	if (due)
		s->last_poll_ns = now;
	pthread_mutex_unlock(&s->mu);
	if (!due)
		return false;
	char plan[24];
	bool twitch, youtube;
	int max_faders;
	read_account(s, plan, &twitch, &youtube, &max_faders);
	pthread_mutex_lock(&s->mu);
	obs_weak_source_t *tw = s->tw, *yt = s->yt;
	obs_weak_source_addref(tw);
	obs_weak_source_addref(yt);
	pthread_mutex_unlock(&s->mu);
	uint32_t tw_mixers = mixers_of(tw), yt_mixers = mixers_of(yt);
	pthread_mutex_lock(&s->mu);
	bool changed = strcmp(plan, s->plan) != 0 || twitch != s->twitch || youtube != s->youtube || max_faders != s->max_faders;
	int track = s->wire_index >= 0 ? s->youtube_track : 0;
	unsigned w = 0;
	if (track) {
		w = fdr_split_track_warnings(tw_mixers, yt_mixers, s->stream_track, track);
		if (s->dropped)
			w |= FDR_SPLIT_WARN_YOUTUBE_NOT_ON_TRACK;
		bool silent = now - s->on_since_ns >= SILENT_NS && (!s->yt_heard_ns || now - s->yt_heard_ns >= SILENT_NS) && s->lanes_heard_ns && now - s->lanes_heard_ns < SILENT_NS;
		if (silent)
			w |= FDR_SPLIT_WARN_YOUTUBE_TRACK_SILENT;
	}
	unsigned was = s->warnings;
	s->warnings = w;
	pthread_mutex_unlock(&s->mu);
	if (w != was) {
		for (int b = 0; b < FDR_SPLIT_WARN_COUNT; b++) {
			unsigned bit = 1u << b;
			if ((w & bit) && !(was & bit))
				fdr_log(LOG_WARNING, "per-platform fader: %s", fdr_split_warning_code(bit));
		}
	}
	fdr_uplink_set_split_status(s->uplink, track, w);
	return changed;
}

// ── Audio thread ────────────────────────────────────────────────────────────────────────

static float max_abs_err(float err, float a, float b)
{
	float d = fabsf(a - b);
	return d > err ? d : err;
}

// Locked. One tick of the split's layout: F@youtube from the YouTube source, EE@youtube from
// YouTube's track, then the whole tick to the uplink. stems: the mixer's (faders, EE@twitch).
static void finalize(struct fdr_splitmix *s, uint64_t ts, const float *const *stems, int n, const float *trk_l, const float *trk_r, float track_peak, bool fading, bool hold, bool have_yt)
{
	static float fyt[CH][FRAMES], shared[CH][FRAMES], ee[CH][FRAMES];
	static float fyt_i[FRAMES * CH], ee_i[FRAMES * CH];
	static const float *outp[MAX_STEMS];
	const int F = s->wire_index, nw = n - 1;
	const uint64_t ts_end = ts + util_mul_div64(FRAMES, 1000000000ULL, RATE);
	const size_t yt_idx = (size_t)(s->tap_track - 1);

	// F@youtube: the YouTube source, gated as the mixer gates a source (mixer.c): what libobs mixed
	// into YouTube's track this tick, from frame 0. During a modelled transition it is left to
	// EE@youtube (the sum stays exact; the fader just doesn't hold it for those ticks).
	memset(fyt, 0, sizeof(fyt));
	bool inc = false;
	obs_source_t *src = s->yt && s->tap_track ? obs_weak_source_get_source(s->yt) : NULL;
	if (src) {
		uint64_t ats = obs_source_get_audio_timestamp(src);
		bool active = obs_source_active(src);
		uint64_t was = s->was_ats;
		bool was_active = s->was_active;
		s->was_ats = ats;
		s->was_active = active;
		if (s->yt_force)
			inc = true;
		else
			inc = active && (obs_source_get_audio_mixers(src) & (1u << yt_idx)) && obs_source_get_monitoring_type(src) != OBS_MONITORING_TYPE_MONITOR_ONLY &&
			      !obs_source_audio_pending(src) && near(ats, ts_end) && near(was, ts) && was_active && !fading;
		inc = inc && !s->dropped && have_yt;
		if (inc) {
			struct obs_source_audio_mix mix;
			obs_source_get_audio_mix(src, &mix);
			if (mix.output[yt_idx].data[0] && mix.output[yt_idx].data[1]) {
				memcpy(fyt[0], mix.output[yt_idx].data[0], sizeof(fyt[0]));
				memcpy(fyt[1], mix.output[yt_idx].data[1], sizeof(fyt[1]));
			} else {
				inc = false;
			}
		}
		if (s->n_release < 2)
			s->release[s->n_release++] = src;
		else
			obs_source_release(src); // never: a call finalizes at most two ticks
	}

	// The shared faders (every fader but the split one), once, from the mixer's stems.
	memset(shared, 0, sizeof(shared));
	for (int f = 0; f < nw; f++) {
		if (f == F)
			continue;
		const float *x = stems[f];
		for (size_t i = 0; i < FRAMES; i++) {
			shared[0][i] += x[2 * i];
			shared[1][i] += x[2 * i + 1];
		}
	}
	// EE@youtube = YouTube's track − shared − F@youtube (nothing without YouTube's track this tick).
	float yt_peak = 0;
	for (int c = 0; c < CH; c++)
		for (size_t i = 0; i < FRAMES; i++) {
			ee[c][i] = have_yt ? s->yt_trk[c][i] - (shared[c][i] + fyt[c][i]) : 0.0f;
			float v = fabsf(s->yt_trk[c][i]);
			yt_peak = have_yt && v > yt_peak ? v : yt_peak;
		}
	for (size_t i = 0; i < FRAMES; i++) {
		fyt_i[2 * i] = fyt[0][i];
		fyt_i[2 * i + 1] = fyt[1][i];
		ee_i[2 * i] = ee[0][i];
		ee_i[2 * i + 1] = ee[1][i];
	}

	// The layout (split.c): faders with F@youtube right after the split fader, EE@twitch, EE@youtube.
	int k = 0;
	for (int f = 0; f < nw; f++) {
		outp[k++] = stems[f];
		if (f == F)
			outp[k++] = fyt_i;
	}
	outp[k++] = stems[nw];
	outp[k++] = ee_i;

	// YouTube's oracle: EE@youtube against the YouTube source's contribution.
	if (hold) {
		s->xy = s->xx = s->yy = 0;
		s->oracle_ticks = 0;
	} else if (have_yt) {
		double xy = 0, xx = 0, yy = 0;
		for (int c = 0; c < CH; c++)
			for (size_t i = 0; i < FRAMES; i++) {
				xy += (double)ee[c][i] * fyt[c][i];
				xx += (double)fyt[c][i] * fyt[c][i];
				yy += (double)ee[c][i] * ee[c][i];
			}
		s->xy += xy;
		s->xx += xx;
		s->yy += yy;
		if (++s->oracle_ticks >= ORACLE_TICKS) {
			if (s->xx > ORACLE_MIN_ENERGY && s->yy > ORACLE_MIN_ENERGY) {
				double r = s->xy / sqrt(s->xx * s->yy);
				s->run = r < -ORACLE_R ? s->run + 1 : 0;
				if (s->run >= ORACLE_RUNS && !s->dropped) {
					s->dropped = true;
					fdr_log(LOG_WARNING, "the YouTube source of the per-platform fader is not in YouTube's track (Track %d) the way it is sent: left in YouTube's Everything else for this session", s->tap_track);
				}
			} else {
				s->run = 0;
			}
			s->xy = s->xx = s->yy = 0;
			s->oracle_ticks = 0;
		}
	}

	// Silence, statistics, and each platform's oracle error.
	uint64_t now = os_gettime_ns();
	if (yt_peak > TRACK_SOUND_PEAK)
		s->yt_heard_ns = now;
	float lane_peak = 0;
	for (int c = 0; c < CH; c++)
		for (size_t i = 0; i < FRAMES; i++) {
			float v = fabsf(shared[c][i] + fyt[c][i]);
			lane_peak = v > lane_peak ? v : lane_peak;
		}
	if (lane_peak > LANE_SOUND_PEAK)
		s->lanes_heard_ns = now;
	struct split_stats *st = &s->st;
	st->ticks++;
	st->missing += !have_yt;
	st->n += FRAMES * CH;
	const float *ftw = stems[F], *eetw = stems[nw];
	for (size_t i = 0; i < FRAMES; i++)
		for (int c = 0; c < CH; c++) {
			float sh = shared[c][i], ft = ftw[2 * i + c], fy = fyt[c][i], et = eetw[2 * i + c], ey = ee[c][i];
			st->shared_sq += (double)sh * sh;
			st->ftw_sq += (double)ft * ft;
			st->fyt_sq += (double)fy * fy;
			st->eetw_sq += (double)et * et;
			st->eeyt_sq += (double)ey * ey;
			st->eetw_x_fyt += (double)et * fy;
			st->eeyt_x_ftw += (double)ey * ft;
			float tw_sum = sh + ft + et;
			st->err_tw = max_abs_err(st->err_tw, c ? trk_r[i] : trk_l[i], tw_sum);
			if (have_yt)
				st->err_yt = max_abs_err(st->err_yt, s->yt_trk[c][i], sh + fy + ey);
		}
	fdr_uplink_push_tick(s->uplink, ts, outp, k, FRAMES, track_peak);
}

static void finalize_pending(struct fdr_splitmix *s, bool have_yt)
{
	const float *p[MAX_STEMS];
	for (int i = 0; i < s->pend_n; i++)
		p[i] = s->pend[i];
	s->pending = false;
	finalize(s, s->pend_ts, p, s->pend_n, s->pend_trk[0], s->pend_trk[1], s->pend_peak, s->pend_fading, s->pend_hold, have_yt);
}

// Locked: take the sources finalize kept, to release once unlocked.
static int take_release(struct fdr_splitmix *s, obs_source_t *out[2])
{
	int n = s->n_release;
	for (int i = 0; i < n; i++)
		out[i] = s->release[i];
	s->n_release = 0;
	return n;
}
static void release_all(obs_source_t *const *list, int n)
{
	for (int i = 0; i < n; i++)
		obs_source_release(list[i]);
}

static void on_youtube(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct fdr_splitmix *s = param;
	pthread_mutex_lock(&s->mu);
	memcpy(s->yt_trk[0], data->data[0], sizeof(s->yt_trk[0]));
	memcpy(s->yt_trk[1], data->data[1], sizeof(s->yt_trk[1]));
	s->yt_ts = data->timestamp;
	s->have_yt = true;
	// The mixer's tap is on a lower track (libobs runs the tracks in order): its tick is waiting.
	if (s->pending && s->pend_ts == s->yt_ts && s->wire_index >= 0)
		finalize_pending(s, true);
	obs_source_t *rel[2];
	int n_rel = take_release(s, rel);
	pthread_mutex_unlock(&s->mu);
	release_all(rel, n_rel);
}

bool fdr_splitmix_tick(fdr_splitmix_t *s, uint64_t ts, const float *const *stems, int n, const float *trk_l, const float *trk_r, float track_peak, bool fading, bool hold)
{
	if (!s)
		return false;
	pthread_mutex_lock(&s->mu);
	if (s->wire_index < 0 || n + FDR_SPLIT_EXTRA != s->layout_n || s->wire_index >= n - 1) {
		pthread_mutex_unlock(&s->mu);
		// No split, or this tick isn't in its layout (the layout is changing): the mixer pushes it
		// as it is. Every tick reaches the uplink, so the stems keep their time.
		return false;
	}
	if (!s->tap_track) {
		// YouTube's tap is being changed: this tick goes out without YouTube's track.
		if (s->pending)
			finalize_pending(s, false);
		finalize(s, ts, stems, n, trk_l, trk_r, track_peak, fading, hold, false);
		obs_source_t *rel[2];
		int n_rel = take_release(s, rel);
		pthread_mutex_unlock(&s->mu);
		release_all(rel, n_rel);
		return true;
	}
	// A tick still waiting means YouTube's track never came for it: send it without.
	if (s->pending)
		finalize_pending(s, false);
	if (s->have_yt && s->yt_ts == ts) {
		finalize(s, ts, stems, n, trk_l, trk_r, track_peak, fading, hold, true);
	} else {
		for (int i = 0; i < n; i++)
			memcpy(s->pend[i], stems[i], sizeof(s->pend[i]));
		memcpy(s->pend_trk[0], trk_l, sizeof(s->pend_trk[0]));
		memcpy(s->pend_trk[1], trk_r, sizeof(s->pend_trk[1]));
		s->pend_n = n;
		s->pend_ts = ts;
		s->pend_peak = track_peak;
		s->pend_fading = fading;
		s->pend_hold = hold;
		s->pending = true;
	}
	obs_source_t *rel[2];
	int n_rel = take_release(s, rel);
	pthread_mutex_unlock(&s->mu);
	release_all(rel, n_rel);
	return true;
}

// ── Test hook ───────────────────────────────────────────────────────────────────────────

static double rms(double sq, double n)
{
	return n > 0 ? sqrt(sq / n) : 0.0;
}

void fdr_splitmix_stats_json(fdr_splitmix_t *s, struct dstr *out)
{
	if (!s) {
		dstr_copy(out, "{\"on\":false,\"taps\":0,\"stems\":0}");
		return;
	}
	fdr_uplink_status us;
	fdr_uplink_get_status(s->uplink, &us);
	pthread_mutex_lock(&s->mu);
	struct split_stats st = s->st;
	memset(&s->st, 0, sizeof(s->st));
	dstr_printf(out, "{\"on\":%s,\"why\":\"%s\",\"fader\":\"%s\",\"youtube_track\":%d,\"taps\":%d,\"stems\":%d,\"ticks\":%llu,\"missing_youtube_ticks\":%llu,", s->on ? "true" : "false",
		    fdr_split_why_name(s->why), s->fader, s->youtube_track, 1 + (s->tap_track ? 1 : 0), us.stems, (unsigned long long)st.ticks, (unsigned long long)st.missing);
	dstr_catf(out, "\"shared_rms\":%.6f,\"f_twitch_rms\":%.6f,\"f_youtube_rms\":%.6f,\"ee_twitch_rms\":%.6f,\"ee_youtube_rms\":%.6f,", rms(st.shared_sq, st.n), rms(st.ftw_sq, st.n), rms(st.fyt_sq, st.n),
		  rms(st.eetw_sq, st.n), rms(st.eeyt_sq, st.n));
	dstr_catf(out, "\"leak_twitch_ee_youtube_music\":%.6f,\"leak_youtube_ee_twitch_music\":%.6f,", st.fyt_sq > LEAK_MIN_ENERGY ? fabs(st.eetw_x_fyt / st.fyt_sq) : 0.0,
		  st.ftw_sq > LEAK_MIN_ENERGY ? fabs(st.eeyt_x_ftw / st.ftw_sq) : 0.0);
	dstr_catf(out, "\"oracle_err_twitch\":%.3e,\"oracle_err_youtube\":%.3e,\"youtube_dropped\":%s,\"warnings\":[", st.err_tw, st.err_yt, s->dropped ? "true" : "false");
	bool first = true;
	for (int b = 0; b < FDR_SPLIT_WARN_COUNT; b++)
		if (s->warnings & (1u << b)) {
			dstr_catf(out, "%s\"%s\"", first ? "" : ",", fdr_split_warning_code(1u << b));
			first = false;
		}
	dstr_cat(out, "]}");
	pthread_mutex_unlock(&s->mu);
}
