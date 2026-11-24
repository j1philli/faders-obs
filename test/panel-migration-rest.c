// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs.h>
#include <string.h>

static void assign_track(int track, const char *fader)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "track", track);
	calldata_set_string(&cd, "fader", fader);
	proc_handler_call(obs_get_proc_handler(), "faders_assign_track", &cd);
	calldata_free(&cd);
}

// The earlier track resolves to the residual. The next one still carries this source to Voice.
bool test_panel_migration_rest(obs_source_t *source, const char *earlier_assignment)
{
	uint32_t original_mixers = obs_source_get_audio_mixers(source);
	obs_source_set_audio_mixers(source, (1 << 0) | (1 << 1) | (1 << 2));
	assign_track(2, earlier_assignment);
	assign_track(3, "Voice");

	calldata_t cd = {0};
	proc_handler_call(obs_get_proc_handler(), "faders_dialog", &cd);
	const char *rows = calldata_string(&cd, "rows");
	bool pass = rows && strstr(rows, "\tOBS Track 3 (everything ticked for it)\tOBS Track 3 (everything ticked for it)|Remove from Voice") &&
		    !strstr(rows, "rest_info\tEverything else: extra");
	calldata_free(&cd);

	assign_track(2, "none");
	assign_track(3, "none");
	obs_source_set_audio_mixers(source, original_mixers);
	return pass;
}
