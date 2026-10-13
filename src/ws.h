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
// Minimal WebSocket client used by the uplink. One implementation per platform:
// ws-apple.m (NSURLSessionWebSocketTask). Windows (WinHTTP) and Linux (libcurl) are TODO.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fdr_ws fdr_ws_t;

typedef struct fdr_ws_callbacks {
	void (*on_open)(void *param);
	void (*on_text)(void *param, const char *text);
	// Called once when the connection fails or closes. `reason` is human-readable.
	void (*on_close)(void *param, const char *reason);
} fdr_ws_callbacks;

// Starts connecting; callbacks fire on a background thread. `bearer` may be NULL.
fdr_ws_t *fdr_ws_connect(const char *url, const char *bearer, const fdr_ws_callbacks *cb, void *param);
// Queues a binary message. Returns false if the socket isn't open or too much is queued.
bool fdr_ws_send_binary(fdr_ws_t *ws, const uint8_t *data, size_t len);
// Closes (if needed) and frees. No callbacks fire after this returns.
void fdr_ws_destroy(fdr_ws_t *ws);

#ifdef __cplusplus
}
#endif
