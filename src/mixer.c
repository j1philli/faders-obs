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
#include "mixer.h"
#include "placement.h"
#include "splitmix.h"
#include "logring.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define FRAMES AUDIO_OUTPUT_FRAMES // 1024 per tick
#define CH 2
#define MAX_ENTRIES FDR_MAX_AUDIO_SOURCES
#define REST FDR_MAX_FADERS // fader index meaning "Everything else"
// Oracle: per ~0.5 s window (24 ticks), the residual correlated with a source's contribution.
// |r| > 0.5 for 3 windows in a row means the source isn't in the stream the way we include it:
// drop it (its audio then flows through Everything else, exactly as the stream has it).
#define ORACLE_TICKS 24
#define ORACLE_R 0.5
#define ORACLE_RUNS 3
#define ORACLE_CALM 6 // windows without it before a duplicate warning is withdrawn
#define ORACLE_SETTLE_NS 1500000000ULL // after a transition ends, before evidence counts again
#define ORACLE_MIN_ENERGY 1e-6 // Σ x² over a window below this is no evidence
#define MAX_CB 2048
#define MAX_TREE 128

struct entry {
	obs_weak_source_t *weak;
	char name[64];
	int fader; // 0..n_faders-1 (folded ones already mapped to REST) or REST
	bool force;
	// Oracle sums for the current window (audio thread).
	double xy, xx;
	int run;
	int calm; // windows in a row without the correlation, once warned about as a duplicate
	bool dropped;
};

struct snapshot {
	int n;
	bool overflow; // more audio sources than entries
	struct entry e[MAX_ENTRIES];
};

// Transition in flight: libobs's own formula (t = (sample_ts − start) / duration) per sample,
// A × (1 − t) + B × t for sources only on one side.
struct fade {
	bool active;
	// transition_stop arrived: libobs no longer mixes A at all, but A's sources can stay
	// "active" for a tick or two until the video thread deactivates them. Keep A silent (and B
	// at full gain) until every A-only source is inactive, then forget the fade.
	bool stopped;
	uint64_t stopped_ns;
	bool learning; // measure the real duration from obs_transition_get_time() reaching 1
	uint64_t start_ns, dur_ns;
	obs_source_t *a[MAX_TREE], *b[MAX_TREE]; // raw pointers for membership only
	int na, nb;
};

struct ats_slot {
	obs_source_t *src;
	uint64_t ats;
	bool active; // obs_source_active() at the last tick
};

struct stats {
	uint64_t ticks, n;
	double rest_sq, fader_sq[FDR_MAX_FADERS];
	uint64_t cb_ns[MAX_CB];
	int n_cb;
	int included[MAX_ENTRIES], gated_new[MAX_ENTRIES];
};

struct fdr_mixer {
	fdr_uplink_t *uplink;
	fdr_splitmix_t *split; // the split fader's YouTube side (splitmix.h); idle without a split
	int stream_track; // 1-6
	size_t mix_idx;
	uint32_t stream_bit;
	bool tapping;

	pthread_mutex_t mu;   // guards faders, snapshot pointer swap, fade, dropped, stats
	char names[FDR_MAX_FADERS][FDR_STEM_NAME_MAX];
	int n_faders;
	int max_stems; // plan allowance; faders past it fold into REST
	int n_wire;    // faders actually sent (≤ n_faders)
	struct snapshot *snap;
	struct fade fade;
	obs_weak_source_t *dropped[MAX_ENTRIES];
	int n_dropped;
	char mismatch[64];
	char duplicate[64]; // a source whose audio is in the stream more than its fader carries; warned, not moved
	struct ats_slot ats[MAX_ENTRIES * 2];
	struct stats st;

	obs_weak_source_t *tr; // transition whose signals we're connected to
	pthread_mutex_t tr_mu; // one track_transition at a time; never taken by a signal handler
	bool overflow_warned;
	bool signals;
	// Core signals fire while libobs holds its own locks, so they only flag; this thread does
	// the rebuild (which enumerates sources) a moment later.
	pthread_t thread;
	os_event_t *stop_ev;
	volatile bool dirty;
	uint64_t given_dur_ns;   // from the frontend's transition duration setting (0 = none)
	uint64_t dbg_until;      // FADERS_DEBUG: log every tick until this time
	uint64_t learned_dur_ns; // measured from the last fade: when get_time() reached 1
	// The oracle takes no evidence while the program transition runs, nor until this time after
	// it: transitions are modelled (a linear crossfade of the frontend's duration) or left to
	// Everything else, and where the real one differs (Fade to Color, Slide, Swipe, a per-scene
	// duration, the T-bar) the difference is the sources' own audio. That is no mismatch.
	bool in_transition;
	uint64_t oracle_hold_until_ns;
};

static void announce(struct fdr_mixer *m);
static void split_refresh(struct fdr_mixer *m);

static inline bool near(uint64_t a, uint64_t b)
{
	return (a > b ? a - b : b - a) <= 4;
}

// ── Audio thread ────────────────────────────────────────────────────────────────────────

// The per-tick loops, written so the compiler turns them into SIMD. Each works on whole buffers
// that never overlap (restrict), one buffer at a time. Sums are the exception: a compiler may not
// reorder a float sum, so a plain loop adds one sample at a time, each add waiting for the one
// before. The sums below only feed the oracle and the level statistics (nothing that reaches a
// stem), so they run in 16 independent lanes, written with the compilers' vector types (clang
// and GCC, NEON and SSE alike).

// dst += src, per sample: exactly what the plain loop did.
static inline void add_into(float *restrict dst, const float *restrict src)
{
	for (size_t i = 0; i < FRAMES; i++)
		dst[i] += src[i];
}

#if defined(__GNUC__) && (FRAMES % 16 == 0)
typedef float v4f __attribute__((vector_size(16), aligned(4)));
static inline v4f v4f_at(const float *p)
{
	v4f v;
	memcpy(&v, p, sizeof(v));
	return v;
}
static inline float v4f_sum4(v4f a, v4f b, v4f c, v4f d)
{
	v4f t = (a + b) + (c + d);
	return (t[0] + t[1]) + (t[2] + t[3]);
}

// Σ (a² + b²): a tick's energy for the level statistics and the oracle's residual.
static inline float energy(const float *a, const float *b)
{
	v4f s[4] = {{0}, {0}, {0}, {0}};
	for (size_t i = 0; i < FRAMES; i += 16)
		for (int u = 0; u < 4; u++) {
			v4f x = v4f_at(a + i + 4 * u), y = v4f_at(b + i + 4 * u);
			s[u] += x * x + y * y;
		}
	return v4f_sum4(s[0], s[1], s[2], s[3]);
}

