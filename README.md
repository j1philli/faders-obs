# Faders

A plugin for [OBS Studio](https://obsproject.com). Faders isn't affiliated with or endorsed by the OBS Project.

Sends three audio tracks that you choose and name to the Faders hub so
viewers can mix them. Your Twitch stream is not touched.

**Get it:** download the installer from [faders.tv/streamers](https://faders.tv/streamers/) or the
[latest release](https://github.com/j1philli/faders-obs/releases/latest), and get your key from the
[Faders portal](https://faders.tv/portal/). Setup guide: [faders.tv/docs/streamer-setup](https://faders.tv/docs/streamer-setup/).

- **Tools → Faders** opens the settings (OBS's own properties window, no custom UI code):
  key (with a link to the portal to get one), which three mixer tracks viewers can control, "only
  send while streaming". The server is `wss://hub.faders.tv/ingest` (the key identifies the
  channel); tests point it elsewhere with the `faders_configure` procedure.
- **Track names (0.3.0+):** rename your audio tracks under **Track names** in the [portal](https://faders.tv/portal/).
  The hub sends the same labels to viewers and this plugin. **Tools → Faders → Audio tracks**
  refreshes while open and remembers labels when disconnected. Names change display labels only;
  track assignments and the three audio stem IDs stay the same.
- **Tools → Faders** shows the installed version at the bottom. OBS 32+ also shows it in
  **Tools → Plugin Manager** (`data/manifest.json`, written from the project version by CMake).
- Taps OBS mixer tracks with `obs_add_raw_audio_callback` (default tracks 2/3/4). Assign
  sources in *Edit → Advanced Audio Properties*.
- Stereo Opus at 128 kbps per track (at least Twitch's ~160 kbps stereo AAC), 20 ms frames,
  500 ms chunks (`PROTOCOL.md`). Up to ~390 kbps total upload (less for quiet or mono tracks),
  and **0 while nobody is listening**: the hub tells the plugin its listener count, the
  plugin keeps a 60 s backlog and replays it when the first listener arrives.
- The key goes in an `Authorization: Bearer` header. Reconnects back off 1 s → 30 s, so a wrong
  key doesn't hammer the hub. Status shows in the settings window and the OBS log (`[faders]`).
- Heartbeat: pings the hub every 20 s and reconnects after 60 s without a reply, so a connection
  silently dropped by a router or proxy recovers on its own.
- When the hub reports a newer release, **Tools → Faders** shows "Update available" with a link
  to the download page, until you install that version. The hub only says so while the plugin
  is connected (by default, while you're live), so the plugin remembers it and opens Tools →
  Faders once at OBS's next start. The plugin never updates itself.
- Scriptable: procedures `faders_configure(server, key, voice, game, music, only_live)` and
  `faders_status(out status)` on the global proc handler.

## One plugin, two platforms

Everything is shared C except the WebSocket transport (`src/ws.h`):

| | macOS | Windows |
|---|---|---|
| Transport | `ws-apple.m` (NSURLSessionWebSocketTask) | `ws-win.c` (WinHTTP WebSocket API) |
| Shared | `plugin-main.c` (settings, Tools menu, events), `uplink.c` (capture, Opus, chunks, backlog, reconnect) | same |
| Output | `faders.plugin` bundle | `faders.dll` + `data/` |
| libobs | links OBS.app frameworks | import libs generated from the release DLLs |

## Install

Download the installer for your platform from the
[latest release](https://github.com/j1philli/faders-obs/releases/latest) and run it, then restart
OBS and open **Tools → Faders**:

- **macOS 13+** (Apple silicon and Intel): `faders-obs-<version>-macos.pkg`. Installs for your
  user into `~/Library/Application Support/obs-studio/plugins`. Signed with a Developer ID and
  notarized by Apple.
- **Windows 10/11 x64**: `faders-obs-<version>-windows.exe`. Installs into
  `%ProgramData%\obs-studio\plugins\faders` by default; for portable OBS 33, pick
  `<OBS folder>\plugins` on the folder page instead (an upgrade reuses the last folder, and moving
  removes the old copy). Setup and uninstall wait for OBS to close. Uninstall from Settings →
  Apps. The DLL is placed
  both where OBS 33 looks (`faders\faders.dll`) and where OBS 32 looks (`faders\bin\64bit\`), so
  one installer works with both and OBS 33 doesn't flag it as a legacy plugin. The plugin DLL,
  installer and uninstaller are code-signed (Azure Trusted Signing).
- **Zips**, to install by hand (portable OBS, scripted setups): `faders-obs-<version>-windows.zip`
  holds a `faders` folder to extract into `%ProgramData%\obs-studio\plugins` (or
  `<OBS folder>\plugins` for portable OBS 33); `faders-obs-<version>-macos.zip` holds
  `faders.plugin` (signed and notarized) for `~/Library/Application Support/obs-studio/plugins`.
  Close OBS first; remove the folder to uninstall.

## Build (macOS)

```sh
./scripts/fetch-deps.sh     # OBS 32.2.2 (Apple + Intel releases), matching headers, SIMDe, static libopus
./scripts/build-macos.sh    # universal build/faders.plugin (+ arm64 headless test host); FADERS_INTEL=0 for arm64 only
./scripts/install-macos.sh  # → ~/Library/Application Support/obs-studio/plugins
./scripts/package-macos.sh  # → faders-obs-<version>-macos.pkg (signs/notarizes when MACOS_* are set; see the script)
```

## Build (Windows x64, MSVC)

From a "Developer PowerShell for VS 2022":

```powershell
.\scripts\fetch-deps-windows.ps1              # OBS 32.2.2 DLLs → .lib, headers, static opus
cmake -S . -B build -A x64; cmake --build build --config Release
.\scripts\install-windows.ps1                 # (as admin) → %ProgramData%\obs-studio\plugins\faders
```

Cross-compiling from macOS or Linux (how releases are built): `./scripts/cross-windows.sh` (MinGW-w64)
→ `build-win/faders.dll`; `scripts/ci/check-windows-dll.py` verifies its exports and imports.

Installer: `makensis -DVERSION=0.2.0 -DDLL=<path>/faders.dll -DOUTFILE=<path>/faders-obs-0.2.0-windows.exe installer/windows/faders.nsi`
(NSIS 3, on Windows or Linux). The installer art in `installer/` is generated from the Faders
brand kit.

CI runs on TeamCity (`scripts/ci/`): `teamcity-macos.sh` builds natively, runs the end-to-end
test below against `test/mock-hub.mjs` and packages (`.pkg` + zip; on `main` also Developer ID
signed and notarized). `teamcity-linux-cross.sh` (in `ubuntu:24.04`) cross-builds the Windows
release with MinGW, checks the DLL statically and builds the `.exe` installer and zip; on `main`
it signs the DLL, installer and uninstaller with Azure Trusted Signing (`scripts/sign-windows.sh`).
The signed `main` builds become a draft GitHub release, which is published by hand.

## Test without the OBS GUI

`test/mock-hub.mjs` is a small stand-in for the Faders hub (same protocol, see `PROTOCOL.md`):

```sh
npm ci && node test/mock-hub.mjs &                       # ws://127.0.0.1:8787, key "dev-token"
build/faders-headless ws://127.0.0.1:8787/ingest/ci_channel dev-token \
  test/media/voice.wav test/media/game.wav test/media/music.wav 40 &
node test/plugin-listener.mjs --channel ci_channel --seconds 20   # PASS/FAIL
```

### Headless host

`build/faders-headless` runs real libobs, plays three WAVs into tracks 2/3/4, loads the plugin
and configures it through its procedures:

```sh
build/faders-headless ws://localhost:8787/ingest/<channel> <fdr_live_key> \
  test/media/voice.wav test/media/game.wav test/media/music.wav 60
```

Verified: wrong key → "server rejected connection (HTTP 401)" with backoff; correct key →
pauses at 0 listeners, resumes and replays backlog on the first listener; Chrome decodes every
frame and each decoded layer correlates 0.97–0.997 with its own source and ≤0.37 with the others.

## Tested

Released as 0.2.0. Tested end to end by hand in the OBS app (installer, **Tools → Faders**,
streaming, listening on faders.tv) on:

- **macOS** on Apple silicon
- **Windows** x64

### Not tested yet

Bug reports are welcome (GitHub issues, or the Faders Discord: discord.gg/TA7Xd8CEAz).

- **Intel Macs:** the universal build links each architecture against its own OBS release, but
  the x86_64 half hasn't been run yet (no Intel Mac / Rosetta in CI).
- **Linux:** not supported yet. It needs a libcurl transport (`src/ws-curl.c`); everything else
  is shared.

## License

GPL-2.0-or-later (see `LICENSE`), the same as OBS Studio, which this plugin links against.
Bundled: Opus (BSD-3-Clause). The Faders hub, website and browser extension are separate
programs that talk to this plugin over the network protocol in `PROTOCOL.md`.

Name synchronization regression test (after building): `node test/fader-names.mjs`. It loads the real plugin in libobs, receives Unicode labels, rejects an excessive name, applies a live rename and checks the saved local labels.

Update notice regression test (after building): `node test/update-notice.mjs`. It checks that the hub's "newer release" notice is saved locally, so Tools → Faders shows it after the stream ends, and that a malformed version is ignored.
