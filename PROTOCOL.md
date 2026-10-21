# Faders wire protocol (spike, v1)

All binary fields little-endian. One binary WebSocket message = one chunk (500 ms of every stem).

```
offset size field
0      1    version      (1)
1      1    stemCount    (up to 16: the configured faders + "Everything else"; 3 or 4 in tracks mode)
2      4    seq          u32, increments by 1 per chunk
6      8    tsMs         f64, stream clock of the chunk's first sample
14     2    frameMs      u16, Opus frame duration (20)
16     ...  per stem:    u8 stemId (the index in the plugin's stems message; 0 voice, 1 game, 2 music, 3 everything else in tracks mode)
                         u16 nFrames (25, or 0: this stem is silent for the whole chunk)
                         nFrames × (u16 len, len bytes raw Opus packet)
```

Opus packets are 48 kHz stereo (plugin 0.2.0+; earlier plugins sent mono, which players decode as stereo).
`nFrames = 0` (plugin 0.3.0+) means "present and silent": the stem's whole 500 ms peaked below
−60 dBFS, so no Opus frames are sent for it. Players treat it as a chunk of silence.

Text (JSON) control messages:

| Direction        | Message                                                      | Meaning                                   |
|------------------|--------------------------------------------------------------|-------------------------------------------|
| hub → plugin     | `{"type":"listeners","n":3}`                                 | listener count; plugin pauses uplink at 0 |
| hub → plugin     | `{"type":"quality","kbps":192,"maxStems":4}`                 | kbps per stem (plan-capped); default 128. `maxStems`: faders the plan allows, not counting Everything else (plugin 0.4.0+ folds faders past it into Everything else; 0.3.0 only reads it) |
| hub → plugin     | `{"type":"update","latest":"0.3.0"}`                         | a newer plugin release is published       |
| plugin → hub     | `{"type":"stems","stems":[{"id":0,"name":"Voice"},…,{"id":3,"name":"Everything else","rest":true}]}` | the stem list, on every connect and whenever it changes, before any chunk with that layout. `rest` marks the residual (the streamed track minus the other stems). Without it (plugins ≤ 0.2) the list is voice/game/music, ids 0–2 |
| plugin → hub     | `{"type":"status","streamAudio":"ok"|"silent","track":1,"mixMismatch":"game"}` | the track OBS streams: silent while the Faders layers have sound (0.1.1+); `mixMismatch` (0.3.0+): a layer (tracks mode) or a source name (sources mode, 0.4.0+) sent to Faders that isn't in the streamed track (omitted when fine) |
| hub → listener   | `{"type":"stems",…}`                                         | the plugin's stem list, on attach (before `backlog`) and when it changes |
| hub → listener   | `{"type":"backlog","count":120}`                             | next `count` binary chunks are history    |
| hub → listener   | `{"type":"live"}`                                            | following chunks are live                 |

The hub never parses Opus. It reads `seq`/`tsMs` only, keeps a 60 s ring per channel, and fans chunks out byte-for-byte.

## Everything else (plugin 0.3.0, tracks mode)

While OBS streams, the plugin taps the streamed track unclamped (`allow_clipping`) alongside the
three Faders tracks and sends `track − (voice + game + music)` as stem 3, "Everything else":
alerts, transitions, anything not on a Faders track. With every source on both, it is silent and
costs nothing on the wire (`nFrames = 0`). The sum of all four stems is the streamed track.