// The oracle's sums for one source: Σ rest·contribution and Σ contribution², both channels.
static inline void correlate(const float *rl, const float *rr, const float *cl, const float *cr, float *xy_out, float *xx_out)
{
	v4f xy[4] = {{0}, {0}, {0}, {0}}, xx[4] = {{0}, {0}, {0}, {0}};
	for (size_t i = 0; i < FRAMES; i += 16)
		for (int u = 0; u < 4; u++) {
			v4f a = v4f_at(rl + i + 4 * u), b = v4f_at(rr + i + 4 * u), c = v4f_at(cl + i + 4 * u), d = v4f_at(cr + i + 4 * u);
			xy[u] += a * c + b * d;
			xx[u] += c * c + d * d;
		}
	*xy_out = v4f_sum4(xy[0], xy[1], xy[2], xy[3]);
	*xx_out = v4f_sum4(xx[0], xx[1], xx[2], xx[3]);
}
#else
static inline float energy(const float *a, const float *b)
{
	float s = 0;
	for (size_t i = 0; i < FRAMES; i++)
		s += a[i] * a[i] + b[i] * b[i];
	return s;
}
static inline void correlate(const float *rl, const float *rr, const float *cl, const float *cr, float *xy_out, float *xx_out)
{
	float xy = 0, xx = 0;
	for (size_t i = 0; i < FRAMES; i++) {
		xy += rl[i] * cl[i] + rr[i] * cr[i];
		xx += cl[i] * cl[i] + cr[i] * cr[i];
	}
	*xy_out = xy;
	*xx_out = xx;
}
#endif

// The largest |sample|. A running maximum is a chain like a sum is, so: 16 of them, side by side
// (the result is the same whatever the order).
static inline float peak_of(const float *restrict a, float peak)
{
	float lane[16] = {0};
	for (size_t i = 0; i + 16 <= FRAMES; i += 16)
		for (size_t j = 0; j < 16; j++) {
			float v = fabsf(a[i + j]);
			lane[j] = v > lane[j] ? v : lane[j];
		}
	for (size_t i = FRAMES - FRAMES % 16; i < FRAMES; i++) {
		float v = fabsf(a[i]);
		peak = v > peak ? v : peak;
	}
	for (size_t j = 0; j < 16; j++)
		peak = lane[j] > peak ? lane[j] : peak;
	return peak;
}

static struct ats_slot *ats_for(struct fdr_mixer *m, obs_source_t *src)
{
	struct ats_slot *free_slot = NULL;
	for (size_t i = 0; i < sizeof(m->ats) / sizeof(*m->ats); i++) {
		if (m->ats[i].src == src)
			return &m->ats[i];
		if (!m->ats[i].src && !free_slot)
			free_slot = &m->ats[i];
	}
	if (!free_slot)
		free_slot = &m->ats[0];
	free_slot->src = src;
	free_slot->ats = 0;
	free_slot->active = false;
	return free_slot;
}

static inline bool in_set(obs_source_t *const *set, int n, obs_source_t *s)
{
	for (int i = 0; i < n; i++)
		if (set[i] == s)
			return true;
	return false;
}

