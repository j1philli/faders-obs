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
// Windows WebSocket transport on WinHTTP: system TLS and proxy settings, no bundled libraries.
// Connecting and receiving run on a dedicated thread so fdr_ws_connect never blocks the uplink.
#include "ws.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fdr_ws {
	fdr_ws_callbacks cb;
	void *param;
	wchar_t *url;
	wchar_t *headers;

	CRITICAL_SECTION handle_lock; // guards the HINTERNET handles against destroy()
	CRITICAL_SECTION send_lock;   // WinHTTP allows one send at a time
	HINTERNET session, connection, request, socket;
	HANDLE thread;

	volatile LONG open;
	volatile LONG closed;    // on_close already delivered
	volatile LONG destroyed; // owner gone: deliver nothing
};

static wchar_t *widen(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = malloc(n * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static void fire_close(struct fdr_ws *ws, const char *reason)
{
	InterlockedExchange(&ws->open, 0);
	if (InterlockedCompareExchange(&ws->closed, 1, 0) != 0)
		return;
	if (!ws->destroyed && ws->cb.on_close)
		ws->cb.on_close(ws->param, reason);
}

// Stores a freshly created handle unless destroy() already ran; then closes it instead.
static bool keep_handle(struct fdr_ws *ws, HINTERNET *slot, HINTERNET h)
{
	EnterCriticalSection(&ws->handle_lock);
	bool ok = h && !ws->destroyed;
	if (ok)
		*slot = h;
	LeaveCriticalSection(&ws->handle_lock);
	if (h && !ok)
		WinHttpCloseHandle(h);
	return ok;
}

static DWORD WINAPI run(LPVOID param)
{
	struct fdr_ws *ws = param;
	char reason[160] = "connection failed";

	URL_COMPONENTS uc = {sizeof(uc)};
	wchar_t host[256], path[2048];
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	uc.dwExtraInfoLength = (DWORD)-1; // query string, appended to the path below
	if (!WinHttpCrackUrl(ws->url, 0, 0, &uc)) {
		fire_close(ws, "invalid server URL");
		return 0;
	}
	wchar_t target[4096];
	_snwprintf(target, 4096, L"%s%.*s", path, (int)uc.dwExtraInfoLength, uc.lpszExtraInfo ? uc.lpszExtraInfo : L"");
	target[4095] = 0;
	bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

	wchar_t agent[64];
	_snwprintf(agent, 64, L"faders-obs/%hs", FDR_VERSION_STR);
	if (!keep_handle(ws, &ws->session, WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)))
		goto done;
	// Connect/send/headers time out; WebSocket receives wait indefinitely (the hub is quiet).
	WinHttpSetTimeouts(ws->session, 10000, 10000, 10000, 0);
	if (!keep_handle(ws, &ws->connection, WinHttpConnect(ws->session, host, uc.nPort, 0)))
		goto done;
	if (!keep_handle(ws, &ws->request,
			 WinHttpOpenRequest(ws->connection, L"GET", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
					    secure ? WINHTTP_FLAG_SECURE : 0)))
		goto done;
	if (!WinHttpSetOption(ws->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0))
		goto done;
	if (!WinHttpSendRequest(ws->request, ws->headers ? ws->headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
				WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(ws->request, NULL)) {
		snprintf(reason, sizeof(reason), "could not reach server (WinHTTP error %lu)", GetLastError());
		goto done;
	}

	DWORD status = 0, size = sizeof(status);
	WinHttpQueryHeaders(ws->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
			    WINHTTP_NO_HEADER_INDEX);
	if (status != 101) {
		// e.g. 401 for a wrong key: same wording as the macOS transport.
		snprintf(reason, sizeof(reason), "server rejected connection (HTTP %lu)", status);
		goto done;
	}
	if (!keep_handle(ws, &ws->socket, WinHttpWebSocketCompleteUpgrade(ws->request, 0)))
		goto done;

	InterlockedExchange(&ws->open, 1);
	if (!ws->destroyed && ws->cb.on_open)
		ws->cb.on_open(ws->param);

	// Receive loop: the hub only sends small JSON text messages.
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	for (;;) {
		DWORD read = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
		if (len + 1024 > cap)
			buf = realloc(buf, cap *= 2);
		DWORD err = WinHttpWebSocketReceive(ws->socket, buf + len, (DWORD)(cap - len - 1), &read, &type);
		if (err != NO_ERROR) {
			snprintf(reason, sizeof(reason), "connection lost (WinHTTP error %lu)", err);
			break;
		}
		if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
			USHORT code = 0;
			DWORD rlen = 0;
			WinHttpWebSocketQueryCloseStatus(ws->socket, &code, NULL, 0, &rlen);
			snprintf(reason, sizeof(reason), "closed by server (%u)", code);
			break;
		}
		len += read;
		if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
			buf[len] = 0;
			if (!ws->destroyed && ws->cb.on_text)
				ws->cb.on_text(ws->param, buf);
			len = 0;
		} else if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
			len = 0; // not used by the protocol
		}
		// *_FRAGMENT_BUFFER_TYPE: keep accumulating.
	}
	free(buf);

