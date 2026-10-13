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
};

static void fdr_ws_fire_close(fdr_ws_t *ws, NSString *reason)
{
	atomic_store(&ws->open, false);
	bool expected = false;
	if (!atomic_compare_exchange_strong(&ws->closed, &expected, true))
		return;
	if (!atomic_load(&ws->destroyed) && ws->cb.on_close)
		ws->cb.on_close(ws->param, reason ? reason.UTF8String : "closed");
}

static void fdr_ws_receive(fdr_ws_t *ws)
{
	NSURLSessionWebSocketTask *task = ws->task;
	[task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *msg, NSError *err) {
		if (atomic_load(&ws->destroyed))
			return;
		if (err) {
			fdr_ws_fire_close(ws, err.localizedDescription);
			return;
		}
		if (msg.type == NSURLSessionWebSocketMessageTypeString && ws->cb.on_text)
			ws->cb.on_text(ws->param, msg.string.UTF8String);
		fdr_ws_receive(ws);
	}];
}

@implementation FdrWsDelegate
- (void)URLSession:(NSURLSession *)session
		webSocketTask:(NSURLSessionWebSocketTask *)task
	didOpenWithProtocol:(NSString *)protocol
{
	fdr_ws_t *ws = self.owner;
	if (!ws || atomic_load(&ws->destroyed))
		return;
	atomic_store(&ws->open, true);
	if (ws->cb.on_open)
		ws->cb.on_open(ws->param);
	fdr_ws_receive(ws);
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
	if ([resp isKindOfClass:[NSHTTPURLResponse class]] && resp.statusCode >= 400)
		reason = [NSString stringWithFormat:@"server rejected connection (HTTP %ld)", (long)resp.statusCode];
	fdr_ws_fire_close(ws, reason);
}
@end

fdr_ws_t *fdr_ws_connect(const char *url, const char *bearer, const fdr_ws_callbacks *cb, void *param)
{
	@autoreleasepool {
		NSURL *nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
		if (!nsurl)
			return NULL;
		fdr_ws_t *ws = calloc(1, sizeof(*ws));
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

bool fdr_ws_send_binary(fdr_ws_t *ws, const uint8_t *data, size_t len)
{
	if (!ws || !atomic_load(&ws->open) || atomic_load(&ws->pending) >= MAX_PENDING_SENDS)
		return false;
	@autoreleasepool {
		atomic_fetch_add(&ws->pending, 1);
		NSData *payload = [NSData dataWithBytes:data length:len];
		NSURLSessionWebSocketMessage *msg = [[NSURLSessionWebSocketMessage alloc] initWithData:payload];
		[ws->task sendMessage:msg
		    completionHandler:^(NSError *err) {
			    atomic_fetch_sub(&ws->pending, 1);
			    if (err && !atomic_load(&ws->destroyed))
				    fdr_ws_fire_close(ws, err.localizedDescription);
		    }];
	}
	return true;
}

void fdr_ws_destroy(fdr_ws_t *ws)
{
	if (!ws)
		return;
	atomic_store(&ws->destroyed, true);
	@autoreleasepool {
		ws->delegate.owner = NULL;
		[ws->task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
		[ws->session invalidateAndCancel];
		ws->task = nil;
		ws->session = nil;
		ws->delegate = nil;
	}
	// Completion blocks may still be queued and reference `ws`; they check `destroyed` first.
	// Leak the small struct rather than race them (one per reconnect, a few dozen bytes).
}