static void on_track(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct fdr_mixer *m = param;
	const uint64_t ts = data->timestamp;
	const uint64_t ts_end = ts + util_mul_div64(FRAMES, 1000000000ULL, RATE);
	// Per fader (plus the residual) planar accumulators, per-entry contributions for the oracle,
	// and the interleaved stems handed to the uplink. Static: no allocation on the audio thread.
	static float acc[FDR_MAX_FADERS + 1][CH][FRAMES];
	static float faded[MAX_ENTRIES][CH][FRAMES]; // contributions with a transition gain applied
	static const float *contrib[MAX_ENTRIES][CH]; // what each included entry added (mix buffer or faded)
	static bool inc[MAX_ENTRIES];
	static float out[FDR_MAX_STEMS][FRAMES * CH];
	static const float *outp[FDR_MAX_STEMS];
	static struct obs_source_audio_mix mix;
	static double yy;
	static int oracle_ticks;
	static char why[MAX_ENTRIES]; // debug: gate reason per entry this tick
	memset(why, '.', sizeof(why));

	pthread_mutex_lock(&m->mu);
	uint64_t t_in = os_gettime_ns();
	struct snapshot *snap = m->snap;
	int n_wire = m->n_wire;
	// The gains apply from the moment the transition starts (B contributes 0 before its start
	// time, like libobs) and stay applied (clamped at t = 1: A silent, B full) until the
	// transition stops and A's sources have gone inactive.
	bool fading = m->fade.active;
	if (m->fade.active && m->fade.stopped && ts > m->fade.stopped_ns + 2000000000ULL)
		m->fade.active = false; // safety: A never went inactive (e.g. it's also in B's tree)
	if (m->fade.active && ts > m->fade.start_ns + m->fade.dur_ns + 5000000000ULL)
		m->fade.active = false; // safety: a stop signal we never saw
	if (m->fade.learning) {
		// The video clock says when the real transition ended: remember its length for next time.
		obs_source_t *t = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
		if (t) {
			if (obs_transition_get_time(t) >= 1.0f) {
				uint64_t now = os_gettime_ns();
				uint64_t len = now > m->fade.start_ns ? now - m->fade.start_ns : 0;
				if (len >= 50000000ULL && len <= 10000000000ULL)
					m->learned_dur_ns = len;
				m->fade.learning = false;
			}
			obs_source_release(t);
		} else {
			m->fade.learning = false;
		}
	}
	memset(acc, 0, sizeof(float) * (size_t)(n_wire + 1) * CH * FRAMES);

	// The end of a fade is ambiguous: once the audio side has finished transitioning, libobs
	// copies whichever scene sits in slot A, which is the old scene until the video side (at
	// frame rate) ends the transition and swaps them, and the swap can land between the
	// transition's audio render and this callback. So for ticks past the end, and until A's
	// sources have gone inactive (including the tick they go inactive on), the two candidates
	// (old scene in full, new scene in full) are held back and the one that matches the
	// transition source's own output this tick is used.
	bool ambiguous = fading && (m->fade.stopped || ts >= m->fade.start_ns + m->fade.dur_ns);
	static float cand[2][CH][FRAMES];
	static int8_t side[MAX_ENTRIES]; // 0 = A-only candidate, 1 = B-only, -1 = decided already
	static struct obs_source_audio_mix tmix;
	const float *tout_l = NULL, *tout_r = NULL;
	// Mix buffers belong to their sources, and several are read again further down (the
	// candidates, the oracle). A source removed meanwhile would free them, so
	// every source whose buffer is kept stays referenced until the tick is done.
	static obs_source_t *held[MAX_ENTRIES + 1];
	int n_held = 0;
	bool tout_read = false; // the transition's buffer, read (and held) at most once per tick
	if (ambiguous) {
		obs_source_t *t = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
		tout_read = true;
		if (t) {
			obs_source_get_audio_mix(t, &tmix);
			tout_l = tmix.output[m->mix_idx].data[0];
			tout_r = tmix.output[m->mix_idx].data[1];
			held[n_held++] = t;
		}
		if (!tout_l)
			ambiguous = false; // no transition buffer to compare with: fall back to the signals
		memset(cand, 0, sizeof(cand));
	}
	int n = snap ? snap->n : 0;
	bool a_alive = false; // after a stop: is any A-only source still active?
	static bool one_side[MAX_ENTRIES]; // in exactly one of the fade's trees (mixed by the transition)
	static bool arrived[MAX_ENTRIES];  // the incoming side's first tick, held back (see below)
	int n_arrived = 0;
	for (int k = 0; k < n; k++) {
		struct entry *e = &snap->e[k];
		inc[k] = false;
		one_side[k] = arrived[k] = false;
		// Every path that skips this entry must leave no stale "held back" mark or pointers from
		// an earlier tick (the entries behind an index change when sources come and go).
		side[k] = -1;
		contrib[k][0] = contrib[k][1] = NULL;
		obs_source_t *s = obs_weak_source_get_source(e->weak);
		if (!s)
			continue;
		struct ats_slot *slot = ats_for(m, s);
		uint64_t ats = obs_source_get_audio_timestamp(s);
		uint64_t was = slot->ats;
		bool active = obs_source_active(s);
		bool was_active = slot->active;
		slot->ats = ats;
		slot->active = active;
		bool in_a = fading && in_set(m->fade.a, m->fade.na, s), in_b = fading && in_set(m->fade.b, m->fade.nb, s);
		one_side[k] = in_a != in_b;
		if (in_a && !in_b && active)
			a_alive = true;
		// Ending a transition moves B from its slot to A's, which deactivates and reactivates
		// every B source within the same call; a tick can catch it inactive while libobs mixes
		// it in full. B-only sources that were active stay active across the stop window.
		if (m->fade.stopped && in_b && !in_a && was_active && !active) {
			active = true;
			slot->active = true;
		}
		// The same swap takes A out of the program, and it can land after libobs mixed this tick
		// with the old scene in full (its audio side had ended, the video side hadn't) and before
		// this callback: A's sources then read inactive for a tick libobs played. So in the
		// ambiguous window an A-only source that was active last tick is still a candidate this
		// tick; the transition's output decides, as for every candidate. Only for this tick:
		// slot->active keeps the real state, so the next tick gates it as inactive.
		if (ambiguous && in_a && !in_b && was_active && !active)
			active = true;
		if (e->fader == REST) { // Everything else: whatever the stream has of it is in the residual
			why[k] = 'R';
			obs_source_release(s);
			continue;
		}
		bool arriving = false;
		if (!e->force) {
			// The gate: what libobs mixed into the streamed track this tick, from frame 0.
			if (!active || !(obs_source_get_audio_mixers(s) & m->stream_bit) ||
			    obs_source_get_monitoring_type(s) == OBS_MONITORING_TYPE_MONITOR_ONLY || obs_source_audio_pending(s) || !near(ats, ts_end)) {
				why[k] = !active ? 'i' : obs_source_audio_pending(s) ? 'p' : !near(ats, ts_end) ? 't' : 'g';
				obs_source_release(s);
				continue;
			}
			// The incoming side of a modelled fade, on its first active tick. libobs activates B
			// just before it signals transition_start, and every audio source is rendered every
			// tick, active or not, so B's buffer and timestamps are this tick's. Whether libobs
			// mixed B into this tick (at B × t) depends on where the start fell, and a scene that
			// has never been in the program can be left out of its first tick. So B's modelled
			// contribution is held back and the transition's own output decides (after the loop).
			arriving = !was_active && fading && !m->fade.stopped && in_b && !in_a;
			if (!near(was, ts) || (!was_active && !arriving)) {
				why[k] = !was_active ? 'A' : 'r';
				// First tick of this source (or a resync): offset unknown, leave it to the residual.
				// Also the tick a source becomes active outside a fade's incoming side: whether
				// libobs mixed it this tick is unknown, and there is no transition output to ask, so
				// a cut is one tick of Everything else. Neither end of a fade is left here: the
				// incoming side's first tick and the old scene's last tick are decided by the
				// transition's output (see above and below).
				m->st.gated_new[k]++;
				obs_source_release(s);
				continue;
			}
		}
		obs_source_get_audio_mix(s, &mix);
		side[k] = -1;
		if (ambiguous && in_a != in_b) {
			// Held back: added to a candidate, decided after the loop.
			obs_source_get_audio_mix(s, &mix);
			const float *il = mix.output[m->mix_idx].data[0], *ir = mix.output[m->mix_idx].data[1];
			add_into(cand[in_a ? 0 : 1][0], il);
			add_into(cand[in_a ? 0 : 1][1], ir);
			side[k] = in_a ? 0 : 1;
			contrib[k][0] = il;
			contrib[k][1] = ir;
			why[k] = in_a ? 'a' : 'b';
			held[n_held++] = s;
			continue;
		}
		if (in_a && !in_b && m->fade.stopped) {
			// Transition over: libobs mixes only B now, whatever A's active flag still says.
			why[k] = 'S';
			m->st.gated_new[k]++;
			obs_source_release(s);
			continue;
		}
		bool per_sample = fading && !m->fade.stopped && in_a != in_b; // on both sides: gain 1 (libobs mixes it once, as a root)
		float *dst_l = acc[e->fader][0], *dst_r = acc[e->fader][1];
		const float *in_l = mix.output[m->mix_idx].data[0], *in_r = mix.output[m->mix_idx].data[1];
		const float *c_l = in_l, *c_r = in_r;
		if (per_sample) {
			float *f_l = faded[k][0], *f_r = faded[k][1];
			for (size_t i = 0; i < FRAMES; i++) {
				uint64_t sts = ts + util_mul_div64(i, 1000000000ULL, RATE);
				float t = sts <= m->fade.start_ns ? 0.0f : sts - m->fade.start_ns >= m->fade.dur_ns ? 1.0f : (float)((double)(sts - m->fade.start_ns) / (double)m->fade.dur_ns);
				float g = in_a ? 1.0f - t : t;
				f_l[i] = in_l[i] * g;
				f_r[i] = in_r[i] * g;
			}
			c_l = f_l;
			c_r = f_r;
		}
		if (arriving) {
			contrib[k][0] = c_l;
			contrib[k][1] = c_r;
			arrived[k] = true;
			n_arrived++;
			why[k] = 'n';
			held[n_held++] = s;
			continue;
		}
		add_into(dst_l, c_l);
		add_into(dst_r, c_r);
		contrib[k][0] = c_l;
		contrib[k][1] = c_r;
		inc[k] = true;
		why[k] = per_sample ? 'f' : 'I';
		m->st.included[k]++;
		held[n_held++] = s;
	}

	if (n_arrived) {
		// Did libobs mix the arriving sources into this tick? The transition's output, less what
		// the model already has in it (every included source on one side of the fade), is what
		// they would have to account for: include them when that remainder correlates with them
		// (⟨r, c⟩ > |c|² / 2, i.e. r is closer to c than to nothing). No transition buffer: leave
		// them out, as before.
		if (!tout_read) {
			obs_source_t *t = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
			tout_read = true;
			if (t) {
				obs_source_get_audio_mix(t, &tmix);
				tout_l = tmix.output[m->mix_idx].data[0];
				tout_r = tmix.output[m->mix_idx].data[1];
				held[n_held++] = t;
			}
		}
		static float model[CH][FRAMES], arr[CH][FRAMES];
		double rc = 0, cc = 0;
		if (tout_l && tout_r) {
			memset(model, 0, sizeof(model));
			memset(arr, 0, sizeof(arr));
			for (int k = 0; k < n; k++)
				for (int c = 0; c < CH; c++) {
					if (inc[k] && one_side[k])
						add_into(model[c], contrib[k][c]);
					else if (arrived[k])
						add_into(arr[c], contrib[k][c]);
				}
			for (int c = 0; c < CH; c++) {
				const float *to = c ? tout_r : tout_l;
				for (size_t i = 0; i < FRAMES; i++) {
					double r = (double)to[i] - model[c][i];
					rc += r * arr[c][i];
					cc += (double)arr[c][i] * arr[c][i];
				}
			}
		}
		bool mixed = tout_l && tout_r && rc > 0.5 * cc;
		for (int k = 0; k < n; k++) {
			if (!arrived[k])
				continue;
			struct entry *e = &snap->e[k];
			if (!mixed) {
				why[k] = 'A';
				m->st.gated_new[k]++;
				continue;
			}
			add_into(acc[e->fader][0], contrib[k][0]);
			add_into(acc[e->fader][1], contrib[k][1]);
			inc[k] = true;
			m->st.included[k]++;
			why[k] = 'N';
		}
	}

	if (ambiguous) {
		// Which candidate did libobs play this tick? The one the transition's output is closer to.
		double err[2] = {0, 0};
		for (int c = 0; c < 2; c++)
			for (size_t i = 0; i < FRAMES; i++) {
				double dl = tout_l[i] - cand[c][0][i], dr = tout_r[i] - cand[c][1][i];
				err[c] += dl * dl + dr * dr;
			}
		int win = err[1] < err[0] ? 1 : 0;
		for (int k = 0; k < n; k++) {
			if (side[k] < 0)
				continue;
			struct entry *e = &snap->e[k];
			if (side[k] != win) {
				why[k] = 'E';
				m->st.gated_new[k]++;
				continue;
			}
			add_into(acc[e->fader][0], contrib[k][0]);
			add_into(acc[e->fader][1], contrib[k][1]);
			inc[k] = true;
			m->st.included[k]++;
			why[k] = win ? 'B' : 'A';
		}
	}
	if (m->fade.active && m->fade.stopped && !a_alive)
		m->fade.active = false; // A is gone from the program: back to plain gating

	// Everything else = the unclamped track − Σ faders.
	float *rest_l = acc[n_wire][0], *rest_r = acc[n_wire][1];
	const float *trk_l = (const float *)data->data[0], *trk_r = (const float *)data->data[1];
	// Σ faders first, fader by fader in the same order as before (so every sample rounds the same
	// way), then the subtraction: the buffers are walked one at a time instead of across.
	static float sum[CH][FRAMES];
	for (int c = 0; c < CH; c++) {
		float *restrict sm = sum[c];
		const float *restrict trk = c ? trk_r : trk_l;
		float *restrict rest = c ? rest_r : rest_l;
		memset(sm, 0, sizeof(sum[c]));
		for (int f = 0; f < n_wire; f++)
			add_into(sm, acc[f][c]);
		for (size_t i = 0; i < FRAMES; i++)
			rest[i] = trk[i] - sm[i];
	}
	float track_peak = peak_of(trk_r, peak_of(trk_l, 0));
	double rest_sq = energy(rest_l, rest_r);

	if (m->dbg_until && ts < m->dbg_until) {
		why[n < MAX_ENTRIES ? n : MAX_ENTRIES - 1] = 0;
		char names[512] = "";
		for (int k = 0; k < n && strlen(names) < 440; k++) {
			strncat(names, snap->e[k].name, 12);
			strncat(names, "/", 2);
		}
		blog(LOG_INFO, "[faders-dbg] tick ts-start=%.1f ms fading=%d stopped=%d rest=%.1f dBFS gates=%s names=%s", ((double)ts - (double)m->fade.start_ns) / 1e6, fading, m->fade.stopped, 10 * log10(rest_sq / (2.0 * FRAMES) + 1e-20), why, names);
	}
	// A transition is no evidence (see in_transition): drop the window in progress and start a
	// new one once it is over and the audio has settled. Runs of earlier windows are kept.
	bool oracle_hold = fading || m->in_transition || ts < m->oracle_hold_until_ns;
	if (oracle_hold) {
		for (int k = 0; k < n; k++)
			snap->e[k].xy = snap->e[k].xx = 0;
		yy = 0;
		oracle_ticks = -1; // ++ below makes it 0
	}
	// Oracle sums for every included source on every other tick (the window is 0.5 s; half the
	// samples are plenty of evidence and it halves the cost). Odd sources on one tick, even ones
	// on the next, so every tick does the same amount of work.
	for (int k = 0; k < n; k++) {
		if (!inc[k] || ((oracle_ticks + k) & 1) || oracle_hold)
			continue;
		struct entry *e = &snap->e[k];
		// Float sums within the tick (1024 samples), accumulated in double across the window.
		float xy, xx;
		correlate(rest_l, rest_r, contrib[k][0], contrib[k][1], &xy, &xx);
		e->xy += xy;
		e->xx += xx;
	}
	if (!oracle_hold)
		yy += rest_sq;
	if (++oracle_ticks >= ORACLE_TICKS) {
		oracle_ticks = 0;
		// r < 0: the stream lacks audio the fader carries (the source isn't in the stream the way
		// it was included), so it is moved to Everything else. r > 0: the stream has more of this
		// audio than the fader carries (captured twice, or another source plays the same sound).
		// Nothing is wrong with the fader then, so the source stays and the streamer is warned.
		const char *drop = NULL, *dup = NULL;
		bool dup_gone = false;
		for (int k = 0; k < n; k++) {
			struct entry *e = &snap->e[k];
			bool evaluable = e->fader != REST && e->xx > ORACLE_MIN_ENERGY && yy > ORACLE_MIN_ENERGY;
			if (evaluable) {
				double r = e->xy / sqrt(e->xx * yy);
				e->run = fabs(r) > ORACLE_R ? e->run + 1 : 0;
				if (e->run >= ORACLE_RUNS && r < 0 && !drop && m->n_dropped < MAX_ENTRIES) {
					drop = e->name;
					e->fader = REST;
					e->dropped = true;
					obs_weak_source_addref(e->weak);
					m->dropped[m->n_dropped++] = e->weak;
				} else if (e->run >= ORACLE_RUNS && r > 0 && !dup && !m->duplicate[0]) {
					dup = e->name;
				}
			}
			// The warned source is playing and the stream no longer has more of it.
			if (m->duplicate[0] && e->fader != REST && e->xx > ORACLE_MIN_ENERGY && strcmp(m->duplicate, e->name) == 0) {
				e->calm = evaluable && e->run ? 0 : e->calm + 1;
				dup_gone = e->calm >= ORACLE_CALM;
			}
			e->xy = e->xx = 0;
		}
		yy = 0;
		if (drop) {
			snprintf(m->mismatch, sizeof(m->mismatch), "%s", drop);
			fdr_log(LOG_WARNING, "\"%s\" is sent to Faders but is not in your stream (Track %d) the same way: moved to Everything else for this session. Check the source's track and that it isn't monitor-only.", drop, m->stream_track);
			fdr_uplink_set_mismatch(m->uplink, drop);
		}
		if (dup) {
			snprintf(m->duplicate, sizeof(m->duplicate), "%s", dup);
			fdr_log(LOG_WARNING, "\"%s\" is in your stream (Track %d) more than once: it is captured twice, or another source plays the same audio. Its fader only controls one copy; the other is in Everything else.", dup, m->stream_track);
			fdr_uplink_set_duplicate(m->uplink, dup);
		} else if (dup_gone) {
			fdr_log(LOG_INFO, "\"%s\" is no longer in your stream more than once", m->duplicate);
			m->duplicate[0] = 0;
			fdr_uplink_set_duplicate(m->uplink, NULL);
		}
	}

	m->st.ticks++;
	m->st.n += FRAMES * CH;
	m->st.rest_sq += rest_sq;
	for (int f = 0; f < n_wire; f++) {
		m->st.fader_sq[f] += energy(acc[f][0], acc[f][1]);
	}

	// Interleave and hand over: faders 0..n_wire-1, then Everything else.
	for (int f = 0; f <= n_wire; f++) {
		for (size_t i = 0; i < FRAMES; i++) {
			out[f][2 * i] = acc[f][0][i];
			out[f][2 * i + 1] = acc[f][1][i];
		}
		outp[f] = out[f];
	}
	uint64_t t_out = os_gettime_ns();
	if (m->st.n_cb < MAX_CB)
		m->st.cb_ns[m->st.n_cb++] = t_out - t_in;
	pthread_mutex_unlock(&m->mu);
	for (int i = 0; i < n_held; i++)
		obs_source_release(held[i]);
	if (!fdr_splitmix_tick(m->split, ts, outp, n_wire + 1, trk_l, trk_r, track_peak, fading, oracle_hold))
		fdr_uplink_push_tick(m->uplink, ts, outp, n_wire + 1, FRAMES, track_peak);
}

