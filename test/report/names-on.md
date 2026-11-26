## Faders debug info

Generated 2026-11-29 15:41:07 (UTC-07:00) · 2026-11-29 22:41:07 UTC
Nothing was uploaded. Removed before you saw this: your plugin key, your stream key, 3 folder paths (your user name), 1 email address, 2 IP addresses.
Source, fader and channel names are included.

- **Faders**: 0.4.0, running 1 h 42 m
- **OBS**: 32.2.2, live 37 m, stream audio Track 1
- **System**: macOS 15.1 (24B83), arm64, Apple M3 Pro, 12 cores / 12 threads, 36 GB
- **Audio**: 48 kHz stereo: OK for Faders
- **Account**: NightOwlPlays · Pro · Twitch and YouTube · 3 of 50 viewers mixing
- **Connection**: connected, 5 layers, 480 kbps up · wss://hub.faders.tv/ingest
- **Last failure**: connect to [ip]:443 failed
- **Problems**: "Música 🎵" isn't in the stream the way it's sent (plays in Everything else); "Alert Box (browser)" is in the stream twice
- **Per-platform fader**: Music, on · Twitch: Spotify · YouTube: YT Music on Track 3 · Twitch source is also on the YouTube track: YouTube viewers will hear it twice

**Faders** (Pro includes 4)

- **Voice**: Mic/Aux
- **Game**: Game Capture
- **Music**: Spotify (Twitch), Música 🎵 (dropped for this stream), YT Music (YouTube)
- **Discord**: Discord
- **Alerts** — not in your plan, plays in Everything else: Alert Box (browser)
- **Everything else**: 4 sources, 2 new — [user]'s mic, Tip jar ([email]) (new), Say "hi" \ now please (new), Key test [REDACTED:stream-key] (its fader "Old fader" is gone)

**Audio sources**: 11 — 3 browser_source, 3 ffmpeg_source, 2 wasapi_process_output_capture, 1 coreaudio_input_capture, 1 game_capture, 1 wasapi_input_capture
Not placed yet 2 · muted 1 · monitor only 1 · not on Track 1 2

**Recent plugin log** (last 9 of 9, UTC)

```text
21:39:02 INFO loaded (version 0.4.0)
21:43:11 INFO uplink started → wss://hub.faders.tv/ingest
21:43:12 INFO connected to wss://hub.faders.tv/ingest
21:43:12 INFO your plan includes 4 faders: "Alerts" and later are folded into Everything else
21:51:27 INFO "Spotify" → Music
21:57:02 WARN disconnected: connect to [ip]:443 failed
21:57:03 INFO retrying with key [REDACTED:plugin-key]
22:01:44 WARN "Alert Box (browser)" is in your stream (Track 1) more than once
22:01:45 INFO settings in /Users/[user]/Library/Application Support/obs-studio
```

<details><summary>Full details (JSON)</summary>

