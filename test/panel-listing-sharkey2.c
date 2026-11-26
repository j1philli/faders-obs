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
// faders-panel-listing-sharkey2: a streamer's real panel (Voice, Game, Music and a long Everything
// else) with an Audio Input Capture of a virtual cable, "6. C - VC - MUSIC", created after the rest.
// Every audio input OBS has must be on some list: a fader's rows, or the Everything else line.
// OBS counts every browser source, media source and game/window capture with audio as an audio
// input, so a scene collection can have far more of them than the panel's named rows suggest.
#include "panel-listing.h"

static const char *kind_name(void *u) { return "Stand-in"; }
#define KIND(var, kind_id)                                                                                      \
	static struct obs_source_info var = {                                                                   \
		.id = kind_id,                                                                                  \
		.type = OBS_SOURCE_TYPE_INPUT,                                                                  \
		.output_flags = OBS_SOURCE_AUDIO,                                                               \
		.get_name = kind_name,                                                                          \
		.create = panel_stub_create,                                                                    \
		.destroy = panel_stub_destroy,                                                                  \
	}
KIND(desktop_info, "wasapi_output_capture");
KIND(app_info, "wasapi_process_output_capture");
KIND(game_info, "game_capture");
KIND(window_info, "window_capture");
KIND(media_info, "ffmpeg_source");

static obs_source_t *make(const char *kind, const char *name)
{
	return obs_source_create(kind, name, NULL, NULL);
}

#define CABLE_C "6. C - VC - MUSIC"

// Whether `name` is on some list: a row under one of the faders, or the Everything else line.
static bool listed(const char *name)
{
	char want[256];
	snprintf(want, sizeof(want), "%s,", name); // names in the line end in ", " or "."
	char want_end[256];
	snprintf(want_end, sizeof(want_end), "%s.", name);
	char want_new[256]; // a source never placed is NEW: "name (new)"
	snprintf(want_new, sizeof(want_new), "%s (new)", name);
	const char *p = strstr(panel_rows, "rest_info\t");
	const char *e = p ? strchr(p, '\n') : NULL;
	bool rest = false;
	if (p && e) {
		size_t n = (size_t)(e - p);
		char *line = malloc(n + 1);
		memcpy(line, p, n);
		line[n] = 0;
		rest = strstr(line, want) || strstr(line, want_end) || strstr(line, want_new);
		free(line);
	}
	return rest || in_block("Voice", name) || in_block("Game", name) || in_block("Music", name);
}

static const char *where(const char *name)
{
	return in_block("Voice", name) ? "Voice" : in_block("Game", name) ? "Game" : in_block("Music", name) ? "Music" : listed(name) ? "Everything else" : "NOWHERE";
}

static obs_source_t *fillers[512];
static int n_fillers;
// Browser sources in Everything else (overlays, chat, alerts without "alert" in the name...).
static void add_fillers(int n)
{
	for (int i = 0; i < n; i++) {
		char name[32];
		snprintf(name, sizeof(name), "Overlay %d", n_fillers + 1);
		fillers[n_fillers++] = make("browser_source", name);
	}
}

static int count_inputs_cb_n;
static bool count_inputs_cb(void *u, obs_source_t *s)
{
	if (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)
		count_inputs_cb_n++;
	return true;
}
static int audio_inputs(void)
{
	count_inputs_cb_n = 0;
	obs_enum_sources(count_inputs_cb, NULL);
	return count_inputs_cb_n;
}

// "6. C - VC - MUSIC" made again, so it is the newest source (OBS enumerates in creation order,
// which a scene collection keeps when it is saved and loaded).
static obs_source_t *recreate_c(obs_source_t *c, const char *fader)
{
	if (c) {
		obs_source_remove(c);
		obs_source_release(c);
	}
	c = cable(CABLE_C, "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-00000000000c}");
	if (fader)
		place(c, fader);
	return c;
}

static void drop_fillers(int k)
{
	for (; k > 0 && n_fillers > 0; k--) {
		obs_source_t *f = fillers[--n_fillers];
		obs_source_remove(f);
		obs_source_release(f);
	}
}