// ── Main thread: assignments ────────────────────────────────────────────────────────────

// Locked. Where a source plays: the fader it was put on (see placement.h), or REST: never placed
// (NEW), placed in Everything else, or on a fader that is locked by the plan or no longer exists.
// Nothing is guessed from the kind of source or its OBS tracks.
static int resolve_fader(struct fdr_mixer *m, obs_source_t *src, enum fdr_state *state)
{
	const char *names[FDR_MAX_FADERS];
	for (int i = 0; i < m->n_faders; i++)
		names[i] = m->names[i];
	struct fdr_fader_list l = {names, m->n_faders, m->n_wire};
	obs_data_t *priv = obs_source_get_private_settings(src);
	bool has = obs_data_has_user_value(priv, FDR_FADER_SETTING);
	const char *name = obs_data_get_string(priv, FDR_FADER_SETTING);
	enum fdr_saved saved = !has ? FDR_SAVED_NEW : *name ? FDR_SAVED_FADER : FDR_SAVED_REST;
	int idx = fdr_place_effective(&l, saved, name, false, state);
	obs_data_release(priv);
	// The split fader's YouTube source (and anything else on it but its Twitch source) is never
	// in these lanes: splitmix.c mixes it, or it plays in Everything else.
	if (idx >= 0 && fdr_splitmix_parks(m->split, src))
		idx = -1;
	return idx < 0 ? REST : idx;
}

