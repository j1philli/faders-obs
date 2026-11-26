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
// faders-panel-listing-cancel: Cancel in the properties window (or "Discard" when closing it)
// after a change in "What's on each fader" moves nothing else.
//
// OBS keeps a copy of the settings as they were when the window opened. Cancel clears the
// settings and updates the source with that copy (OBSBasicProperties::on_buttonBox_clicked);
// Discard and Edit → Undo do the same. The copy holds the dropdowns as they were drawn then, so
// a row whose dropdown now shows a different source must not read as the streamer picking it.
#include "panel-listing.h"

int main(void)
{
	obs_source_t *settings_source = panel_start("Voice,Game,Music");
	if (!settings_source)
		return 1;

	// Voice has two sources (a mic and a music cable, both put on it), Game one.
	obs_source_t *mic = cable("Mic", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000001}");
	place(mic, "Voice");
	obs_source_t *other = cable("Discord cable", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000002}");
	place(other, "Game");
	obs_source_t *music = cable("Music (VB-Cable)", "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000003}");
	place(music, "Voice");

	dialog(); // the window as last drawn: the settings hold its dropdowns
	obs_data_t *at_open = obs_data_create();
	obs_data_t *cur = obs_source_get_settings(settings_source);
	obs_data_apply(at_open, cur); // OBS's copy, taken as the window opens
	dialog();                     // the window opens
	char before[512];
	snprintf(before, sizeof(before), "Voice [%s] Game [%s]", block("Voice"), block("Game"));
	// Take the mic off Voice: the music cable's row moves up into the mic's place.
	bool removed = pick(key_showing("Mic"), "Remove from Voice");
	dialog(); // redrawn
	char after_remove[512];
	snprintf(after_remove, sizeof(after_remove), "Voice [%s]", block("Voice"));
	// Cancel.
	obs_data_clear(cur);
	obs_source_update(settings_source, at_open);
	obs_data_release(cur);
	obs_data_release(at_open);
	dialog();
	result(removed && strcmp(placed(music), "Voice") == 0 && strcmp(placed(other), "Game") == 0 && in_block("Voice", "Music (VB-Cable)") && in_block("Game", "Discord cable"),
	       "cancel: Cancel after a removal moves nothing else", "before %s; after removing Mic %s; after Cancel Voice [%s] Game [%s]; stored: Mic %s, Music %s, Discord cable %s", before, after_remove,
	       block("Voice"), block("Game"), placed(mic), placed(music), placed(other));

	obs_source_release(mic);
	obs_source_release(other);
	obs_source_release(music);
	return panel_end(settings_source);
}
