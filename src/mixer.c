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

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RATE 48000
#define FRAMES AUDIO_OUTPUT_FRAMES // 1024 per tick
#define CH 2
#define MAX_ENTRIES 64
#define REST FDR_MAX_FADERS // fader index meaning "Everything else"
// Oracle: per ~0.5 s window (24 ticks), the residual correlated with a source's contribution.
// |r| > 0.5 for 3 windows in a row means the source isn't in the stream the way we include it:
// drop it (its audio then flows through Everything else, exactly as the stream has it).
#define ORACLE_TICKS 24
#define ORACLE_R 0.5
#define ORACLE_RUNS 3
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
	bool dropped;
};

struct snapshot {
	int n;
	struct entry e[MAX_ENTRIES];
};

// Transition in flight: libobs's own formula (t = (sample_ts − start) / duration) per sample,
// A × (1 − t) + B × t for sources only on one side.
struct fade {
	bool active;
	bool learning; // measure the real duration from obs_transition_get_time() reaching 1
	uint64_t start_ns, dur_ns;
	obs_source_t *a[MAX_TREE], *b[MAX_TREE]; // raw pointers for membership only
	int na, nb;
};

struct ats_slot {
	obs_source_t *src;
	uint64_t ats;
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
	struct ats_slot ats[MAX_ENTRIES * 2];
	struct stats st;

	obs_weak_source_t *tr; // transition whose signals we're connected to
	bool signals;
	// Core signals fire while libobs holds its own locks, so they only flag; this thread does
	// the rebuild (which enumerates sources) a moment later.
	pthread_t thread;
	os_event_t *stop_ev;
	volatile bool dirty;
	uint64_t given_dur_ns;   // from the frontend's transition duration setting (0 = none)
	uint64_t learned_dur_ns; // measured from the last fade: when get_time() reached 1
};

static void announce(struct fdr_mixer *m);

static inline bool near(uint64_t a, uint64_t b)
{
	return (a > b ? a - b : b - a) <= 4;
}

// ── Audio thread ────────────────────────────────────────────────────────────────────────