static bool is_audio_input(obs_source_t *s)
{
	uint32_t flags = obs_source_get_output_flags(s);
	return (flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_COMPOSITE) && obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT;
}

struct collect {
	struct fdr_mixer *m;
	struct snapshot *snap;
};

static bool collect_cb(void *param, obs_source_t *s)
{
	struct collect *c = param;
	if (!is_audio_input(s))
		return true;
	if (c->snap->n >= MAX_ENTRIES) {
		c->snap->overflow = true;
		return true;
	}
	struct entry *e = &c->snap->e[c->snap->n];
	memset(e, 0, sizeof(*e));
	e->weak = obs_source_get_weak_source(s);
	snprintf(e->name, sizeof(e->name), "%s", obs_source_get_name(s));
	e->fader = resolve_fader(c->m, s, NULL);
	obs_data_t *priv = obs_source_get_private_settings(s);
	e->force = obs_data_get_bool(priv, FDR_FORCE_SETTING);
	obs_data_release(priv);
	for (int i = 0; i < c->m->n_dropped; i++)
		if (obs_weak_source_references_source(c->m->dropped[i], s)) {
			e->fader = REST;
			e->dropped = true;
		}
	c->snap->n++;
	return true;
}

static void free_snapshot(struct snapshot *s)
{
	if (!s)
		return;
	for (int i = 0; i < s->n; i++)
		obs_weak_source_release(s->e[i].weak);
	bfree(s);
}

static void on_transition_start(void *data, calldata_t *cd);
static void on_transition_stop(void *data, calldata_t *cd);

