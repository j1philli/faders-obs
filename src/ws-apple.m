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
// macOS WebSocket transport on NSURLSessionWebSocketTask: system TLS, no bundled libraries.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include "ws.h"

// Chunks are ~11 KB every 500 ms; anything this deep means the network can't keep up.
#define MAX_PENDING_SENDS 64

@interface FdrWsDelegate : NSObject <NSURLSessionWebSocketDelegate>
@property(nonatomic, assign) fdr_ws_t *owner;
@end

struct fdr_ws {
	NSURLSession *session;
	NSURLSessionWebSocketTask *task;
	FdrWsDelegate *delegate;
	fdr_ws_callbacks cb;
	void *param;
	atomic_bool open;
	atomic_bool closed;   // on_close already delivered
	atomic_bool destroyed; // owner gone: deliver nothing
	atomic_int pending;
	// Held for the whole of every callback into the owner, and by fdr_ws_destroy() while it marks
	// the socket destroyed. So once destroy returns, no callback is running and none will start:
	// the owner can free itself. (Checking `destroyed` alone isn't enough: a callback that passed
	// the check could still be inside the owner.) Recursive: a failed receive reports the close
	// from inside the receive handler. Owners must not hold a lock their callbacks take while
	// calling fdr_ws_destroy().
	pthread_mutex_t cb_mu;
};

static void cb_lock_init(fdr_ws_t *ws)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&ws->cb_mu, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void fdr_ws_fire_close_status(fdr_ws_t *ws, int http_status, NSString *reason)
{
	atomic_store(&ws->open, false);
	bool expected = false;
	if (!atomic_compare_exchange_strong(&ws->closed, &expected, true))
		return;
	pthread_mutex_lock(&ws->cb_mu);
	if (!atomic_load(&ws->destroyed) && ws->cb.on_close)
		ws->cb.on_close(ws->param, http_status, reason ? reason.UTF8String : "closed");
	pthread_mutex_unlock(&ws->cb_mu);
}

static void fdr_ws_fire_close(fdr_ws_t *ws, NSString *reason)
{
	fdr_ws_fire_close_status(ws, 0, reason);
}

// Called with cb_mu held and the socket not destroyed (ws->task is only cleared under cb_mu).
static void fdr_ws_receive(fdr_ws_t *ws)
{
	NSURLSessionWebSocketTask *task = ws->task;
	[task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *msg, NSError *err) {
		pthread_mutex_lock(&ws->cb_mu);
		if (!atomic_load(&ws->destroyed)) {
			if (err) {
				fdr_ws_fire_close(ws, err.localizedDescription);
			} else {
				if (msg.type == NSURLSessionWebSocketMessageTypeString && ws->cb.on_text)
					ws->cb.on_text(ws->param, msg.string.UTF8String);
				fdr_ws_receive(ws);
			}
		}
		pthread_mutex_unlock(&ws->cb_mu);
	}];
}

@implementation FdrWsDelegate
- (void)URLSession:(NSURLSession *)session
		webSocketTask:(NSURLSessionWebSocketTask *)task
	didOpenWithProtocol:(NSString *)protocol
{
	fdr_ws_t *ws = self.owner;
	if (!ws)
		return;
	pthread_mutex_lock(&ws->cb_mu);
	if (!atomic_load(&ws->destroyed)) {
		atomic_store(&ws->open, true);
		if (ws->cb.on_open)
			ws->cb.on_open(ws->param);
		fdr_ws_receive(ws);
	}
	pthread_mutex_unlock(&ws->cb_mu);
}

- (void)URLSession:(NSURLSession *)session
	   webSocketTask:(NSURLSessionWebSocketTask *)task
	didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
	      reason:(NSData *)reason
{
	fdr_ws_t *ws = self.owner;
	if (ws)
		fdr_ws_fire_close(ws, [NSString stringWithFormat:@"closed by server (%ld)", (long)code]);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
	fdr_ws_t *ws = self.owner;
	if (!ws)
		return;
	// A rejected handshake (e.g. 401 for a bad key) surfaces here with the HTTP status.
	NSHTTPURLResponse *resp = (NSHTTPURLResponse *)task.response;
	NSString *reason = error.localizedDescription ?: @"connection ended";
	int status = 0;
	if ([resp isKindOfClass:[NSHTTPURLResponse class]] && resp.statusCode >= 400) {
		status = (int)resp.statusCode;
		reason = [NSString stringWithFormat:@"server rejected connection (HTTP %d)", status];
	}
	fdr_ws_fire_close_status(ws, status, reason);
}
@end

fdr_ws_t *fdr_ws_connect(const char *url, const char *bearer, const fdr_ws_callbacks *cb, void *param)
{
	@autoreleasepool {
		NSURL *nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
		if (!nsurl)
			return NULL;
		fdr_ws_t *ws = calloc(1, sizeof(*ws));
		cb_lock_init(ws);
		ws->cb = *cb;
		ws->param = param;
		ws->delegate = [FdrWsDelegate new];
		ws->delegate.owner = ws;
		NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		ws->session = [NSURLSession sessionWithConfiguration:cfg delegate:ws->delegate delegateQueue:nil];
		NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
		if (bearer && *bearer)
			[req setValue:[NSString stringWithFormat:@"Bearer %s", bearer] forHTTPHeaderField:@"Authorization"];
		[req setValue:@"faders-obs/" FDR_VERSION_STR forHTTPHeaderField:@"User-Agent"];
		ws->task = [ws->session webSocketTaskWithRequest:req];
		ws->task.maximumMessageSize = 1 << 20;
		[ws->task resume];
		return ws;
	}
}

static bool fdr_ws_send(fdr_ws_t *ws, NSURLSessionWebSocketMessage *(^make)(void))
{
	if (!ws || !atomic_load(&ws->open) || atomic_load(&ws->pending) >= MAX_PENDING_SENDS)
		return false;
	@autoreleasepool {
		atomic_fetch_add(&ws->pending, 1);
		NSURLSessionWebSocketMessage *msg = make();
		[ws->task sendMessage:msg
		    completionHandler:^(NSError *err) {
			    atomic_fetch_sub(&ws->pending, 1);
			    if (err && !atomic_load(&ws->destroyed))
				    fdr_ws_fire_close(ws, err.localizedDescription);
		    }];
	}
	return true;
}

bool fdr_ws_send_binary(fdr_ws_t *ws, const uint8_t *data, size_t len)
{
	return fdr_ws_send(ws, ^{
		return [[NSURLSessionWebSocketMessage alloc] initWithData:[NSData dataWithBytes:data length:len]];
	});
}

bool fdr_ws_send_text(fdr_ws_t *ws, const char *text)
{
	return fdr_ws_send(ws, ^{
		return [[NSURLSessionWebSocketMessage alloc] initWithString:[NSString stringWithUTF8String:text]];
	});
}

void fdr_ws_destroy(fdr_ws_t *ws)
{
	if (!ws)
		return;
	// Waits for a callback that is inside the owner right now; after this none can start.
	pthread_mutex_lock(&ws->cb_mu);
	atomic_store(&ws->destroyed, true);
	atomic_store(&ws->open, false);
	@autoreleasepool {
		ws->delegate.owner = NULL;
		[ws->task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
		[ws->session invalidateAndCancel];
		ws->task = nil;
		ws->session = nil;
		ws->delegate = nil;
	}
	pthread_mutex_unlock(&ws->cb_mu);
	// Completion blocks may still be queued and reference `ws` and its lock; they check
	// `destroyed` under the lock and do nothing. So the small struct is leaked rather than
	// freed under them (one per reconnect, about a hundred bytes).
}
