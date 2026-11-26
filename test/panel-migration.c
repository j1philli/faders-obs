// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise saved OBS source and plugin settings through the real Tools → Faders properties.
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>

static const char *input_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Audio Input Capture (test)";
}

static void *input_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void input_destroy(void *unused)
{
	UNUSED_PARAMETER(unused);
}

#define VOICE_MIXERS ((1 << 0) | (1 << 4)) // streamed Track 1 and Voice Track 5

static obs_source_t *load_input(const char *name, const char *id, const char *device, const char *direct)
{
	obs_data_t *data = obs_data_create();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", device);
	obs_data_set_obj(data, "settings", settings);
	obs_data_release(settings);
	obs_data_set_string(data, "name", name);
	obs_data_set_string(data, "id", id);
	obs_data_set_string(data, "versioned_id", id);
	obs_data_set_int(data, "mixers", VOICE_MIXERS);
	if (direct) {
		obs_data_t *priv = obs_data_create();
		obs_data_set_string(priv, "faders_fader", direct);
		obs_data_set_obj(data, "private_settings", priv);
		obs_data_release(priv);
	}
	obs_source_t *source = obs_load_source(data);
	obs_data_release(data);
	return source;
}

static bool check_save(obs_module_t *module, obs_source_t *cable)
{
	calldata_t cd = {0};
	calldata_set_string(&cd, "key", "add:0");
	calldata_set_string(&cd, "choice", "Cable Music");
	bool picked = proc_handler_call(obs_get_proc_handler(), "faders_dialog_set", &cd) && calldata_bool(&cd, "ok");
	calldata_free(&cd);

	obs_data_t *saved = obs_save_source(cable);
	obs_data_t *priv = obs_data_get_obj(saved, "private_settings");
	char *path = obs_module_get_config_path(module, "settings.json");
	char *file = os_quick_read_utf8_file(path);
	// The direct placement and the source's own track ticks are saved; the old track route isn't.
	bool kept = picked && strcmp(obs_data_get_string(priv, "faders_fader"), "Voice") == 0 && obs_data_get_int(saved, "mixers") == VOICE_MIXERS && file && !strstr(file, "track:");
	printf("%s panel-migration-save: direct Voice placement and track ticks round-trip; no track:N in settings.json\n", kept ? "PASS" : "FAIL");
	bfree(file);
	bfree(path);
	obs_data_release(priv);
	obs_data_release(saved);
	return kept;
}

// Settings from earlier versions: 0.3.1's track_voice, 0.4.0's track:5 → Voice, and the cable placed
// directly ("legacy": never placed; "track-only": in Everything else; "map-and-track": on Voice).
// Tracks place nothing now: the mic and an unplaced cable are NEW in Everything else, and only a
// direct placement puts the cable on Voice.
int test_panel_migration(const char *config_dir, const char *scenario)
{
	bool legacy = strcmp(scenario, "legacy") == 0;
	bool both = strcmp(scenario, "map-and-track") == 0;
	struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_startup("en-US", config_dir, NULL))
		return 1;
	if (!obs_reset_audio(&audio)) {
		obs_shutdown();
		return 1;
	}

	const struct obs_source_info wasapi = {
		.id = "wasapi_input_capture", .type = OBS_SOURCE_TYPE_INPUT, .output_flags = OBS_SOURCE_AUDIO,
		.get_name = input_name, .create = input_create, .destroy = input_destroy,
	};
	const struct obs_source_info coreaudio = {
		.id = "coreaudio_input_capture", .type = OBS_SOURCE_TYPE_INPUT, .output_flags = OBS_SOURCE_AUDIO,
		.get_name = input_name, .create = input_create, .destroy = input_destroy,
	};
	obs_register_source(&wasapi);
	obs_register_source(&coreaudio);
	const char *id = both ? coreaudio.id : wasapi.id;
	obs_source_t *mic = load_input("Microphone", id, "mic", NULL);
	obs_source_t *cable = load_input("Cable Music", id, "virtual-cable", legacy ? NULL : both ? "Voice" : "");

	obs_module_t *module = NULL;
	bool ready = mic && cable && obs_open_module(&module, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) == MODULE_SUCCESS;
	if (ready) {
		char *dir = obs_module_get_config_path(module, "");
		char *path = obs_module_get_config_path(module, "settings.json");
		obs_data_t *saved = obs_data_create_from_json("{\"enabled\":false,\"track_voice\":5}");
		if (!legacy)
			obs_data_set_string(saved, "track:5", "Voice");
		ready = os_mkdirs(dir) != MKDIR_ERROR && obs_data_save_json(saved, path) && obs_init_module(module);
		obs_data_release(saved);
		bfree(path);
		bfree(dir);
	}

	calldata_t cd = {0};
	const char *rows = NULL;
	if (ready && proc_handler_call(obs_get_proc_handler(), "faders_dialog", &cd))
		rows = calldata_string(&cd, "rows");
	bool cable_on_voice = rows && strstr(rows, "\tCable Music\tCable Music|Remove from Voice");
	bool any_track = rows && (strstr(rows, "OBS Track") || strstr(rows, "From OBS tracks"));
	bool mic_new = rows && strstr(rows, "Microphone (new)") && !strstr(rows, "\tMicrophone\tMicrophone|");
	bool cable_rest = rows && (strstr(rows, "Cable Music.") || strstr(rows, "Cable Music,")) && !strstr(rows, "Cable Music (new)");
	bool cable_new = rows && strstr(rows, "Cable Music (new)");
	bool pass = rows && !any_track && mic_new && (both ? cable_on_voice : legacy ? cable_new : cable_rest);
	printf("%s panel-migration-%s: mic NEW=%d, cable on Voice=%d / placed in Everything else=%d / NEW=%d, OBS track rows=%d\n", pass ? "PASS" : "FAIL", scenario, mic_new,
	       cable_on_voice, cable_rest, cable_new, any_track);
	if (!pass && rows)
		fputs(rows, stdout);
	calldata_free(&cd);
	if (pass && !legacy && !both)
		pass = check_save(module, cable);
	obs_source_release(cable);
	obs_source_release(mic);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return pass ? 0 : 1;
}
