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
// faders-panel-listing-voice: a fader with two or more sources lists each of them, including an
// Audio Input Capture of a virtual cable, and a second assignment keeps the first listed.
#include "panel-listing.h"

int main(void)
{
	obs_source_t *settings_source = panel_start("Voice,Game,Music");
	if (!settings_source)
		return 1;

	// Voice has exactly two sources: a mic and a virtual cable, both put on it (nothing is placed
	// by kind).
	obs_source_t *mic = cable("Mic", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000001}");
	place(mic, "Voice");
	obs_source_t *other = cable("Discord cable", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000002}");
	place(other, "Game");
	obs_source_t *music = cable("Music (VB-Cable)", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000003}");
	place(music, "Voice");
	dialog();
	result(in_block("Voice", "Mic") && in_block("Voice", "Music (VB-Cable)") && in_block("Game", "Discord cable") && !strstr(everything_else(), "Music (VB-Cable)"),
	       "voice: Voice lists both of its sources", "Voice [%s] Game [%s]", block("Voice"), block("Game"));

	// Two cables added to Voice through its Add dropdown, one after the other.
	place(music, "");
	obs_source_t *music2 = cable("Music 2 (VoiceMeeter)", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000004}");
	place(music2, "");
	dialog();
	bool a = pick("add:0", "Music (VB-Cable)");
	dialog();
	bool b = pick("add:0", "Music 2 (VoiceMeeter)");
	dialog();
	result(a && b && in_block("Voice", "Mic") && in_block("Voice", "Music (VB-Cable)") && in_block("Voice", "Music 2 (VoiceMeeter)") && strcmp(placed(music), "Voice") == 0 &&
		       strcmp(placed(music2), "Voice") == 0,
	       "voice: a second assignment keeps the first listed", "picked %d %d; Voice [%s]; stored %s, %s", a, b, block("Voice"), placed(music), placed(music2));

	obs_source_release(mic);
	obs_source_release(other);
	obs_source_release(music);
	obs_source_release(music2);
	return panel_end(settings_source);
}