// Follow whichever transition sits on output channel 0 (the program). Not under m->mu: libobs
// calls signal handlers with the signal's own lock held, and ours take m->mu, so connecting or
// disconnecting while holding m->mu could deadlock against a transition starting. tr_mu keeps
// two rebuilds from doing this at once; no handler ever takes it.
static void track_transition(struct fdr_mixer *m)
{
	pthread_mutex_lock(&m->tr_mu);
	obs_source_t *t = obs_get_output_source(0);
	if (t && obs_source_get_type(t) != OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_release(t);
		t = NULL;
	}
	pthread_mutex_lock(&m->mu);
	obs_source_t *old = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
	pthread_mutex_unlock(&m->mu);
	if (old != t) {
		if (old) {
			signal_handler_t *sh = obs_source_get_signal_handler(old);
			signal_handler_disconnect(sh, "transition_start", on_transition_start, m);
			signal_handler_disconnect(sh, "transition_stop", on_transition_stop, m);
		}
		obs_weak_source_t *weak = t ? obs_source_get_weak_source(t) : NULL;
		pthread_mutex_lock(&m->mu);
		obs_weak_source_t *was = m->tr;
		m->tr = weak;
		// The old transition's stop signal can no longer reach us: don't wait for it.
		m->in_transition = false;
		m->oracle_hold_until_ns = os_gettime_ns() + ORACLE_SETTLE_NS;
		pthread_mutex_unlock(&m->mu);
		obs_weak_source_release(was);
		if (t) {
			signal_handler_t *sh = obs_source_get_signal_handler(t);
			signal_handler_connect(sh, "transition_start", on_transition_start, m);
			signal_handler_connect(sh, "transition_stop", on_transition_stop, m);
		}
	}
	obs_source_release(old);
	obs_source_release(t);
	pthread_mutex_unlock(&m->tr_mu);
}

// Locked. The oracle's evidence so far (each source's sums for the window in progress, its run of
// windows and its calm count) carries over to the new snapshot when the rebuild changed nothing it
// measures: every source in the old snapshot is still there, routed the same way. Sources may have
// been added (a source list is rebuilt more than once as sources come and go, and each rebuild
// used to restart every three-window count). Anything else, a source gone, replaced or routed
// elsewhere, starts every count over, as before: the evidence was about a residual that changed.
// "Routed" is the fader, force and dropped: a source plays on its one placed fader only (no extra
// faders from OBS tracks), and whether it is on the streamed track is read live every tick.
static void carry_oracle(const struct snapshot *old, struct snapshot *snap)
{
	if (!old)
		return;
	int match[MAX_ENTRIES];
	for (int i = 0; i < old->n; i++) {
		const struct entry *o = &old->e[i];
		obs_source_t *s = obs_weak_source_get_source(o->weak);
		match[i] = -1;
		// Sources keep their order when others are added, so look where it was first.
		for (int j = 0; s && j < snap->n && match[i] < 0; j++) {
			int k = (i + j) % snap->n;
			const struct entry *e = &snap->e[k];
			if (obs_weak_source_references_source(e->weak, s) && e->fader == o->fader && e->force == o->force && e->dropped == o->dropped)
				match[i] = k;
		}
		obs_source_release(s);
		if (match[i] < 0)
			return;
	}
	for (int i = 0; i < old->n; i++) {
		struct entry *e = &snap->e[match[i]];
		e->xy = old->e[i].xy;
		e->xx = old->e[i].xx;
		e->run = old->e[i].run;
		e->calm = old->e[i].calm;
	}
}

void fdr_mixer_rebuild(fdr_mixer_t *m)
{
	if (!m)
		return;
	split_refresh(m);
	struct snapshot *snap = bzalloc(sizeof(*snap));
	pthread_mutex_lock(&m->mu);
	struct collect c = {m, snap};
	obs_enum_sources(collect_cb, &c);
	struct snapshot *old = m->snap;
	carry_oracle(old, snap);
	m->snap = snap;
	bool warn = snap->overflow && !m->overflow_warned;
	m->overflow_warned = m->overflow_warned || snap->overflow;
	pthread_mutex_unlock(&m->mu);
	if (warn)
		fdr_log(LOG_WARNING, "more than %d audio sources: the ones past that are left in Everything else", MAX_ENTRIES);
	track_transition(m);
	free_snapshot(old);
}

// ── Transitions ─────────────────────────────────────────────────────────────────────────

struct tree_collect {
	obs_source_t **set;
	int *n;
};
static void tree_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct tree_collect *t = param;
	if (*t->n < MAX_TREE && !in_set(t->set, *t->n, child))
		t->set[(*t->n)++] = child;
}

static void on_transition_start(void *data, calldata_t *cd)
{
	struct fdr_mixer *m = data;
	obs_source_t *t = calldata_ptr(cd, "source");
	if (!t)
		return;
	pthread_mutex_lock(&m->mu);
	uint64_t dur = m->given_dur_ns ? m->given_dur_ns : m->learned_dur_ns ? m->learned_dur_ns : 300000000ULL;
	pthread_mutex_unlock(&m->mu);
	struct fade f = {.active = true, .start_ns = os_gettime_ns(), .dur_ns = dur};
	obs_source_t *a = obs_transition_get_source(t, OBS_TRANSITION_SOURCE_A), *b = obs_transition_get_source(t, OBS_TRANSITION_SOURCE_B);
	struct tree_collect ca = {f.a, &f.na}, cb = {f.b, &f.nb};
	if (a) {
		if (f.na < MAX_TREE)
			f.a[f.na++] = a;
		obs_source_enum_active_tree(a, tree_cb, &ca);
	}
	if (b) {
		if (f.nb < MAX_TREE)
			f.b[f.nb++] = b;
		obs_source_enum_active_tree(b, tree_cb, &cb);
	}
	const char *name_a = a ? obs_source_get_name(a) : "-", *name_b = b ? obs_source_get_name(b) : "-";
	if (obs_transition_fixed(t))
		f.active = false; // a stinger or other fixed-length transition: its audio is the transition's own; let the residual carry the change
	pthread_mutex_lock(&m->mu);
	m->in_transition = true; // every kind, modelled or not
	m->fade = f;
	m->fade.learning = f.active;
	if (getenv("FADERS_DEBUG"))
		m->dbg_until = f.start_ns + 1000000000ULL;
	pthread_mutex_unlock(&m->mu);
	if (getenv("FADERS_DEBUG"))
		fdr_log(LOG_INFO, "transition start: %s → %s, %llu ms, A tree %d, B tree %d, modelled=%d", name_a, name_b, (unsigned long long)(dur / 1000000ULL), f.na, f.nb, f.active);
	obs_source_release(a);
	obs_source_release(b);
}