```json
{
 "schema": 1,
 "generated": {"local": "2026-11-29T15:41:07-07:00", "utc": "2026-11-29T22:41:07Z"},
 "names": true,
 "redacted": {"plugin_key": 1, "stream_key": 1, "paths": 3, "emails": 1, "urls": 0, "tokens": 0, "ips": 2},
 "plugin": {"version": "0.4.0", "uptime_s": 6125, "config_dir": "/Users/[user]/Library/Application Support/obs-studio/plugin_config/faders", "module_dir": "/Library/Application Support/obs-studio/plugins/faders.plugin/Contents/MacOS", "update_available": null},
 "obs": {"version": "32.2.2", "locale": "en-US", "streaming": true, "stream_uptime_s": 2276, "stream_audio_track": 1},
 "system": {"os": "macOS", "os_version": "15.1 (24B83)", "arch": "arm64", "cpu": "Apple M3 Pro", "cores_physical": 12, "cores_logical": 12, "ram_gb": 36},
 "audio": {"sample_rate": 48000, "channels": 2, "mixer_ok": true},
 "settings": {"enabled": true, "only_live": true, "plugin_key_set": true},
 "account": {"channel": "NightOwlPlays", "plan": "pro", "platforms": ["twitch", "youtube"], "listener_limit": 50, "max_stems": 5},
 "connection": {"running": true, "connected": true, "ever_connected": true, "server": "wss://hub.faders.tv/ingest", "listeners": 3, "uplink_kbps": 480, "chunks_sent": 4552, "stems_on_wire": 5, "stream_track": 1, "stream_track_silent": false, "last_failure": "connect to [ip]:443 failed", "rejected_status": 0},
 "problems": {"mix_mismatch": "Música 🎵", "mix_duplicate": "Alert Box (browser)"},
 "split": {"fader": "Music", "ticked": true, "on": true, "why": "on", "twitch_source": "Spotify", "youtube_source": "YT Music", "youtube_track": 3, "warnings": ["twitch-source-on-youtube-track"]},
 "faders": [
  {"name": "Voice", "on_wire": true, "sources": ["Mic/Aux"]},
  {"name": "Game", "on_wire": true, "sources": ["Game Capture"]},
  {"name": "Music", "on_wire": true, "sources": ["Spotify", "Música 🎵", "YT Music"]},
  {"name": "Discord", "on_wire": true, "sources": ["Discord"]},
  {"name": "Alerts", "on_wire": false, "sources": ["Alert Box (browser)"]}
 ],
 "allowance": 4,
 "sources": {"audio_total": 11, "listed": 11, "truncated": false, "past_limit": 0, "new": 2, "muted": 1, "monitor_only": 1, "not_on_stream_track": 2,
  "by_kind": {"browser_source": 3, "ffmpeg_source": 3, "wasapi_process_output_capture": 2, "coreaudio_input_capture": 1, "game_capture": 1, "wasapi_input_capture": 1},
  "list": [
   {"i": 1, "name": "Mic/Aux", "kind": "wasapi_input_capture", "saved": "Voice", "plays": "Voice", "state": "ok", "tracks": [1, 2]},
   {"i": 2, "name": "Game Capture", "kind": "game_capture", "saved": "Game", "plays": "Game", "state": "ok", "tracks": [1, 3]},
   {"i": 3, "name": "Spotify", "kind": "wasapi_process_output_capture", "saved": "Music", "plays": "Music", "state": "ok", "tracks": [1, 3]},
   {"i": 4, "name": "Discord", "kind": "wasapi_process_output_capture", "saved": "Discord", "plays": "Discord", "state": "ok", "tracks": [1, 2], "monitoring": "monitor and output"},
   {"i": 5, "name": "Alert Box (browser)", "kind": "browser_source", "saved": "Alerts", "plays": "Everything else", "state": "locked", "tracks": [1]},
   {"i": 6, "name": "[user]'s mic", "kind": "coreaudio_input_capture", "saved": "rest", "plays": "Everything else", "state": "rest", "tracks": [], "muted": true},
   {"i": 7, "name": "Tip jar ([email])", "kind": "browser_source", "saved": "new", "plays": "Everything else", "state": "new", "tracks": [1]},
   {"i": 8, "name": "Say \"hi\" \\ now\tplease", "kind": "ffmpeg_source", "saved": "new", "plays": "Everything else", "state": "new", "tracks": [1], "monitoring": "monitor only"},
   {"i": 9, "name": "Key test [REDACTED:stream-key]", "kind": "ffmpeg_source", "saved": "Old fader", "plays": "Everything else", "state": "deleted", "tracks": [1]},
   {"i": 10, "name": "Música 🎵", "kind": "ffmpeg_source", "saved": "Music", "plays": "Everything else", "state": "dropped", "tracks": [1]},
   {"i": 11, "name": "YT Music", "kind": "browser_source", "saved": "Music", "plays": "Music", "state": "ok", "tracks": [3]}
  ]},
 "log": {"total": 9, "lines": [
  {"t": "21:39:02", "level": "info", "msg": "loaded (version 0.4.0)"},
  {"t": "21:43:11", "level": "info", "msg": "uplink started → wss://hub.faders.tv/ingest"},
  {"t": "21:43:12", "level": "info", "msg": "connected to wss://hub.faders.tv/ingest"},
  {"t": "21:43:12", "level": "info", "msg": "your plan includes 4 faders: \"Alerts\" and later are folded into Everything else"},
  {"t": "21:51:27", "level": "info", "msg": "\"Spotify\" → Music"},
  {"t": "21:57:02", "level": "warning", "msg": "disconnected: connect to [ip]:443 failed"},
  {"t": "21:57:03", "level": "info", "msg": "retrying with key [REDACTED:plugin-key]"},
  {"t": "22:01:44", "level": "warning", "msg": "\"Alert Box (browser)\" is in your stream (Track 1) more than once"},
  {"t": "22:01:45", "level": "info", "msg": "settings in /Users/[user]/Library/Application Support/obs-studio"}
 ]}
}
```

</details>
