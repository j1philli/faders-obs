## Faders debug info

Generated 2026-11-29 15:41:07 (UTC-07:00) · 2026-11-29 22:41:07 UTC
Nothing was uploaded. Removed before you saw this: your plugin key, 2 folder paths (your user name), 2 IP addresses.
Names are left out: sources are "Source 1", "Source 2"…, faders "Fader 1"…, the channel "[channel]".

- **Faders**: 0.4.0, running 1 h 42 m
- **OBS**: 32.2.2, live 37 m, stream audio Track 1
- **System**: macOS 15.1 (24B83), arm64, Apple M3 Pro, 12 cores / 12 threads, 36 GB
- **Audio**: 48 kHz stereo: OK for Faders
- **Account**: [channel] · Pro · Twitch and YouTube · 3 of 50 viewers mixing
- **Connection**: connected, 5 layers, 480 kbps up · wss://hub.faders.tv/ingest
- **Last failure**: connect to [ip]:443 failed
- **Problems**: "Source 10" isn't in the stream the way it's sent (plays in Everything else); "Source 5" is in the stream twice
- **Per-platform fader**: Fader 3, on · Twitch: Source 3 · YouTube: Source 11 on Track 3 · Twitch source is also on the YouTube track: YouTube viewers will hear it twice

**Faders** (Pro includes 4)

- **Fader 1**: Source 1
- **Fader 2**: Source 2
- **Fader 3**: Source 3 (Twitch), Source 10 (dropped for this stream), Source 11 (YouTube)
- **Fader 4**: Source 4
- **Fader 5** — not in your plan, plays in Everything else: Source 5
- **Everything else**: 4 sources, 2 new — Source 6, Source 7 (new), Source 8 (new), Source 9 (its fader "Removed fader 1" is gone)

**Audio sources**: 11 — 3 browser_source, 3 ffmpeg_source, 2 wasapi_process_output_capture, 1 coreaudio_input_capture, 1 game_capture, 1 wasapi_input_capture
Not placed yet 2 · muted 1 · monitor only 1 · not on Track 1 2

**Recent plugin log** (last 9 of 9, UTC)

```text
21:39:02 INFO loaded (version 0.4.0)
21:43:11 INFO uplink started → wss://hub.faders.tv/ingest
21:43:12 INFO connected to wss://hub.faders.tv/ingest
21:43:12 INFO your plan includes 4 faders: "Fader 5" and later are folded into Everything else
21:51:27 INFO "Source 3" → Fader 3
21:57:02 WARN disconnected: connect to [ip]:443 failed
21:57:03 INFO retrying with key [REDACTED:plugin-key]
22:01:44 WARN "Source 5" is in your stream (Track 1) more than once
22:01:45 INFO settings in /Users/[user]/Library/Application Support/obs-studio
```

<details><summary>Full details (JSON)</summary>

