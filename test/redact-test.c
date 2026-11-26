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
// faders-redact-test: the debug report's redactor (src/redact.c, no OBS) against its 59-case
// table (input → expected; each case is run twice, so every rule must be idempotent), the leak
// check that makes the report fail closed, and the edge cases listed in the design's
// redaction.md. Every key, token, name and address below is made up.
//   faders-redact-test [--md]     (--md prints the table as Markdown)
#include "../src/redact.c"

#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const fdr_redact_ctx ctx = {
	.plugin_key = "fdr_live_Q8zK3vT1mN7pR2xW9yB4cD6fG0hJ5kL1",
	.stream_key = "live_918273645_Xy7Pq2Lm9Rt4Vb6Nc8Kd1Jf3",
	.user_names = {"jordan", "Jordan Lee"},
};

static const struct {
	const char *area, *in, *want, *why;
} cases[] = {
	// Known values (layer 1)
	{"known", "key=fdr_live_Q8zK3vT1mN7pR2xW9yB4cD6fG0hJ5kL1", "key=[REDACTED:plugin-key]", "the configured plugin key, exact"},
	{"known", "Mic (live_918273645_Xy7Pq2Lm9Rt4Vb6Nc8Kd1Jf3)", "Mic ([REDACTED:stream-key])", "stream key pasted into a source name"},
	// Token shapes
	{"token", "old key fdr_live_A1b2C3d4E5f6G7h8", "old key fdr_live_[REDACTED]", "a Faders key that isn't the current one"},
	{"token", "fdr_live_", "fdr_live_", "the bare prefix is not a key"},
	{"token", "twitch live_123456789_abcdefghijKLMNOP", "twitch [REDACTED:stream-key]", "Twitch key shape"},
	{"token", "yt a1b2-c3d4-e5f6-g7h8-i9j0", "yt [REDACTED:stream-key]", "YouTube key shape (5 groups)"},
	{"token", "this-that-what-when", "this-that-what-when", "4x4 words without a digit: kept"},
	{"token", "sk_us-west-2_aB3dE5gH7jK9mN1pQ", "[REDACTED:token]", "Kick-style sk_ key"},
	{"token", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U", "[REDACTED:jwt]", "JWT"},
	{"token", "Authorization: Bearer fdr_live_A1b2C3d4E5f6G7h8", "Authorization: [REDACTED]", "header: rest of line"},
	{"token", "retrying with Bearer abcdefgh12345678", "retrying with Bearer [REDACTED]", "bare bearer token"},
	{"token", "stream_key=abc123&x=1", "stream_key=[REDACTED]&x=1", "key=value"},
	{"token", "{\"password\": \"hunter2 hunter2\"}", "{\"password\": [REDACTED]}", "JSON pair, quoted value with a space"},
	{"token", "Hotkey: F5", "Hotkey: F5", "'key' inside a word: kept"},
	{"token", "Key: F5", "Key: [REDACTED]", "false positive we accept"},
	{"token", "WebSocket error code: 1006", "WebSocket error code: 1006", "'code' is not a secret name"},
	{"token", "9fK2mQx7Lp0Zr4Tb8Wc1vN", "[REDACTED:token]", "random 22 chars: entropy backstop"},
	{"token", "3f2b8c1e-9a4d-4e6b-b1c7-0d5e8f9a2c31", "[REDACTED:token]", "UUID: removed (harmless loss)"},
	{"token", "FortniteClient-Win64-Shipping.exe", "FortniteClient-Win64-Shipping.exe", "long exe name: kept"},
	{"token", "wasapi_output_capture", "wasapi_output_capture", "OBS source id: kept"},
	// URLs
	{"url", "wss://hub.faders.tv/ingest", "wss://hub.faders.tv/ingest", "the hub: kept"},
	{"url", "rtmp://live.twitch.tv/app/live_123456789_abcdefghijKLMNOP", "rtmp://live.twitch.tv/app/[REDACTED]", "RTMP: path after app"},
	{"url", "rtmps://a.rtmps.youtube.com:443/live2/a1b2-c3d4-e5f6-g7h8", "rtmps://a.rtmps.youtube.com:443/live2/[REDACTED]", "RTMPS with port"},
	{"url", "srt://ingest.example.net:9000?streamid=#!::r=live/x,m=publish&passphrase=hunter2", "srt://ingest.example.net:9000?[REDACTED]", "SRT: query holds the secrets"},
	{"url", "https://bob:s3cret@cdn.example.com/a.mp3", "https://[REDACTED]@cdn.example.com/a.mp3", "userinfo"},
	{"url", "https://streamlabs.com/alert-box/v3/8F3A9C2E71B04D6E5A1F?token=abc", "https://streamlabs.com/alert-box/v3/[REDACTED]?[REDACTED]", "browser-source token in path and query"},
	{"url", "https://discord.com/api/webhooks/123456789012345678/Zx9Yw8Vu7Ts6Rq5Po4Nm3Lk2", "https://discord.com/api/webhooks/123456789012345678/[REDACTED]", "webhook: id kept, token removed"},
	{"url", "https://example.com/cb#access_token=abc", "https://example.com/cb#[REDACTED]", "fragment"},
	{"url", "(see https://faders.tv/support/).", "(see https://faders.tv/support/).", "trailing punctuation stays outside"},
	{"url", "file:///Users/jordan/overlay.html?v=2", "file:///Users/[user]/overlay.html?[REDACTED]", "file URL: query and home folder"},
	// Emails
	{"email", "Alerts (jordan.lee+obs@gmail.com)", "Alerts ([email])", "email in a source name"},
	{"email", "hello@faders.tv", "[email]", "every email, ours too"},
	{"email", "thanks @faders!", "thanks @faders!", "a handle is not an email"},
	{"email", "user@localhost", "user@localhost", "no dot in domain: kept"},
	// Paths and user names
	{"path", "C:\\Users\\jordan\\Music\\bgm.mp3", "C:\\Users\\[user]\\Music\\bgm.mp3", "Windows"},
	{"path", "C:\\Users\\Jordan Lee\\Videos\\intro.mp4", "C:\\Users\\[user]\\Videos\\intro.mp4", "Windows, space in name"},
	{"path", "C:\\\\Users\\\\jordan\\\\AppData", "C:\\\\Users\\\\[user]\\\\AppData", "JSON-escaped backslashes"},
	{"path", "c:/users/jordan/Desktop/x.wav", "c:/users/[user]/Desktop/x.wav", "forward slashes, lower case"},
	{"path", "\\\\?\\C:\\Users\\jordan\\x", "\\\\?\\C:\\Users\\[user]\\x", "long-path prefix"},
	{"path", "/Users/jordan/Library/Application Support/obs-studio", "/Users/[user]/Library/Application Support/obs-studio", "macOS"},
	{"path", "/Users/Shared/loop.wav", "/Users/Shared/loop.wav", "macOS shared folder: kept"},
	{"path", "/home/jordan/.config/obs-studio", "/home/[user]/.config/obs-studio", "Linux"},
	{"path", "/var/home/jordan/x", "/var/home/[user]/x", "Fedora Silverblue"},
	{"path", "D:\\Streams\\bgm.mp3", "D:\\Streams\\bgm.mp3", "not under a home folder: kept"},
	{"path", "\\\\NAS01\\media\\bgm.mp3", "\\\\[host]\\media\\bgm.mp3", "UNC host"},
	{"path", "\"\\\\\\\\NAS01\\\\media\"", "\"\\\\\\\\[host]\\\\media\"", "UNC, JSON-escaped"},
	{"path", "C:\\\\ProgramData\\\\obs-studio\\\\plugins", "C:\\\\ProgramData\\\\obs-studio\\\\plugins", "JSON-escaped separators are not UNC hosts"},
	{"path", "Jordan's mic", "[user]'s mic", "OS user name anywhere"},
	{"path", "jordanna cam", "jordanna cam", "user name inside a word: kept"},
	// IPs
	{"ip", "connect to 203.0.113.7:443 failed", "connect to [ip]:443 failed", "IPv4"},
	{"ip", "127.0.0.1:4455", "127.0.0.1:4455", "loopback: kept"},
	{"ip", "Windows 10.0.22631", "Windows 10.0.22631", "OS build: kept"},
	{"ip", "1.2.3.4.5", "1.2.3.4.5", "5 parts: not an IP"},
	// Everything else
	{"misc", "https://faders.tv/portal/#plugin-keys", "https://faders.tv/portal/#[REDACTED]", "harmless loss: every fragment goes"},
	{"misc", "obs-websocket password: hunter2", "obs-websocket password: [REDACTED]", "plain password"},
	{"misc", "GET https://jordan:pw@203.0.113.7/x?sig=1 from C:\\Users\\jordan", "GET https://[REDACTED]@[ip]/x?[REDACTED] from C:\\Users\\[user]", "several in one log line"},
	{"misc", "git 5a2a4dd0c1f3e9b7a6d5c4b3a2f1e0d9c8b7a6f5", "git [REDACTED:token]", "40-hex hash: removed (harmless loss)"},
	{"misc", "🎵 BRB music", "🎵 BRB music", "emoji untouched"},
	{"misc", "[REDACTED:jwt] and [user]", "[REDACTED:jwt] and [user]", "idempotent"},
};

static void md_cell(const char *s)
{
	putchar('`');
	for (; *s; s++)
		if (*s == '|')
			fputs("\\|", stdout);
		else
			putchar(*s);
	putchar('`');
}

static int fails;
static void check(bool ok, const char *name, const char *got)
{
	printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, ok ? "" : got);
	fails += !ok;
}

