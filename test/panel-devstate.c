// SPDX-License-Identifier: GPL-2.0-or-later
// Real Faders properties and libobs, with stand-ins for Windows capture backends.
#include <obs.h>
#include <obs-module.h>
#include <sys/stat.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CABLE "6. C - VC - MUSIC"
#define ENDPOINT "{0.0.1.00000000}.{6f1c0a3e-0000-4000-8000-000000000001}"

static const char *faders[] = {"Voice", "Game", "Music"};
static struct {
	const char *name, *id;
	int fader;
	uint32_t tracks;
} fixture[] = {
	{"Lav Mic", "wasapi_input_capture", 0, 0x3f},
	{"1.2 Traveler", "wasapi_input_capture", 0, 0x3f},
	{"1.5 Audio", "wasapi_process_output_capture", 1, 0x3f},
	{"6. Game Capture", "game_capture", 1, 0x3f},
	{"Window Capture", "window_capture", 1, 0x3f},
	{"Eclipse", "game_capture", 1, 0x3f},
	{"Window Capture 2", "window_capture", 1, 0x3f},
	{"videoHorizontal", "window_capture", 1, 0x3f},
	{"Contexto-WindowCapture", "window_capture", 1, 0x3f},
	{"Emulator2.9", "game_capture", 1, 0x3f},
	{"Glidex", "window_capture", 1, 0x3f},
	{"Music-Lofi", "ffmpeg_source", 2, 0x3f},
	{"9. Spotify", "wasapi_process_output_capture", 2, 0x3f},
	{"Audio Output Capture", "wasapi_output_capture", 2, 0x3f},
	{"StartingSoon-Music", "ffmpeg_source", 2, 0x3f},
	{"StartingSoon-Music2", "ffmpeg_source", 2, 0x3f},
	{"4. B - Virtual Cable (Track 1 Only)", "wasapi_input_capture", -1, 1},
	{"5. A - Virtual Cable (All Tracks)", "wasapi_input_capture", -1, 0x3f},
	{"Virtual Cable 1 (Track 1 Only)", "wasapi_input_capture", -1, 1},
	{"Browser overlay", "browser_source", -1, 1},
};

static int failures;
static volatile bool reload_requested;

static void result(bool ok, const char *name, const char *detail)
{
	printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
	failures += !ok;
}

static const char *capture_name(void *unused) { return "Capture stand-in"; }
static void *capture_create(obs_data_t *settings, obs_source_t *source)
{
	return obs_data_get_bool(settings, "fail_backend") ? NULL : source;
}
static void capture_destroy(void *unused) {}
static uint32_t capture_dimension(void *unused) { return 320; }

static void register_kind(const char *id, enum obs_source_type type, uint32_t flags)
{
	struct obs_source_info info = {
		.id = id, .type = type, .output_flags = flags, .get_name = capture_name,
		.create = capture_create, .destroy = capture_destroy,
		.get_width = capture_dimension, .get_height = capture_dimension,
	};
	obs_register_source(&info);
}

static obs_source_t *create_input(const char *id, const char *name, const char *device, bool private, bool fail_backend)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", device);
	obs_data_set_bool(settings, "capture_audio", true);
	obs_data_set_bool(settings, "fail_backend", fail_backend);
	obs_source_t *source = private ? obs_source_create_private(id, name, settings) : obs_source_create(id, name, settings, NULL);
	obs_data_release(settings);
	if (!source)
		exit(1);
	return source;
}

static void place(obs_source_t *source, const char *fader)
{
	obs_data_t *settings = obs_source_get_private_settings(source);
	if (fader)
		obs_data_set_string(settings, "faders_fader", fader);
	else
		obs_data_unset_user_value(settings, "faders_fader");
	obs_data_release(settings);
}

static bool find_settings(void *param, obs_source_t *source)
{
	if (strcmp(obs_source_get_id(source), "faders_uplink") != 0)
		return true;
	*(obs_source_t **)param = obs_source_get_ref(source);
	return false;
}

static bool selected(obs_property_t *p, const char *name)
{
	const char *key = obs_property_name(p);
	return strncmp(key, "row:", 4) == 0 && strcmp(obs_property_list_item_name(p, 0), name) == 0;
}

// Check what the real panel shows. Add choices count separately from visible assignments.
static const char *where(obs_properties_t *props, const char *name, bool include_choices)
{
	obs_property_t *sources = obs_properties_get(props, "sources_group");
	obs_properties_t *group = obs_property_group_content(sources);
	obs_property_t *rest = obs_properties_get(group, "rest_info");
	if (rest && strstr(obs_property_description(rest), name))
		return "Everything else";
	for (obs_property_t *p = obs_properties_first(group); p; obs_property_next(&p)) {
		if (obs_property_get_type(p) != OBS_PROPERTY_LIST) {
			if (strncmp(obs_property_name(p), "track_members:", 14) == 0 && strstr(obs_property_description(p), name))
				return "track members";
			continue;
		}
		if (selected(p, name))
			return "fader row";
	}
	if (include_choices) {
		obs_property_t *add = obs_properties_get(group, "add:0");
		for (size_t i = 0; add && i < obs_property_list_item_count(add); i++)
			if (strcmp(obs_property_list_item_name(add, i), name) == 0)
				return "Add choices";
	}
	return NULL;
}