void fdr_mixer_set_transition_duration(fdr_mixer_t *m, int duration_ms)
{
	if (!m)
		return;
	pthread_mutex_lock(&m->mu);
	m->given_dur_ns = duration_ms > 0 ? (uint64_t)duration_ms * 1000000ULL : 0;
	pthread_mutex_unlock(&m->mu);
}

static void on_transition_stop(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct fdr_mixer *m = data;
	pthread_mutex_lock(&m->mu);
	if (m->fade.active && !m->fade.stopped) {
		m->fade.stopped = true;
		m->fade.stopped_ns = os_gettime_ns();
	}
	m->in_transition = false;
	m->oracle_hold_until_ns = os_gettime_ns() + ORACLE_SETTLE_NS;
	pthread_mutex_unlock(&m->mu);
}

// ── Core signals: sources come and go, output channels change ───────────────────────────

static void on_source_change(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	((struct fdr_mixer *)data)->dirty = true;
}

static void *rebuild_thread(void *param)
{
	struct fdr_mixer *m = param;
	os_set_thread_name("faders-mixer");
	while (os_event_timedwait(m->stop_ev, 50) == ETIMEDOUT) {
		// The plan's allowance arrives from the hub after connecting (and can change mid-stream).
		int allowed = fdr_uplink_max_stems(m->uplink);
		if (allowed > FDR_MAX_FADERS)
			allowed = FDR_MAX_FADERS;
		if (allowed >= 1 && allowed != m->max_stems) {
			pthread_mutex_lock(&m->mu);
			m->max_stems = allowed;
			int wire = m->n_faders < allowed ? m->n_faders : allowed;
			if (m->n_faders > allowed)
				fdr_log(LOG_INFO, "your plan includes %d faders: \"%s\" and later are folded into Everything else", allowed, m->names[allowed]);
			m->n_wire = wire;
			announce(m);
			pthread_mutex_unlock(&m->mu);
			m->dirty = true;
		}
		if (fdr_splitmix_poll(m->split))
			m->dirty = true; // the hub's hello changed: the split may turn on or off
		if (m->dirty) {
			m->dirty = false;
			fdr_mixer_rebuild(m);
		}
	}
	return NULL;
}

static const char *const CORE_SIGNALS[] = {"source_create", "source_destroy", "source_remove", "source_rename", "source_load", "channel_change", NULL};

static void connect_signals(struct fdr_mixer *m, bool on)
{
	signal_handler_t *sh = obs_get_signal_handler();
	for (const char *const *s = CORE_SIGNALS; *s; s++) {
		if (on)
			signal_handler_connect(sh, *s, on_source_change, m);
		else
			signal_handler_disconnect(sh, *s, on_source_change, m);
	}
	m->signals = on;
}

// ── Lifecycle / configuration ───────────────────────────────────────────────────────────

static void tap(struct fdr_mixer *m, bool on)
{
	// The mixer's own planar float buffers at 48 kHz stereo, unclamped: no resampler in between.
	struct audio_convert_info conv = {.samples_per_sec = RATE, .format = AUDIO_FORMAT_FLOAT_PLANAR, .speakers = SPEAKERS_STEREO, .allow_clipping = true};
	if (on && !m->tapping) {
		obs_add_raw_audio_callback(m->mix_idx, &conv, on_track, m);
		m->tapping = true;
	} else if (!on && m->tapping) {
		obs_remove_raw_audio_callback(m->mix_idx, on_track, m);
		m->tapping = false;
	}
}

fdr_mixer_t *fdr_mixer_start(fdr_uplink_t *uplink, int stream_track)
{
	struct obs_audio_info ai;
	if (!obs_get_audio_info(&ai) || ai.speakers != SPEAKERS_STEREO) {
		fdr_log(LOG_WARNING, "Faders needs stereo OBS audio (Settings → Audio → Channels): not running");
		return NULL;
	}
	if (ai.samples_per_sec != RATE) {
		fdr_log(LOG_WARNING, "Faders needs 48 kHz OBS audio (Settings → Audio → Sample Rate): not running");
		return NULL;
	}
	struct fdr_mixer *m = bzalloc(sizeof(*m));
	m->uplink = uplink;
	pthread_mutex_init(&m->mu, NULL);
	pthread_mutex_init(&m->tr_mu, NULL);
	m->max_stems = FDR_MAX_FADERS;
	m->stream_track = stream_track < 1 || stream_track > 6 ? 1 : stream_track;
	m->mix_idx = (size_t)(m->stream_track - 1);
	m->stream_bit = 1u << m->mix_idx;
	m->split = fdr_splitmix_create(uplink, m->stream_track);
	connect_signals(m, true);
	fdr_mixer_rebuild(m);
	os_event_init(&m->stop_ev, OS_EVENT_TYPE_MANUAL);
	pthread_create(&m->thread, NULL, rebuild_thread, m);
	tap(m, true);
	fdr_log(LOG_INFO, "virtual mixer started on Track %d", m->stream_track);
	return m;
}

void fdr_mixer_stop(fdr_mixer_t *m)
{
	if (!m)
		return;
	tap(m, false);
	connect_signals(m, false);
	os_event_signal(m->stop_ev);
	pthread_join(m->thread, NULL);
	os_event_destroy(m->stop_ev);
	fdr_splitmix_destroy(m->split); // its tap stops here
	m->split = NULL;
	// Every other thread that touches the mixer has stopped. A transition handler may still be
	// running: disconnecting waits for it, so m->mu must be free here.
	obs_source_t *t = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
	if (t) {
		signal_handler_t *sh = obs_source_get_signal_handler(t);
		signal_handler_disconnect(sh, "transition_start", on_transition_start, m);
		signal_handler_disconnect(sh, "transition_stop", on_transition_stop, m);
		obs_source_release(t);
	}
	pthread_mutex_lock(&m->mu);
	obs_weak_source_release(m->tr);
	struct snapshot *snap = m->snap;
	m->snap = NULL;
	for (int i = 0; i < m->n_dropped; i++)
		obs_weak_source_release(m->dropped[i]);
	pthread_mutex_unlock(&m->mu);
	free_snapshot(snap);
	pthread_mutex_destroy(&m->mu);
	pthread_mutex_destroy(&m->tr_mu);
	bfree(m);
	fdr_log(LOG_INFO, "virtual mixer stopped");
}

void fdr_mixer_set_stream_track(fdr_mixer_t *m, int stream_track)
{
	if (!m || stream_track < 1 || stream_track > 6 || stream_track == m->stream_track)
		return;
	tap(m, false);
	pthread_mutex_lock(&m->mu);
	m->stream_track = stream_track;
	m->mix_idx = (size_t)(stream_track - 1);
	m->stream_bit = 1u << m->mix_idx;
	pthread_mutex_unlock(&m->mu);
	fdr_splitmix_set_stream_track(m->split, stream_track);
	tap(m, true);
}