```json
{
 "schema": 1,
 "generated": {"local": "2026-11-29T15:41:07-07:00", "utc": "2026-11-29T22:41:07Z"},
 "names": false,
 "redacted": {"plugin_key": 1, "stream_key": 0, "paths": 2, "emails": 0, "urls": 0, "tokens": 0, "ips": 2},
 "plugin": {"version": "0.4.0", "uptime_s": 6125, "config_dir": "/Users/[user]/Library/Application Support/obs-studio/plugin_config/faders", "module_dir": "/Library/Application Support/obs-studio/plugins/faders.plugin/Contents/MacOS", "update_available": null},
 "obs": {"version": "32.2.2", "locale": "en-US", "streaming": true, "stream_uptime_s": 2276, "stream_audio_track": 1},
 "system": {"os": "macOS", "os_version": "15.1 (24B83)", "arch": "arm64", "cpu": "Apple M3 Pro", "cores_physical": 12, "cores_logical": 12, "ram_gb": 36},
 "audio": {"sample_rate": 48000, "channels": 2, "mixer_ok": true},
 "settings": {"enabled": true, "only_live": true, "plugin_key_set": true},
 "account": {"channel": "[channel]", "plan": "pro", "platforms": ["twitch", "youtube"], "listener_limit": 50, "max_stems": 5},
 "connection": {"running": true, "connected": true, "ever_connected": true, "server": "wss://hub.faders.tv/ingest", "listeners": 3, "uplink_kbps": 480, "chunks_sent": 4552, "stems_on_wire": 5, "stream_track": 1, "stream_track_silent": false, "last_failure": "connect to [ip]:443 failed", "rejected_status": 0},
 "problems": {"mix_mismatch": "Source 10", "mix_duplicate": "Source 5"},
 "split": {"fader": "Fader 3", "ticked": true, "on": true, "why": "on", "twitch_source": "Source 3", "youtube_source": "Source 11", "youtube_track": 3, "warnings": ["twitch-source-on-youtube-track"]},
 "faders": [
  {"name": "Fader 1", "on_wire": true, "sources": ["Source 1"]},
  {"name": "Fader 2", "on_wire": true, "sources": ["Source 2"]},
  {"name": "Fader 3", "on_wire": true, "sources": ["Source 3", "Source 10", "Source 11"]},
  {"name": "Fader 4", "on_wire": true, "sources": ["Source 4"]},
  {"name": "Fader 5", "on_wire": false, "sources": ["Source 5"]}
 ],
 "allowance": 4,
 "sources": {"audio_total": 11, "listed": 11, "truncated": false, "past_limit": 0, "new": 2, "muted": 1, "monitor_only": 1, "not_on_stream_track": 2,
  "by_kind": {"browser_source": 3, "ffmpeg_source": 3, "wasapi_process_output_capture": 2, "coreaudio_input_capture": 1, "game_capture": 1, "wasapi_input_capture": 1},
  "list": [
   {"i": 1, "name": "Source 1", "kind": "wasapi_input_capture", "saved": "Fader 1", "plays": "Fader 1", "state": "ok", "tracks": [1, 2]},
   {"i": 2, "name": "Source 2", "kind": "game_capture", "saved": "Fader 2", "plays": "Fader 2", "state": "ok", "tracks": [1, 3]},
   {"i": 3, "name": "Source 3", "kind": "wasapi_process_output_capture", "saved": "Fader 3", "plays": "Fader 3", "state": "ok", "tracks": [1, 3]},
   {"i": 4, "name": "Source 4", "kind": "wasapi_process_output_capture", "saved": "Fader 4", "plays": "Fader 4", "state": "ok", "tracks": [1, 2], "monitoring": "monitor and output"},
   {"i": 5, "name": "Source 5", "kind": "browser_source", "saved": "Fader 5", "plays": "Everything else", "state": "locked", "tracks": [1]},
   {"i": 6, "name": "Source 6", "kind": "coreaudio_input_capture", "saved": "rest", "plays": "Everything else", "state": "rest", "tracks": [], "muted": true},
   {"i": 7, "name": "Source 7", "kind": "browser_source", "saved": "new", "plays": "Everything else", "state": "new", "tracks": [1]},
   {"i": 8, "name": "Source 8", "kind": "ffmpeg_source", "saved": "new", "plays": "Everything else", "state": "new", "tracks": [1], "monitoring": "monitor only"},
   {"i": 9, "name": "Source 9", "kind": "ffmpeg_source", "saved": "Removed fader 1", "plays": "Everything else", "state": "deleted", "tracks": [1]},
   {"i": 10, "name": "Source 10", "kind": "ffmpeg_source", "saved": "Fader 3", "plays": "Everything else", "state": "dropped", "tracks": [1]},
   {"i": 11, "name": "Source 11", "kind": "browser_source", "saved": "Fader 3", "plays": "Fader 3", "state": "ok", "tracks": [3]}
  ]},
 "log": {"total": 9, "lines": [
  {"t": "21:39:02", "level": "info", "msg": "loaded (version 0.4.0)"},
  {"t": "21:43:11", "level": "info", "msg": "uplink started → wss://hub.faders.tv/ingest"},
  {"t": "21:43:12", "level": "info", "msg": "connected to wss://hub.faders.tv/ingest"},
  {"t": "21:43:12", "level": "info", "msg": "your plan includes 4 faders: \"Fader 5\" and later are folded into Everything else"},
  {"t": "21:51:27", "level": "info", "msg": "\"Source 3\" → Fader 3"},
  {"t": "21:57:02", "level": "warning", "msg": "disconnected: connect to [ip]:443 failed"},
  {"t": "21:57:03", "level": "info", "msg": "retrying with key [REDACTED:plugin-key]"},
  {"t": "22:01:44", "level": "warning", "msg": "\"Source 5\" is in your stream (Track 1) more than once"},
  {"t": "22:01:45", "level": "info", "msg": "settings in /Users/[user]/Library/Application Support/obs-studio"}
 ]}
}
```

</details>