static void listed(obs_source_t *settings, const char *scenario, const char *expected)
{
	obs_properties_t *props = obs_source_properties(settings);
	const char *location = where(props, CABLE, true);
	result(expected ? location && strcmp(location, expected) == 0 : !location, scenario, location ? location : "absent from every list");
	obs_properties_destroy(props);
}

static void request_reload(void *unused, calldata_t *cd)
{
	os_atomic_set_bool(&reload_requested, true);
}

// OBS normally dispatches redraw_window through Qt. Dispatch the signal here and
// rebuild on the test thread only if the plugin signals the open properties view.
static void dispatch_signal(obs_task_t task, void *param, bool wait)
{
	task(param);
}

static void check_open(obs_source_t *settings, obs_properties_t **open, const char *scenario)
{
	for (int frame = 0; frame < 60; ++frame) {
		if (os_atomic_exchange_bool(&reload_requested, false)) {
			obs_properties_destroy(*open);
			*open = obs_source_properties(settings);
		}
		if (where(*open, CABLE, false))
			break;
		os_sleep_ms(10);
	}
	const char *location = where(*open, CABLE, true);
	result(where(*open, CABLE, false) != NULL, scenario, location ? location : CABLE " absent from every list");
}

static void set_track(obs_source_t *settings, const char *fader)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "track:2", fader);
	obs_source_update(settings, data);
	obs_data_release(data);
}

