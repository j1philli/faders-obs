# Faders wire protocol (spike, v1)

All binary fields little-endian. One binary WebSocket message = one chunk (500 ms of every stem).

```
offset size field
0      1    version      (1)
1      1    stemCount    (3)
2      4    seq          u32, increments by 1 per chunk
6      8    tsMs         f64, stream clock of the chunk's first sample
14     2    frameMs      u16, Opus frame duration (20)
16     ...  per stem:    u8 stemId (0 voice, 1 game, 2 music)
                         u16 nFrames
                         nFrames × (u16 len, len bytes raw Opus packet)
```

Opus packets are 48 kHz stereo (plugin 0.2.0+; earlier plugins sent mono, which players decode as stereo).

Text (JSON) control messages:

| Direction        | Message                                  | Meaning                                   |
|------------------|------------------------------------------|-------------------------------------------|
| hub → plugin     | `{"type":"listeners","n":3}`             | listener count; plugin pauses uplink at 0 |
| hub → plugin     | `{"type":"quality","kbps":192}`          | kbps per track (plan-capped); default 128 |
| hub → plugin/listener | `{"type":"fader-names","names":{"voice":"Mic","game":"Discord","music":"Music"}}` | display labels on connection and when changed; plugin 0.3.0+ |
| hub → listener   | `{"type":"backlog","count":120}`         | next `count` binary chunks are history    |
| hub → listener   | `{"type":"live"}`                        | following chunks are live                 |

The hub never parses Opus. It reads `seq`/`tsMs` only, keeps a 60 s ring per channel, and fans chunks out byte-for-byte.