static uint64_t *ats_for(struct fdr_mixer *m, obs_source_t *src)
{
	struct ats_slot *free_slot = NULL;
	for (size_t i = 0; i < sizeof(m->ats) / sizeof(*m->ats); i++) {
		if (m->ats[i].src == src)
			return &m->ats[i].ats;
		if (!m->ats[i].src && !free_slot)
			free_slot = &m->ats[i];
	}
	if (!free_slot)
		free_slot = &m->ats[0];
	free_slot->src = src;
	free_slot->ats = 0;
	return &free_slot->ats;
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

	pthread_mutex_lock(&m->mu);
	uint64_t t_in = os_gettime_ns();
	struct snapshot *snap = m->snap;
	int n_wire = m->n_wire;
	// The gains stay applied (clamped at t = 1: A silent, B full) until the transition stops:
	// libobs keeps crossfading A at 0 until the video side ends it and A goes inactive.
	bool fading = m->fade.active && ts_end > m->fade.start_ns;
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

	int n = snap ? snap->n : 0;
	for (int k = 0; k < n; k++) {
		struct entry *e = &snap->e[k];
		inc[k] = false;
		obs_source_t *s = obs_weak_source_get_source(e->weak);
		if (!s)
			continue;
		uint64_t *prev = ats_for(m, s);
		uint64_t ats = obs_source_get_audio_timestamp(s);
		uint64_t was = *prev;
		*prev = ats;
		if (e->fader == REST) { // Everything else: whatever the stream has of it is in the residual
			obs_source_release(s);
			continue;
		}
		if (!e->force) {
			// The gate: what libobs mixed into the streamed track this tick, from frame 0.
			if (!obs_source_active(s) || !(obs_source_get_audio_mixers(s) & m->stream_bit) ||
			    obs_source_get_monitoring_type(s) == OBS_MONITORING_TYPE_MONITOR_ONLY || obs_source_audio_pending(s) || !near(ats, ts_end)) {
				obs_source_release(s);
				continue;
			}
			if (!near(was, ts)) {
				// First tick of this source (or a resync): offset unknown, leave it to the residual.
				m->st.gated_new[k]++;
				obs_source_release(s);
				continue;
			}
		}
		obs_source_get_audio_mix(s, &mix);
		bool in_a = fading && in_set(m->fade.a, m->fade.na, s), in_b = fading && in_set(m->fade.b, m->fade.nb, s);
		bool per_sample = fading && in_a != in_b; // on both sides: gain 1 (libobs mixes it once, as a root)
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
		for (size_t i = 0; i < FRAMES; i++) {
			dst_l[i] += c_l[i];
			dst_r[i] += c_r[i];
		}
		contrib[k][0] = c_l;
		contrib[k][1] = c_r;
		inc[k] = true;
		m->st.included[k]++;
		obs_source_release(s);
	}

	// Everything else = the unclamped track − Σ faders.
	float *rest_l = acc[n_wire][0], *rest_r = acc[n_wire][1];
	const float *trk_l = (const float *)data->data[0], *trk_r = (const float *)data->data[1];
	float track_peak = 0;
	double rest_sq = 0;
	for (size_t i = 0; i < FRAMES; i++) {
		float sl = 0, sr = 0;
		for (int f = 0; f < n_wire; f++) {
			sl += acc[f][0][i];
			sr += acc[f][1][i];
		}
		rest_l[i] = trk_l[i] - sl;
		rest_r[i] = trk_r[i] - sr;
		float pl = fabsf(trk_l[i]), pr = fabsf(trk_r[i]);
		track_peak = pl > track_peak ? pl : track_peak;
		track_peak = pr > track_peak ? pr : track_peak;
		rest_sq += (double)rest_l[i] * rest_l[i] + (double)rest_r[i] * rest_r[i];
	}

	// Oracle sums for every included source, every other tick (the window is 0.5 s; half the
	// samples are plenty of evidence and it halves the per-tick cost).
	for (int k = 0; k < n; k++) {
		if (!inc[k] || (m->st.ticks & 1))
			continue;
		struct entry *e = &snap->e[k];
		// Float sums within the tick (1024 samples), accumulated in double across the window.
		float xy = 0, xx = 0;
		const float *cl = contrib[k][0], *cr = contrib[k][1];
		for (size_t i = 0; i < FRAMES; i++) {
			xy += rest_l[i] * cl[i] + rest_r[i] * cr[i];
			xx += cl[i] * cl[i] + cr[i] * cr[i];
		}
		e->xy += xy;
		e->xx += xx;
	}
	yy += rest_sq;
	if (++oracle_ticks >= ORACLE_TICKS) {
		oracle_ticks = 0;
		const char *drop = NULL;
		for (int k = 0; k < n; k++) {
			struct entry *e = &snap->e[k];
			bool evaluable = e->fader != REST && e->xx > ORACLE_MIN_ENERGY && yy > ORACLE_MIN_ENERGY;
			if (evaluable) {
				double r = e->xy / sqrt(e->xx * yy);
				e->run = fabs(r) > ORACLE_R ? e->run + 1 : 0;
				if (e->run >= ORACLE_RUNS && !drop && m->n_dropped < MAX_ENTRIES) {
					drop = e->name;
					e->fader = REST;
					e->dropped = true;
					obs_weak_source_addref(e->weak);
					m->dropped[m->n_dropped++] = e->weak;
				}
			}
			e->xy = e->xx = 0;
		}
		yy = 0;
		if (drop) {
			snprintf(m->mismatch, sizeof(m->mismatch), "%s", drop);
			blog(LOG_WARNING, "[faders] \"%s\" is sent to Faders but is not in your stream (Track %d) the same way: moved to Everything else for this session. Check the source's track and that it isn't captured twice.", drop, m->stream_track);
			fdr_uplink_set_mismatch(m->uplink, drop);
		}
	}

	m->st.ticks++;
	m->st.n += FRAMES * CH;
	m->st.rest_sq += rest_sq;
	for (int f = 0; f < n_wire; f++) {
		double sq = 0;
		for (size_t i = 0; i < FRAMES; i++)
			sq += (double)acc[f][0][i] * acc[f][0][i] + (double)acc[f][1][i] * acc[f][1][i];
		m->st.fader_sq[f] += sq;
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
	fdr_uplink_push_tick(m->uplink, ts, outp, n_wire + 1, FRAMES, track_peak);
}

// ── Main thread: assignments ────────────────────────────────────────────────────────────

static bool hint_has(obs_source_t *src, const char *needle)
{
	char buf[512];
	const char *name = obs_source_get_name(src);
	obs_data_t *s = obs_source_get_settings(src);
	static const char *const keys[] = {"window", "application", "url", "local_file", "playlist", "device_id"};
	bool hit = false;
	for (int pass = 0; pass < 1 + (int)(sizeof(keys) / sizeof(*keys)) && !hit; pass++) {
		const char *v = pass == 0 ? name : obs_data_get_string(s, keys[pass - 1]);
		if (!v || !*v)
			continue;
		size_t n = strlen(v);
		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;
		for (size_t i = 0; i < n; i++)
			buf[i] = (char)tolower((unsigned char)v[i]);
		buf[n] = 0;
		hit = strstr(buf, needle) != NULL;
	}
	obs_data_release(s);
	return hit;
}

static bool any_hint(obs_source_t *src, const char *const *needles)
{
	for (; *needles; needles++)
		if (hint_has(src, *needles))
			return true;
	return false;
}

const char *fdr_mixer_auto_fader(obs_source_t *src)
{
	const char *id = obs_source_get_id(src);
	static const char *const mics[] = {"wasapi_input_capture", "coreaudio_input_capture", "pulse_input_capture", "alsa_input_capture", NULL};
	static const char *const app_capture[] = {"wasapi_process_output_capture", "sck_audio_capture", "pipewire-audio-capture-app", NULL};
	static const char *const desktop[] = {"wasapi_output_capture", "coreaudio_output_capture", "pulse_output_capture", NULL};
	static const char *const video_capture[] = {"game_capture", "window_capture", "monitor_capture", "screen_capture", "display_capture", "xcomposite_input", "pipewire-screen-capture-source", "pipewire-window-capture-source", NULL};
	static const char *const media[] = {"ffmpeg_source", "vlc_source", "slideshow", NULL};
	static const char *const voip[] = {"discord", "teamspeak", "slack", "zoom", "skype", "mumble", NULL};
	static const char *const music_apps[] = {"spotify", "apple music", "tidal", "foobar", "deezer", "youtube music", "music", "playlist", NULL};
	static const char *const alerts[] = {"alert", "streamelements", "streamlabs", "sound alert", NULL};
	bool is = false;
	for (const char *const *p = mics; *p; p++)
		is = is || strcmp(id, *p) == 0;
	if (is)
		return "Voice";
	bool app = false;
	for (const char *const *p = app_capture; *p; p++)
		app = app || strcmp(id, *p) == 0;
	if (app && any_hint(src, voip))
		return "Discord";
	bool med = false;
	for (const char *const *p = media; *p; p++)
		med = med || strcmp(id, *p) == 0;
	if ((app || med) && any_hint(src, music_apps))
		return "Music";
	if (strcmp(id, "browser_source") == 0)
		return any_hint(src, alerts) ? "Alerts" : NULL;
	if (any_hint(src, alerts))
		return "Alerts";
	bool game = app || med;
	for (const char *const *p = desktop; *p && !game; p++)
		game = strcmp(id, *p) == 0;
	for (const char *const *p = video_capture; *p && !game; p++)
		game = strcmp(id, *p) == 0;
	return game ? "Game" : NULL;
}

// Index of a fader by name (case-insensitive), or -1.
static int fader_index(struct fdr_mixer *m, const char *name)
{
	if (!name || !*name)
		return -1;
	for (int i = 0; i < m->n_faders; i++)
		if (astrcmpi(m->names[i], name) == 0)
			return i;
	return -1;
}

// Locked. The fader index for a source: its private setting, else the rule table (with the
// table's fallbacks when that fader doesn't exist), folded faders → REST.
static int resolve_fader(struct fdr_mixer *m, obs_source_t *src, bool *is_auto)
{
	obs_data_t *priv = obs_source_get_private_settings(src);
	int idx = -1;
	bool automatic = !obs_data_has_user_value(priv, FDR_FADER_SETTING);
	if (!automatic) {
		idx = fader_index(m, obs_data_get_string(priv, FDR_FADER_SETTING));
	} else {
		const char *pick = fdr_mixer_auto_fader(src);
		idx = fader_index(m, pick);
		if (idx < 0 && pick && strcmp(pick, "Discord") == 0)
			idx = fader_index(m, "Voice");
	}
	obs_data_release(priv);
	if (is_auto)
		*is_auto = automatic;
	if (idx < 0 || idx >= m->n_wire)
		return REST;
	return idx;
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
	if (!is_audio_input(s) || c->snap->n >= MAX_ENTRIES)
		return true;
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

// Locked. Follow whichever transition sits on output channel 0 (the program).
static void track_transition(struct fdr_mixer *m)
{
	obs_source_t *t = obs_get_output_source(0);
	if (t && obs_source_get_type(t) != OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_release(t);
		t = NULL;
	}
	obs_source_t *old = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
	if (old != t) {
		if (old) {
			signal_handler_t *sh = obs_source_get_signal_handler(old);
			signal_handler_disconnect(sh, "transition_start", on_transition_start, m);
			signal_handler_disconnect(sh, "transition_stop", on_transition_stop, m);
		}
		obs_weak_source_release(m->tr);
		m->tr = NULL;
		if (t) {
			signal_handler_t *sh = obs_source_get_signal_handler(t);
			signal_handler_connect(sh, "transition_start", on_transition_start, m);
			signal_handler_connect(sh, "transition_stop", on_transition_stop, m);
			m->tr = obs_source_get_weak_source(t);
		}
	}
	obs_source_release(old);
	obs_source_release(t);
}

void fdr_mixer_rebuild(fdr_mixer_t *m)
{
	if (!m)
		return;
	struct snapshot *snap = bzalloc(sizeof(*snap));
	pthread_mutex_lock(&m->mu);
	struct collect c = {m, snap};
	obs_enum_sources(collect_cb, &c);
	struct snapshot *old = m->snap;
	m->snap = snap;
	track_transition(m);
	pthread_mutex_unlock(&m->mu);
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
	m->fade = f;
	m->fade.learning = f.active;
	pthread_mutex_unlock(&m->mu);
	if (getenv("FADERS_DEBUG"))
		blog(LOG_INFO, "[faders] transition start: %s → %s, %llu ms, A tree %d, B tree %d, modelled=%d", name_a, name_b, (unsigned long long)(dur / 1000000ULL), f.na, f.nb, f.active);
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
	m->fade.active = false;
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
				blog(LOG_INFO, "[faders] your plan includes %d faders: \"%s\" and later are folded into Everything else", allowed, m->names[allowed]);
			m->n_wire = wire;
			announce(m);
			pthread_mutex_unlock(&m->mu);
			m->dirty = true;
		}
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
		blog(LOG_WARNING, "[faders] sources mode needs stereo OBS audio (Settings → Audio → Channels); falling back to tracks mode");
		return NULL;
	}
	if (ai.samples_per_sec != RATE) {
		blog(LOG_WARNING, "[faders] sources mode needs 48 kHz OBS audio (Settings → Audio → Sample Rate); falling back to tracks mode");
		return NULL;
	}
	struct fdr_mixer *m = bzalloc(sizeof(*m));
	m->uplink = uplink;
	pthread_mutex_init(&m->mu, NULL);
	m->max_stems = FDR_MAX_FADERS;
	m->stream_track = stream_track < 1 || stream_track > 6 ? 1 : stream_track;
	m->mix_idx = (size_t)(m->stream_track - 1);
	m->stream_bit = 1u << m->mix_idx;
	connect_signals(m, true);
	fdr_mixer_rebuild(m);
	os_event_init(&m->stop_ev, OS_EVENT_TYPE_MANUAL);
	pthread_create(&m->thread, NULL, rebuild_thread, m);
	tap(m, true);
	blog(LOG_INFO, "[faders] virtual mixer started on Track %d", m->stream_track);
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
	pthread_mutex_lock(&m->mu);
	obs_source_t *t = m->tr ? obs_weak_source_get_source(m->tr) : NULL;
	if (t) {
		signal_handler_t *sh = obs_source_get_signal_handler(t);
		signal_handler_disconnect(sh, "transition_start", on_transition_start, m);
		signal_handler_disconnect(sh, "transition_stop", on_transition_stop, m);
		obs_source_release(t);
	}
	obs_weak_source_release(m->tr);
	struct snapshot *snap = m->snap;
	m->snap = NULL;
	for (int i = 0; i < m->n_dropped; i++)
		obs_weak_source_release(m->dropped[i]);
	pthread_mutex_unlock(&m->mu);
	free_snapshot(snap);
	pthread_mutex_destroy(&m->mu);
	bfree(m);
	blog(LOG_INFO, "[faders] virtual mixer stopped");
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
	tap(m, true);
}

// Locked. Tell the uplink the stems on the wire.
static void announce(struct fdr_mixer *m)
{
	const char *names[FDR_MAX_STEMS];
	int n = m->n_wire;
	for (int i = 0; i < n; i++)
		names[i] = m->names[i];
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
		blog(LOG_INFO, "[faders] your plan includes %d faders: \"%s\" and later are folded into Everything else", max_stems, m->names[max_stems]);
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
	dstr_catf(out, "],\"cb_us\":{\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f,\"n\":%d},\"mismatch\":\"%s\",\"fading\":%s}", p50, p99, mx, st.n_cb, m->mismatch, m->fade.active ? "true" : "false");
	memset(&m->st, 0, sizeof(m->st));
	pthread_mutex_unlock(&m->mu);
}