int main(void)
{
	char config_dir[1024];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = getenv("TEMP");
	snprintf(config_dir, sizeof(config_dir), "%s/faders-panel-devstate-%llu", tmp ? tmp : ".", (unsigned long long)os_gettime_ns());
	if (!obs_startup("en-US", config_dir, NULL))
		return 1;
	obs_set_ui_task_handler(dispatch_signal);
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	struct obs_video_info vi = {
		.graphics_module = FADERS_GRAPHICS_MODULE, .fps_num = 30, .fps_den = 1,
		.base_width = 320, .base_height = 180, .output_width = 320, .output_height = 180,
		.output_format = VIDEO_FORMAT_NV12, .colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL,
	};
	obs_module_t *module = NULL;
	if (!obs_reset_audio(&ai) || obs_reset_video(&vi) != OBS_VIDEO_SUCCESS ||
	    obs_open_module(&module, FADERS_PLUGIN_BIN, FADERS_PLUGIN_DATA) != MODULE_SUCCESS || !obs_init_module(module))
		return 1;
	register_kind("wasapi_input_capture", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE);
	register_kind("wasapi_output_capture", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR);
	register_kind("wasapi_process_output_capture", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR);
	register_kind("game_capture", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO);
	register_kind("window_capture", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO);
	register_kind("ffmpeg_source", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO);
	register_kind("browser_source", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO);
	register_kind("test_audio_filter", OBS_SOURCE_TYPE_FILTER, OBS_SOURCE_AUDIO);
	register_kind("test_video_only", OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_VIDEO);
	obs_source_t *settings = NULL;
	obs_enum_all_sources(find_settings, &settings);
	if (!settings)
		return 1;

	obs_scene_t *live = obs_scene_create("Live");
	obs_set_output_source(0, obs_scene_get_source(live));
	for (size_t i = 0; i < sizeof(fixture) / sizeof(fixture[0]); ++i) {
		const char *device = fixture[i].fader < 0 && strcmp(fixture[i].id, "wasapi_input_capture") == 0 ? ENDPOINT : "";
		int fader = fixture[i].fader;
		obs_source_t *source = create_input(fixture[i].id, fixture[i].name, device, false, false);
		obs_source_set_audio_mixers(source, fixture[i].tracks);
		place(source, fader < 0 ? "" : faders[fader]);
		obs_scene_add(live, source);
		obs_source_release(source);
	}

	// There is no refresh from a hub or a streaming status change in this fixture.
	// Let OBS finish notifications from the collection before opening the window.
	os_sleep_ms(150);
	signal_handler_t *signals = obs_source_get_signal_handler(settings);
	signal_handler_connect(signals, "update_properties", request_reload, NULL);
	obs_properties_t *open = obs_source_properties(settings);
	bool fixture_listed = true;
	for (size_t i = 0; i < sizeof(fixture) / sizeof(fixture[0]); ++i) {
		const char *location = where(open, fixture[i].name, false);
		const char *expected = fixture[i].fader < 0 ? "Everything else" : "fader row";
		fixture_listed = fixture_listed && location && strcmp(location, expected) == 0;
	}
	result(fixture_listed && !where(open, CABLE, true), "reported scene collection", "all fixture sources are listed before adding the missing cable");
	obs_source_t *cable = create_input("wasapi_input_capture", CABLE, ENDPOINT, false, false);
	check_open(settings, &open, "panel_refreshes_after_source_creation");
	obs_properties_destroy(open);
	listed(settings, "public cable without assignment (existing Voice default)", "fader row");

	obs_source_set_name(cable, "Previous cable name");
	place(cable, "Music");
	os_sleep_ms(150);
	os_atomic_set_bool(&reload_requested, false);
	open = obs_source_properties(settings);
	obs_source_set_name(cable, CABLE);
	check_open(settings, &open, "panel_refreshes_after_source_rename");
	obs_properties_destroy(open);
	listed(settings, "renamed cable in a freshly opened panel", "fader row");
	signal_handler_disconnect(signals, "update_properties", request_reload, NULL);

	listed(settings, "same device as all three siblings and no scene", "fader row");
	obs_source_t *duplicate = obs_source_duplicate(cable, "Copy of cable", false);
	result(duplicate == cable, "OBS source duplication flag", "WASAPI duplicate returns a reference to the same source");
	obs_source_release(duplicate);
	obs_sceneitem_t *first = obs_scene_add(live, cable);
	obs_sceneitem_t *second = obs_scene_add(live, cable);
	listed(settings, "same source pointer used in two scene items", "fader row");
	obs_sceneitem_remove(first);
	obs_sceneitem_remove(second);
	for (uint32_t mask = 0; mask < 64; ++mask) {
		obs_source_set_audio_mixers(cable, mask);
		obs_properties_t *props = obs_source_properties(settings);
		const char *location = where(props, CABLE, false);
		obs_properties_destroy(props);
		if (!location || strcmp(location, "fader row") != 0) {
			printf("FAIL test_all_six_bit_masks: missing with mask 0x%x\n", mask);
			failures++;
			break;
		}
		if (mask == 63)
			result(true, "test_all_six_bit_masks", "all 64 masks keep the direct fader row");
	}

	place(cable, "");
	listed(settings, "all six tracks and no track assigned to a fader", "Everything else");
	set_track(settings, "Game");
	obs_properties_t *tracked = obs_source_properties(settings);
	const char *location = where(tracked, CABLE, true);
	result(location && (strcmp(location, "Add choices") == 0 || strcmp(location, "track members") == 0),
	       "track-only route", location ? location : "absent from every list");
	obs_properties_destroy(tracked);
	obs_source_set_audio_mixers(cable, 1);
	listed(settings, "Track 1 only (stream track), Track 2 assigned to Game", "Everything else");
	obs_source_set_audio_mixers(cable, 0);
	listed(settings, "zero track mask, Track 2 assigned to Game", "Everything else");
	set_track(settings, "~none");

	obs_scene_t *inactive = obs_scene_create("Inactive");
	obs_scene_t *nested = obs_scene_create("Nested");
	obs_scene_add(inactive, obs_scene_get_source(nested));
	obs_sceneitem_t *group = obs_scene_add_group(nested, "Hidden group");
	obs_sceneitem_t *item = obs_scene_add(obs_sceneitem_group_get_scene(group), cable);
	obs_sceneitem_set_visible(item, false);
	obs_source_set_monitoring_type(cable, OBS_MONITORING_TYPE_MONITOR_ONLY);
	obs_source_set_muted(cable, true);
	obs_source_set_enabled(cable, false);
	obs_source_set_volume(cable, 0);
	obs_source_set_audio_active(cable, false);
	place(cable, "Music");
	obs_source_t *filter = obs_source_create_private("test_audio_filter", "Filter", NULL);
	obs_source_filter_add(cable, filter);
	obs_source_release(filter);
	result(obs_source_get_monitoring_type(cable) == OBS_MONITORING_TYPE_MONITOR_ONLY && !obs_source_audio_active(cable),
	       "combined state applied", "monitor only and audio inactive");
	listed(settings, "combined mute, disabled, hidden nested group, monitor only, zero gain, inactive, filter, zero tracks", "fader row");

	obs_sceneitem_remove(item);
	obs_weak_source_t *weak = obs_source_get_weak_source(cable);
	listed(settings, "public cable held by a strong reference", "fader row");
	obs_source_release(cable);
	obs_wait_for_destroy_queue();
	result(obs_weak_source_expired(weak), "weak reference after final strong release", "source lifetime ended");
	obs_weak_source_release(weak);
	listed(settings, "destroyed cable", NULL);

	cable = create_input("wasapi_input_capture", CABLE, ENDPOINT, true, false);
	obs_set_output_source(1, cable);
	listed(settings, "actual libobs-private input even on an output channel", NULL);
	obs_set_output_source(1, NULL);
	obs_source_release(cable);
	cable = create_input("wasapi_input_capture", CABLE, ENDPOINT, false, true);
	listed(settings, "registered WASAPI kind, backend creation fails", "fader row");
	obs_source_release(cable);
	cable = create_input("test_video_only", CABLE, ENDPOINT, false, false);
	obs_source_set_audio_active(cable, true);
	listed(settings, "source type without OBS_SOURCE_AUDIO, audio_active true", NULL);
	obs_source_release(cable);

	obs_set_output_source(0, NULL);
	obs_scene_t *scenes[] = {inactive, nested, live};
	for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
		obs_source_remove(obs_scene_get_source(scenes[i]));
		obs_scene_release(scenes[i]);
	}
	obs_source_release(settings);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return failures ? 1 : 0;
}