done:
	fire_close(ws, reason);
	return 0;
}

fdr_ws_t *fdr_ws_connect(const char *url, const char *bearer, const fdr_ws_callbacks *cb, void *param)
{
	// WinHTTP only cracks http(s):// URLs; the WebSocket upgrade happens on the request.
	char http_url[2048];
	if (!strncmp(url, "wss://", 6))
		snprintf(http_url, sizeof(http_url), "https://%s", url + 6);
	else if (!strncmp(url, "ws://", 5))
		snprintf(http_url, sizeof(http_url), "http://%s", url + 5);
	else
		return NULL;

	struct fdr_ws *ws = calloc(1, sizeof(*ws));
	ws->cb = *cb;
	ws->param = param;
	ws->url = widen(http_url);
	if (bearer && *bearer) {
		char h[1024];
		snprintf(h, sizeof(h), "Authorization: Bearer %s\r\n", bearer);
		ws->headers = widen(h);
	}
	InitializeCriticalSection(&ws->handle_lock);
	InitializeCriticalSection(&ws->send_lock);
	ws->thread = CreateThread(NULL, 0, run, ws, 0, NULL);
	return ws;
}

bool fdr_ws_send_binary(fdr_ws_t *ws, const uint8_t *data, size_t len)
{
	if (!ws || !ws->open)
		return false;
	EnterCriticalSection(&ws->send_lock);
	DWORD err = WinHttpWebSocketSend(ws->socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)data, (DWORD)len);
	LeaveCriticalSection(&ws->send_lock);
	if (err != NO_ERROR) {
		char reason[96];
		snprintf(reason, sizeof(reason), "send failed (WinHTTP error %lu)", err);
		fire_close(ws, reason);
		return false;
	}
	return true;
}

void fdr_ws_destroy(fdr_ws_t *ws)
{
	if (!ws)
		return;
	InterlockedExchange(&ws->destroyed, 1);
	InterlockedExchange(&ws->open, 0);
	// Closing the handles aborts a blocking connect or receive on the worker thread.
	EnterCriticalSection(&ws->handle_lock);
	HINTERNET handles[] = {ws->socket, ws->request, ws->connection, ws->session};
	for (int i = 0; i < 4; i++)
		if (handles[i])
			WinHttpCloseHandle(handles[i]);
	ws->socket = ws->request = ws->connection = ws->session = NULL;
	LeaveCriticalSection(&ws->handle_lock);

	if (WaitForSingleObject(ws->thread, 15000) != WAIT_OBJECT_0) {
		// Thread still stuck in WinHTTP: leak rather than free memory it may touch.
		return;
	}
	CloseHandle(ws->thread);
	EnterCriticalSection(&ws->send_lock); // no send in flight
	LeaveCriticalSection(&ws->send_lock);
	DeleteCriticalSection(&ws->send_lock);
	DeleteCriticalSection(&ws->handle_lock);
	free(ws->url);
	free(ws->headers);
	free(ws);
}