int main(int argc, char **argv)
{
	int md = argc > 1 && !strcmp(argv[1], "--md");
	int n = (int)(sizeof(cases) / sizeof(*cases));
	int table_fails = 0;
	if (md)
		puts("| # | Area | Input | Expected output | Why | Result |\n|---|---|---|---|---|---|");
	for (int i = 0; i < n; i++) {
		char *got = fdr_redact(cases[i].in, &ctx, NULL);
		char *twice = fdr_redact(got, &ctx, NULL);
		int ok = !strcmp(got, cases[i].want) && !strcmp(twice, got);
		table_fails += !ok;
		if (md) {
			printf("| %d | %s | ", i + 1, cases[i].area);
			md_cell(cases[i].in);
			fputs(" | ", stdout);
			md_cell(cases[i].want);
			printf(" | %s | %s |\n", cases[i].why, ok ? "pass" : "FAIL");
		} else if (!ok) {
			printf("FAIL %d [%s]\n  in:   %s\n  want: %s\n  got:  %s\n  2nd:  %s\n", i + 1, cases[i].why, cases[i].in, cases[i].want, got, twice);
		}
		free(got);
		free(twice);
	}
	if (md)
		return table_fails != 0;
	printf("%s  %-60s %d/%d\n", table_fails ? "FAIL" : "PASS", "redaction table (59 cases, each run twice)", n - table_fails, n);
	fails += table_fails != 0 || n != 59;

	// Fail closed: a known secret that survives must be caught; its placeholder must not be.
	check(fdr_redact_leaks("x fdr_live_Q8zK3vT1mN7pR2xW9yB4cD6fG0hJ5kL1", &ctx) == 1 && fdr_redact_leaks("x [REDACTED:plugin-key]", &ctx) == 0 &&
		      fdr_redact_leaks("Mic live_918273645_Xy7Pq2Lm9Rt4Vb6Nc8Kd1Jf3", &ctx) == 1 && fdr_redact_leaks("nothing here", &ctx) == 0,
	      "leak check catches the plugin key and the stream key", "");

	// What was removed, counted per kind (the report says "Removed: your plugin key, …").
	fdr_redact_counts c;
	char *out = fdr_redact("key fdr_live_Q8zK3vT1mN7pR2xW9yB4cD6fG0hJ5kL1 and live_918273645_Xy7Pq2Lm9Rt4Vb6Nc8Kd1Jf3 at C:\\Users\\jordan\\x and /Users/jordan/y, mail a@b.co, 203.0.113.7",
			       &ctx, &c);
	char got[512];
	snprintf(got, sizeof(got), "plugin_key %d stream_key %d paths %d emails %d ips %d urls %d tokens %d: %s", c.plugin_key, c.stream_key, c.paths, c.emails, c.ips, c.urls, c.tokens, out);
	check(c.plugin_key == 1 && c.stream_key == 1 && c.paths == 2 && c.emails == 1 && c.ips == 1 && c.urls == 0 && c.tokens == 0, "counts per kind", got);
	free(out);

	// No keys configured (empty plugin key field, no stream service): nothing crashes, nothing leaks.
	fdr_redact_ctx empty = {0};
	out = fdr_redact("connect to 203.0.113.7 failed", &empty, NULL);
	check(!strcmp(out, "connect to [ip] failed") && fdr_redact_leaks(out, &empty) == 0, "no keys configured", out);
	free(out);
	out = fdr_redact(NULL, &empty, NULL);
	check(out && !*out, "NULL input is an empty string", out ? out : "(null)");
	free(out);

	// Non-ASCII user names are replaced whole; the UTF-8 is never cut in half.
	fdr_redact_ctx intl = {.user_names = {"Jörg", "山田"}};
	out = fdr_redact("C:\\Users\\Jörg\\Music and /Users/山田/Movies and Jörg's mic", &intl, NULL);
	check(!strcmp(out, "C:\\Users\\[user]\\Music and /Users/[user]/Movies and [user]'s mic"), "non-ASCII user names, whole", out);
	free(out);

	// A short or ordinary-word user name only goes from home folders.
	fdr_redact_ctx shortname = {.user_names = {"al", NULL}};
	out = fdr_redact("/Users/al/x: al's mic, also", &shortname, NULL);
	check(!strcmp(out, "/Users/[user]/x: al's mic, also"), "a 2-letter user name only from paths", out);
	free(out);

	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