// 6. C made again as the newest source and the `position`th audio input (fillers added or taken
// away before it), placed on `fader` (NULL: never placed).
static obs_source_t *newest_c_at(obs_source_t *c, int position, const char *fader)
{
	if (c) {
		obs_source_remove(c);
		obs_source_release(c);
	}
	int now = audio_inputs();
	if (now > position - 1)
		drop_fillers(now - (position - 1));
	else
		add_fillers(position - 1 - now);
	return recreate_c(NULL, fader);
}

int main(void)
{
	obs_source_t *settings_source = panel_start("Voice,Game,Music");
	if (!settings_source)
		return 1;
	obs_register_source(&desktop_info);
	obs_register_source(&app_info);
	obs_register_source(&game_info);
	obs_register_source(&window_info);
	obs_register_source(&media_info);

	// His panel as described. On 0.4.0 the rule table put most of these where his screenshot shows
	// them; nothing is placed by kind any more, so the fixture places each one where it is shown.
	obs_source_t *his[32];
	int n = 0;
	his[n++] = cable("Lav Mic", "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-000000000001}");      // Voice (kind)
	his[n++] = cable("1.2 Traveler", "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-000000000002}"); // Voice (kind)
	his[n++] = make("wasapi_output_capture", "1.5 Audio");                                         // Game (kind)
	his[n++] = make("game_capture", "6. Game Capture");                                            // Game (kind)
	his[n++] = make("window_capture", "Window Capture");
	his[n++] = make("wasapi_process_output_capture", "Eclipse");
	his[n++] = make("window_capture", "Window Capture 2");
	his[n++] = make("ffmpeg_source", "videoHorizontal");
	his[n++] = make("window_capture", "Contexto-WindowCapture");
	his[n++] = make("wasapi_process_output_capture", "Emulator2.9");
	his[n++] = make("wasapi_process_output_capture", "Glidex");
	his[n++] = make("ffmpeg_source", "Music-Lofi");                       // Music (kind + name)
	his[n++] = make("wasapi_process_output_capture", "9. Spotify");       // Music (kind + name)
	obs_source_t *aoc = his[n++] = make("wasapi_output_capture", "Audio Output Capture");
	place(aoc, "Music");
	for (int i = 0; i < n; i++) {
		const char *nm = obs_source_get_name(his[i]);
		bool voice = !strcmp(nm, "Lav Mic") || !strcmp(nm, "1.2 Traveler");
		bool music = !strcmp(nm, "Music-Lofi") || !strcmp(nm, "9. Spotify");
		if (his[i] != aoc)
			place(his[i], voice ? "Voice" : music ? "Music" : "Game");
	}
	his[n++] = make("ffmpeg_source", "StartingSoon-Music");
	his[n++] = make("ffmpeg_source", "StartingSoon-Music2");
	place(his[n - 2], "Music");
	place(his[n - 1], "Music");
	obs_source_t *b = his[n++] = cable("4. B - Virtual Cable (Track 1 Only)", "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-00000000000b}");
	obs_source_t *a = his[n++] = cable("5. A - Virtual Cable (All Tracks)", "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-00000000000a}");
	obs_source_t *vc1 = his[n++] = cable("Virtual Cable 1 (Track 1 Only)", "{0.0.1.00000000}.{b9a1c0de-0000-4000-8000-000000000010}");
	place(b, "");
	place(a, "");
	place(vc1, "");
	add_fillers(6); // the rest of "a long list"

	obs_source_t *c = recreate_c(NULL, "");
	dialog();
	result(strcmp(block("Voice"), "Lav Mic|1.2 Traveler") == 0 && in_block("Game", "6. Game Capture") && in_block("Game", "Glidex") && in_block("Music", "9. Spotify") &&
		       in_block("Music", "Audio Output Capture") && in_block("Music", "StartingSoon-Music2") && listed("4. B - Virtual Cable (Track 1 Only)") &&
		       listed("5. A - Virtual Cable (All Tracks)") && listed("Virtual Cable 1 (Track 1 Only)"),
	       "sharkey2: fixture matches his screenshot", "Voice [%s] Game [%s] Music [%s]", block("Voice"), block("Game"), block("Music"));

	// The panel as his screenshot shows it, at its visible size: "6. C" is listed wherever it sits.
	const char *placements[] = {"", "Music", "Voice", NULL};
	const char *label[] = {"Everything else", "Music", "Voice", "(never placed)"};
	for (int i = 0; i < 4; i++) {
		c = recreate_c(c, placements[i]);
		dialog();
		char name[96];
		snprintf(name, sizeof(name), "sharkey2: %d inputs, 6. C on %s is listed", audio_inputs(), label[i]);
		result(listed(CABLE_C), name, "found under: %s", where(CABLE_C));
	}

	// Names: "6. C - VC - MUSIC" and "6. Game Capture" share "6. ", and "4. B", "5. A" also start
	// with a number and a period. Both "6." sources on the same fader, and both in Everything else.
	obs_source_t *gc = his[3];
	c = recreate_c(c, "Game");
	dialog();
	result(in_block("Game", CABLE_C) && in_block("Game", "6. Game Capture"), "sharkey2: both \"6. \" sources listed on one fader", "Game [%s]", block("Game"));
	place(gc, "");
	c = recreate_c(c, "");
	dialog();
	result(listed(CABLE_C) && listed("6. Game Capture") && !in_block("Game", "6. Game Capture"), "sharkey2: both \"6. \" sources in Everything else", "C: %s, Game Capture: %s",
	       where(CABLE_C), where("6. Game Capture"));
	place(gc, "Game");

	// A long Everything else (no cap on its length): 6. C made last, as the 128th audio input.
	c = newest_c_at(c, 128, "");
	dialog();
	result(audio_inputs() == 128 && listed(CABLE_C), "sharkey2: 6. C is input 128, in Everything else: listed", "%d inputs; found under: %s", audio_inputs(), where(CABLE_C));

	// One more source made before it: 6. C is the 129th audio input. Wherever it is placed, and
	// with the cables made before it still on the Everything else line, it must be listed.
	for (int i = 0; i < 4; i++) {
		c = newest_c_at(c, 129, placements[i]);
		dialog();
		char name[96];
		snprintf(name, sizeof(name), "sharkey2: 6. C is input %d, on %s: listed", audio_inputs(), label[i]);
		result(listed(CABLE_C) && listed("5. A - Virtual Cable (All Tracks)") && listed("4. B - Virtual Cable (Track 1 Only)"), name, "found under: %s; 5. A: %s",
		       where(CABLE_C), where("5. A - Virtual Cable (All Tracks)"));
	}
	// His collection could be bigger still.
	c = newest_c_at(c, 200, "");
	dialog();
	result(audio_inputs() == 200 && listed(CABLE_C), "sharkey2: 6. C is input 200, in Everything else: listed", "%d inputs; found under: %s", audio_inputs(), where(CABLE_C));

	// The same number of inputs with 6. C made early (before the newer sources): listed even on
	// 0.4.0, so whether it shows depends on when it was made.
	obs_source_remove(c);
	obs_source_release(c);
	c = NULL;
	drop_fillers(n_fillers);
	c = recreate_c(NULL, "");
	add_fillers(129 - audio_inputs());
	dialog();
	result(audio_inputs() == 129 && listed(CABLE_C), "sharkey2: 129 inputs, 6. C made early: listed", "%d inputs; found under: %s", audio_inputs(), where(CABLE_C));

	// Ticked for Track 3: OBS tracks don't decide faders any more, so 6. C stays in Everything else
	// and is listed there as the 128th input, with no OBS Track row anywhere.
	c = newest_c_at(c, 128, "");
	obs_source_set_audio_mixers(c, (1 << 0) | (1 << 2));
	dialog();
	result(audio_inputs() == 128 && listed(CABLE_C) && !in_block("Music", CABLE_C) && !strstr(panel_rows, "OBS Track"), "sharkey2: 6. C is input 128, ticked for Track 3: in Everything else",
	       "found under: %s; Music [%.200s]", where(CABLE_C), block("Music"));

	for (int i = 0; i < n_fillers; i++)
		obs_source_release(fillers[i]);
	for (int i = 0; i < n; i++)
		obs_source_release(his[i]);
	obs_source_release(c);
	return panel_end(settings_source);
}