// Locked. Tell the uplink the stems on the wire.
static void announce(struct fdr_mixer *m)
{
	const char *names[FDR_MAX_STEMS];
	int n = m->n_wire;
	for (int i = 0; i < n; i++)
		names[i] = m->names[i];
	if (fdr_splitmix_announce(m->split, names, n))
		return; // the split's layout (two more layers)
	names[n] = "Everything else";
	fdr_uplink_set_stems(m->uplink, names, n + 1, true);
}

void fdr_mixer_set_faders(fdr_mixer_t *m, const char *const *names, int n, int max_stems)
{
	if (!m)
		return;
	if (n > FDR_MAX_FADERS)
		n = FDR_MAX_FADERS;
	if (max_stems < 1)
		max_stems = 1;
	if (max_stems > FDR_MAX_FADERS)
		max_stems = FDR_MAX_FADERS;
	pthread_mutex_lock(&m->mu);
	m->n_faders = 0;
	for (int i = 0; i < n; i++) {
		if (!names[i] || !*names[i])
			continue;
		snprintf(m->names[m->n_faders], sizeof(m->names[0]), "%s", names[i]);
		m->n_faders++;
	}
	m->max_stems = max_stems;
	int wire = m->n_faders < max_stems ? m->n_faders : max_stems;
	if (wire != m->n_wire && m->n_faders > max_stems)
		fdr_log(LOG_INFO, "your plan includes %d faders: \"%s\" and later are folded into Everything else", max_stems, m->names[max_stems]);
	m->n_wire = wire;
	announce(m);
	pthread_mutex_unlock(&m->mu);
	fdr_mixer_rebuild(m);
}

int fdr_mixer_fader_count(fdr_mixer_t *m)
{
	return m ? m->n_faders : 0;
}

int fdr_mixer_faders_on_wire(fdr_mixer_t *m)
{
	return m ? m->n_wire : 0;
}

const char *fdr_mixer_fader_name(fdr_mixer_t *m, int i)
{
	return m && i >= 0 && i < m->n_faders ? m->names[i] : NULL;
}

const char *fdr_mixer_effective_fader(fdr_mixer_t *m, obs_source_t *src)
{
	if (!m || !src)
		return NULL;
	pthread_mutex_lock(&m->mu);
	int idx = resolve_fader(m, src, NULL);
	for (int i = 0; i < m->n_dropped; i++)
		if (obs_weak_source_references_source(m->dropped[i], src))
			idx = REST;
	const char *name = idx == REST ? NULL : m->names[idx];
	pthread_mutex_unlock(&m->mu);
	return name;
}

bool fdr_mixer_is_dropped(fdr_mixer_t *m, obs_source_t *src)
{
	if (!m || !src)
		return false;
	bool dropped = false;
	pthread_mutex_lock(&m->mu);
	for (int i = 0; i < m->n_dropped && !dropped; i++)
		dropped = obs_weak_source_references_source(m->dropped[i], src);
	pthread_mutex_unlock(&m->mu);
	return dropped;
}

void fdr_mixer_assign(fdr_mixer_t *m, obs_source_t *src, const char *fader)
{
	if (!src)
		return;
	obs_data_t *priv = obs_source_get_private_settings(src);
	if (!fader)
		obs_data_erase(priv, FDR_FADER_SETTING);
	else
		obs_data_set_string(priv, FDR_FADER_SETTING, fader);
	obs_data_release(priv);
	fdr_mixer_rebuild(m);
}

// ── Test hook ───────────────────────────────────────────────────────────────────────────

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

void fdr_mixer_stats_json(fdr_mixer_t *m, struct dstr *out)
{
	if (!m) {
		dstr_copy(out, "{}");
		return;
	}
	pthread_mutex_lock(&m->mu);
	struct stats st = m->st;
	struct snapshot *snap = m->snap;
	int n_wire = m->n_wire;
	double dbfs = st.n ? 20.0 * log10(sqrt(st.rest_sq / (double)st.n) + 1e-12) : -999;
	dstr_printf(out, "{\"ticks\":%llu,\"rest_rms\":%.6f,\"rest_dbfs\":%.1f,\"faders\":[", (unsigned long long)st.ticks, st.n ? sqrt(st.rest_sq / (double)st.n) : 0.0, dbfs);
	for (int f = 0; f < n_wire; f++)
		dstr_catf(out, "%s{\"name\":\"%s\",\"rms\":%.6f}", f ? "," : "", m->names[f], st.n ? sqrt(st.fader_sq[f] / (double)st.n) : 0.0);
	dstr_cat(out, "],\"sources\":[");
	for (int k = 0; snap && k < snap->n; k++)
		dstr_catf(out, "%s{\"name\":\"%s\",\"fader\":\"%s\",\"included\":%d,\"new\":%d,\"dropped\":%s}", k ? "," : "", snap->e[k].name,
			  snap->e[k].fader == REST ? "" : m->names[snap->e[k].fader], st.included[k], st.gated_new[k], snap->e[k].dropped ? "true" : "false");
	double p50 = 0, p99 = 0, mx = 0;
	if (st.n_cb) {
		qsort(st.cb_ns, (size_t)st.n_cb, sizeof(uint64_t), cmp_u64);
		p50 = st.cb_ns[st.n_cb / 2] / 1000.0;
		p99 = st.cb_ns[(int)(0.99 * (st.n_cb - 1))] / 1000.0;
		mx = st.cb_ns[st.n_cb - 1] / 1000.0;
	}
	dstr_catf(out, "],\"cb_us\":{\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f,\"n\":%d},\"mismatch\":\"%s\",\"duplicate\":\"%s\",\"fading\":%s}", p50, p99, mx, st.n_cb, m->mismatch, m->duplicate, m->fade.active ? "true" : "false");
	memset(&m->st, 0, sizeof(m->st));
	pthread_mutex_unlock(&m->mu);
}

// ── Split fader ─────────────────────────────────────────────────────────────────────────

fdr_splitmix_t *fdr_mixer_split(fdr_mixer_t *m)
{
	return m ? m->split : NULL;
}

// Not locked. The split fader's sources and the hub's account, before the sources are collected
// (they decide which sources it parks); a layout change is announced at once.
static void split_refresh(struct fdr_mixer *m)
{
	if (!fdr_splitmix_refresh(m->split))
		return;
	pthread_mutex_lock(&m->mu);
	announce(m);
	pthread_mutex_unlock(&m->mu);
}
