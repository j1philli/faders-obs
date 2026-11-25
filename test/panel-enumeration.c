// SPDX-License-Identifier: GPL-2.0-or-later
// Called by the real libobs mixtest host after the plugin loads. Only the capture
// backend is synthetic. Use the IDs and flags OBS registers for device captures.
#include <obs-module.h>
#include <stdio.h>
#include <string.h>
#include "../src/mixer.h"

static const char *name(void *unused) { return "Cable"; }
static void *create(obs_data_t *settings, obs_source_t *source) { return source; }
static void destroy(void *unused) {}

static void set_fader(obs_source_t *source, const char *fader)
{
	obs_data_t *settings = obs_source_get_private_settings(source);
	obs_data_set_string(settings, FDR_FADER_SETTING, fader);
	obs_data_release(settings);
}

// Count selected rows on Voice, ignoring the choices in each dropdown.
static int check(const char *scenario, int expected_game_inputs)
{
	calldata_t cd = {0};
	proc_handler_call(obs_get_proc_handler(), "faders_dialog", &cd);
	const char *rows = calldata_string(&cd, "rows");
	int mic = 0, cable = 0, game_inputs = 0;
	bool track_offered = rows && strstr(rows, "OBS Track 6 (everything ticked for it)");
	bool voice = false;
	for (const char *line = rows; line && *line;) {
		const char *end = strchr(line, '\n');
		if (!end)
			break;
		if (strncmp(line, "row:", 4) == 0) {
			const char *label = strchr(line, '\t') + 1;
			if (*label != ' ')
				voice = strncmp(label, "Voice\t", 6) == 0;
			const char *shown = strchr(label, '\t') + 1;
			game_inputs += strncmp(shown, "Other input ", 12) == 0;
			if (voice) {
				mic += strncmp(shown, "Control mic\t", 12) == 0;
				cable += strncmp(shown, "Music cable\t", 12) == 0;
			}
		}
		line = end + 1;
	}
	calldata_free(&cd);
	bool ok = mic == 1 && cable == 1 && game_inputs == expected_game_inputs && track_offered;
	printf("%s  %s: Voice rows mic=%d cable=%d (want 1 each), other inputs=%d/%d, track 6 offered=%d\n",
	       ok ? "PASS" : "FAIL", scenario, mic, cable, game_inputs, expected_game_inputs, track_offered);
	return !ok;
}

int panel_enumeration(void)
{
	struct obs_source_info input = {.id = "wasapi_input_capture", .type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE, .get_name = name, .create = create, .destroy = destroy};
	obs_register_source(&input);
	input.id = "coreaudio_input_capture";
	obs_register_source(&input);
	obs_source_t *mic = obs_source_create(input.id, "Control mic", NULL, NULL);
	obs_source_t *music = obs_source_create("wasapi_input_capture", "Music cable", NULL, NULL);
	set_fader(mic, "Voice");
	set_fader(music, "Voice");
	int failures = 0;
	for (int channel = 1; channel <= 6; channel++) {
		obs_source_t *old = obs_get_output_source(channel);
		obs_set_output_source(channel, music);
		char scenario[32];
		snprintf(scenario, sizeof(scenario), "global channel %d", channel);
		failures += check(scenario, 0);
		obs_set_output_source(channel, old);
		obs_source_release(old);
	}

	obs_scene_t *live = obs_scene_create("Live");
	obs_scene_t *other = obs_scene_create("Other");
	obs_scene_t *nested = obs_scene_create("Nested");
	obs_set_output_source(0, obs_scene_get_source(live));
	obs_scene_add(other, obs_scene_get_source(nested));
	obs_sceneitem_t *group = obs_scene_add_group(nested, "Group");
	obs_sceneitem_t *item = obs_scene_add(obs_sceneitem_group_get_scene(group), music);
	obs_sceneitem_set_visible(item, false);
	obs_source_set_muted(music, true);
	obs_source_set_enabled(music, false);
	obs_source_set_audio_active(music, false);
	obs_source_set_audio_mixers(music, 0);
	failures += check("hidden group input in inactive nested scene", 0);
	obs_set_output_source(0, NULL);
	obs_sceneitem_remove(item);
	obs_source_remove(obs_scene_get_source(live));
	obs_source_remove(obs_scene_get_source(other));
	obs_source_remove(obs_scene_get_source(nested));
	obs_scene_release(live);
	obs_scene_release(other);
	obs_scene_release(nested);
	obs_source_release(music);
	obs_wait_for_destroy_queue();

	// Place the cable after the former 128-item cutoff, within mixer capacity.
	obs_source_t *padding[249];
	for (size_t i = 0; i < 249; i++) {
		char label[32];
		snprintf(label, sizeof(label), "Other input %u", (unsigned)i);
		padding[i] = obs_source_create(input.id, label, NULL, NULL);
		set_fader(padding[i], "Game");
	}
	music = obs_source_create("wasapi_input_capture", "Music cable", NULL, NULL);
	set_fader(music, "Voice");
	failures += check("assigned input beyond 128", 249);
	obs_source_release(music);
	for (size_t i = 0; i < 249; i++)
		obs_source_release(padding[i]);
	obs_source_release(mic);
	return failures;
}
