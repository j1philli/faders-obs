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
// Headless test host for the Faders OBS plugin: real libobs (from OBS.app), no GUI.
// Plays three WAV files into OBS mixer tracks 2/3/4, loads faders.plugin, configures it via
// its "faders_configure" procedure, and prints the plugin's status every few seconds.
//
//   faders-headless <server> <key> <voice.wav> <game.wav> <music.wav> [seconds]
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── A minimal looping WAV source (16-bit PCM, mono, 48 kHz — what media/*.wav are) ───────
struct wav_src {
	obs_source_t *source;
	int16_t *samples;
	size_t count;
	size_t pos;
	pthread_t thread;
	volatile bool stop;
};

static bool load_wav(const char *path, int16_t **out, size_t *count)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	char id[4];
	uint32_t size;
	fseek(f, 12, SEEK_SET); // RIFF header
	while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
		if (!memcmp(id, "data", 4)) {
			*out = malloc(size);
			*count = fread(*out, 2, size / 2, f);
			fclose(f);
			return true;
		}
		fseek(f, size + (size & 1), SEEK_CUR);
	}
	fclose(f);
	return false;
}

static void *wav_thread(void *param)
{
	struct wav_src *w = param;
	const size_t block = 480; // 10 ms
	uint64_t t0 = os_gettime_ns();
	for (uint64_t n = 0; !w->stop; n++) {
		int16_t buf[480];
		for (size_t i = 0; i < block; i++) {
			buf[i] = w->samples[w->pos];
			w->pos = (w->pos + 1) % w->count;
		}
		struct obs_source_audio a = {
			.data = {(uint8_t *)buf},
			.frames = block,
			.speakers = SPEAKERS_MONO,
			.format = AUDIO_FORMAT_16BIT,
			.samples_per_sec = 48000,
			.timestamp = t0 + n * 10000000ULL,
		};
		obs_source_output_audio(w->source, &a);
		os_sleepto_ns(t0 + (n + 1) * 10000000ULL);
	}
	return NULL;
}

static const char *wav_name(void *u) { return "WAV loop"; }
static void *wav_create(obs_data_t *settings, obs_source_t *source)
{
	struct wav_src *w = bzalloc(sizeof(*w));
	w->source = source;
	if (!load_wav(obs_data_get_string(settings, "path"), &w->samples, &w->count)) {
		fprintf(stderr, "cannot read %s\n", obs_data_get_string(settings, "path"));
		exit(2);
	}
	pthread_create(&w->thread, NULL, wav_thread, w);
	return w;
}
static void wav_destroy(void *data)
{
	struct wav_src *w = data;
	w->stop = true;
	pthread_join(w->thread, NULL);
	free(w->samples);
	bfree(w);
}
static struct obs_source_info wav_info = {
	.id = "faders_test_wav",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = wav_name,
	.create = wav_create,
	.destroy = wav_destroy,
};

// Debug: RMS of what reaches each mixer track (tracks 2-4), to tell host problems from plugin ones.
static double track_rms[6];
static void on_track_audio(void *param, size_t mix_idx, struct audio_data *data)
{
	const float *f = (const float *)data->data[0];
	double acc = 0;
	for (uint32_t i = 0; i < data->frames; i++)
		acc += f[i] * f[i];
	double rms = data->frames ? sqrt(acc / data->frames) : 0;
	track_rms[mix_idx] = track_rms[mix_idx] * 0.9 + rms * 0.1;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s <server> <key> <voice.wav> <game.wav> <music.wav> [seconds]\n", argv[0]);
		return 1;
	}
	int seconds = argc > 6 ? atoi(argv[6]) : 30;

	// Keep the plugin's saved settings (which include the key) out of the working directory.
	char config_dir[512];
	const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : (getenv("TEMP") ? getenv("TEMP") : "/tmp");
	snprintf(config_dir, sizeof(config_dir), "%s/faders-headless-config", tmp);
	if (!obs_startup("en-US", config_dir, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}
	// libobs only mixes audio from sources in a video mix (obs-audio.c builds its render list
	// from obs->video.mixes), so even an audio test needs a minimal video pipeline.
	struct obs_video_info ovi = {
		.graphics_module = FADERS_GRAPHICS_MODULE,
		.fps_num = 30,
		.fps_den = 1,
		.base_width = 320,
		.base_height = 180,
		.output_width = 320,
		.output_height = 180,
		.output_format = VIDEO_FORMAT_NV12,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
		.gpu_conversion = true,
	};
	int vret = obs_reset_video(&ovi);
	if (vret != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed (%d)\n", vret);
		return 1;
	}
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai)) {
		fprintf(stderr, "obs_reset_audio failed\n");
		return 1;
	}
	obs_register_source(&wav_info);

	// Voice → track 2, game → track 3, music → track 4 (mixer bit = track - 1).
	obs_source_t *srcs[3];
	for (int i = 0; i < 3; i++) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "path", argv[3 + i]);
		static const char *names[] = {"voice", "game", "music"};
		srcs[i] = obs_source_create("faders_test_wav", names[i], s, NULL);
		obs_data_release(s);
		obs_source_set_audio_mixers(srcs[i], 1u << (i + 1));
		obs_set_output_source(1 + i, srcs[i]); // output channels make the sources active
	}

	struct audio_convert_info dbg = {.samples_per_sec = 48000, .format = AUDIO_FORMAT_FLOAT, .speakers = SPEAKERS_MONO};
	for (size_t m = 1; m <= 3; m++)
		obs_add_raw_audio_callback(m, &dbg, on_track_audio, NULL);

	obs_module_t *mod = NULL;
	const char *bin = FADERS_PLUGIN_BIN;
	const char *data = FADERS_PLUGIN_DATA;
	if (obs_open_module(&mod, bin, data) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", bin);
		return 1;
	}

	calldata_t cd = {0};
	calldata_set_string(&cd, "server", argv[1]);
	calldata_set_string(&cd, "key", argv[2]);
	calldata_set_int(&cd, "voice", 2);
	calldata_set_int(&cd, "game", 3);
	calldata_set_int(&cd, "music", 4);
	calldata_set_bool(&cd, "only_live", false);
	proc_handler_call(obs_get_proc_handler(), "faders_configure", &cd);
	// FADERS_STREAM_TRACK=n: run the streamed-track silence check on track n (no streaming output here).
	if (getenv("FADERS_STREAM_TRACK")) {
		calldata_t wc = {0};
		calldata_set_int(&wc, "track", atoi(getenv("FADERS_STREAM_TRACK")));
		proc_handler_call(obs_get_proc_handler(), "faders_watch_stream_track", &wc);
		calldata_free(&wc);
	}
	calldata_free(&cd);

	for (int t = 0; t < seconds; t++) {
		os_sleep_ms(1000);
		if (t % 3 == 2) {
			calldata_t st = {0};
			proc_handler_call(obs_get_proc_handler(), "faders_status", &st);
			printf("[%2ds] %s  (track rms: 2=%.4f 3=%.4f 4=%.4f)\n", t + 1, calldata_string(&st, "status"), track_rms[1], track_rms[2], track_rms[3]);
			fflush(stdout);
			calldata_free(&st);
		}
	}

	for (int i = 0; i < 3; i++) {
		obs_set_output_source(1 + i, NULL);
		obs_source_release(srcs[i]);
	}
	obs_shutdown();
	return 0;
}