The residual is also an oracle: per chunk, the plugin correlates it with each stem. |r| > 0.5 for
three chunks in a row means that stem's audio is in the residual, i.e. the source is on a
Faders track but not on the streamed track (viewers can't hear it from the stream and the
extension can't line it up). The plugin logs it, shows it in Tools → Faders, and reports it in
the `status` message (`mixMismatch`); it clears after three matching chunks. Quiet chunks
(either side below −60 dBFS) are no evidence either way.

## Virtual mixer (faders-obs 0.4, verified with `faders-mixcheck` against libobs 32.2.2)

The plugin will stop consuming OBS tracks and build each Faders layer itself from per-source
audio, plus a residual "Everything else" layer so that Σ layers == the streamed track exactly.
`test/mixcheck.c` (CI target `faders-mixcheck`) proves the libobs behaviour this relies on; it
runs headless with synthetic tone sources and fails the build if any check fails. Findings:

- **Unclamped track.** A raw audio callback (`obs_add_raw_audio_callback`) registered with
  `audio_convert_info.allow_clipping = true` receives the track before clamping (peak 1.6 with
  two 0.8 tones), so `track − Σ layers` is exact. Needs libobs ≥ 30.0 (`allow_clipping` was
  added for OBS 30); the plugin's minimum OBS version becomes 30.
- **Per-source buffers from the raw callback.** `obs_source_get_audio_mix()` returns a source's
  `audio_output_buf` pointers; inside a track-1 raw callback the buffers hold exactly what the
  mixer just summed (volume, mute, push-to-talk, sync offset, balance, track bits already
  applied: an off-track source's buffer for that track is zero). Residual over 2 tones + a
  0.5 volume: 0.0 (bit-exact). The callback runs on the audio thread right after mixing, in
  the same tick, so the buffers are stable until the next tick; hold a strong reference
  (`obs_weak_source_get_source`) per tick so a source can't be destroyed meanwhile.
- **Alignment.** Request `AUDIO_FORMAT_FLOAT_PLANAR`, 48 kHz, stereo so the callback gets the
  mixer's own buffers (no resampler). By the time raw callbacks run, libobs has already
  advanced a consumed source's `audio_ts` to the tick's end (`discard_audio`) and reset a
  scene's to 0. "Mixed from frame 0 of this tick" is therefore: `obs_source_get_audio_timestamp()`
  ≈ tick end **now** and ≈ tick start **at the previous tick** (compare with a few ns of
  tolerance: libobs computes tick times as start + frames_to_ns(total), which rounds
  differently from start-of-tick + frames_to_ns(1024)). A source's very first tick, or the tick
  after a timestamp resync, is left to the residual (≤ 21 ms). Also skip sources for which
  `obs_source_audio_pending()` is true (libobs did not mix them; their buffers are stale).
- **Gating.** `obs_source_active()` is false for a source only in a non-program scene (and for
  one in studio-mode preview: `showing` true, `active` false), yet its buffers still carry audio
  every tick (`render_order` includes every audio source), so the gate is required. Nested
  scenes need no special handling: a child's buffer is what the scene mixed, at the same
  offset. A source that appears twice in the tree is mixed once by libobs
  (`audio_is_duplicated`): count it once.
- **Transitions.** Per-source buffers do not include the crossfade; the transition applies it
  when mixing children. Model it per sample with libobs's own formula
  (`t = (sample_ts − start) / duration`, start = `os_gettime_ns()` when the transition starts,
  fade: A × (1 − t), B × t): residual −113…−119 dBFS during a 300 ms fade. Per-tick
  `obs_transition_get_time()` (video clock) gives only −34 dBFS; no model −11 dBFS. The tick in
  which `obs_transition_start()` runs can be mixed with a half-applied state: up to one tick
  (21 ms) of error, carried by the residual. Stingers: the stinger's audio is a private media
  source mixed inside the transition (`transition-stinger.c`), so it lands in Everything else by
  design (not testable headless; by code reading).
- **Mute / push-to-talk.** `obs_source_set_muted` zeroes the buffers within the tick that
  contains the mute time (all-zero from the next tick, ≤ 21 ms); push-to-talk release delay of
  200 ms shows up as silence 195–206 ms after release. The capture callback's `muted` flag
  (`obs_source_add_audio_capture_callback`) agrees to within a few ms. The plugin needs
  neither: the buffers already carry it.
- **Sync offset** (+200 ms on a source): residual stays 0; the offset is applied inside the
  source's buffering before the buffers are filled.
- **CPU.** Gating + per-sample transition gains + summing 20 sources: p50 5–14 µs, p99 33 µs,
  max 53 µs per 21 ms tick (budget 200 µs).
- **Monitor Only** sources never reach `audio_output_buf` (libobs skips them in
  `source_output_audio_data`), so they are absent from both the track and the buffers.

Phase 3 therefore uses `obs_source_get_audio_mix()` from the track raw callback (not the
capture-callback fallback), with the timestamp rule above for alignment and `obs_source_active`
+ track bits + `audio_pending` as the gate.

## Sources mode (plugin 0.4.0): how the mixer works

`src/mixer.c`. One raw audio callback on the track OBS streams (float planar, 48 kHz stereo,
`allow_clipping`). Per tick, for every audio input source in the collection (a snapshot the main
thread rebuilds on source create/destroy/rename/load, output-channel changes and frontend
events; the audio thread takes a strong reference per source per tick):

1. Gate: `obs_source_active()`, the source's track bits include the streamed track, not
   Monitor Only, not `obs_source_audio_pending()`, `obs_source_get_audio_timestamp()` ≈ tick end
   now and ≈ tick start at the previous tick. Anything gated out contributes nothing (if the
   stream has it, the residual carries it).
2. Gain: 1, or during a transition the per-sample crossfade (A × (1 − t), B × t with
   t = (sample_ts − start) / duration; start from the transition's `transition_start` signal,
   duration from the frontend's setting, `faders_transition_duration` headless, or learned from
   the last fade). The end of a fade is racy to observe: once the audio side reaches t ≥ 1,
   libobs copies whichever scene sits in slot A (the old one, in full, until the video thread
   swaps the slots), the swap deactivates and reactivates the new scene within one call, and
   `transition_stop` can land between the transition's audio render and our callback. So for
   ticks past the fade end, until the old scene's sources are inactive, both candidates (old
   scene in full, new scene in full) are held back and the one closer to the transition
   source's own `obs_source_get_audio_mix()` output that tick is used. A source's first active
   tick goes to Everything else (a scene switched in by a transition is mixed at t ≈ 0 before
   the `transition_start` handler runs). Fixed-length transitions (stingers) are not modelled:
   their audio is the transition's own and lands in Everything else. A source on both sides, or
   appearing twice in the tree, gets gain 1 (libobs mixes it once). `faders-mixtest` case 7
   holds < −40 dBFS during a 300 ms fade in 16/16 runs (−76 to −109).
3. Sum into its fader (`faders_fader` private setting by name; absent = the rule table;
   "" = Everything else; faders past the plan's `maxStems` fold into Everything else).
4. Everything else = unclamped track − Σ faders. Σ(all stems) == the streamed track exactly.
5. Oracle: per ~0.5 s window, the residual correlated with each included source's
   contribution; |r| > 0.5 three windows in a row means the source isn't in the stream the
   way it was included (gating bug, double capture…): it is moved to Everything else for the
   session, logged, shown in Tools → Faders and reported as `mixMismatch` with the source name.

Measured (test/mixtest.c, CI target `faders-mixtest`, libobs 32.2.2, Apple M3 Pro): every
fader exact and Everything else == the unassigned source (±3 %); mute and a 200 ms push-to-talk
release land on the tick; hidden / preview-only / Monitor Only / off-track sources excluded with
nothing leaking; 300 ms fade residual −72 dBFS during the fade (−154 after); sync offset +200 ms
aligned; a duplicated source counted once; two dozen sources p50 42–52 µs, p99 230–320 µs per 21 ms tick;
plan cap folds faders (hub `maxStems` 2 → 2 + Everything else on the wire); an injected
mismatch dropped after 1.3 s.

Rule table for automatic assignment (first match; a fader that isn't configured falls back as
noted): mic inputs (`wasapi_input_capture`, `coreaudio_input_capture`, `pulse_input_capture`,
`alsa_input_capture`) → Voice; app audio capture (`wasapi_process_output_capture`,
`sck_audio_capture`, pipewire app capture) whose name/app/window mentions discord, teamspeak,
slack, zoom, skype or mumble → Discord (else Voice); app capture or media sources
(`ffmpeg_source`, `vlc_source`, `slideshow`) mentioning spotify, apple music, tidal, foobar,
deezer, youtube music, "music" or "playlist" → Music; `browser_source` or anything mentioning
alert, streamelements, streamlabs or sound alert → Alerts (else Everything else); other app
capture, media, desktop audio (`wasapi_output_capture`, `coreaudio_output_capture`,
`pulse_output_capture`), game/window/display capture → Game; everything else → Everything else.
