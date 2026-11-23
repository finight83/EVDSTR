# EVDSTR - Enkelvoud split server, STREAMER half

ESP32-S3. Takes one Opus frame per 20 ms from **EVDCTR** over a 4-wire SPI
link and puts it on the network: unicast fan-out to every registered player,
the RTT clock-sync responder, zones, node statistics, the player-facing API.
It never encodes and never touches an audio input.

```
EVDCTR ──pin link (SPI)──► audioLink (c0) ──► opusQueue ──► udpSend (c0)
                                                                 │
                                                    v2 header + Opus
                                                                 ▼
                                                    players on :4202
```

## Core policy

| Core | Tasks | Rule |
|---|---|---|
| **0** | WiFi/lwIP, AsyncTCP+HTTP, `audioLink` pri 6, `udpSend` pri 3, `loop()` pri 1 | the wire |
| **1** | `players` pri 3 | player management |

`audioLinkTask` is the only consumer of the pin link. `udpSendTask` is the only
sender of audio, and it paces on an absolute 20 ms frame schedule
(`EVDPacer.h`): ordinary lateness never moves the origin, a genuine gap with an
empty queue does. `playerTask` on core 1 does the heartbeat pruning, the 30 s
authoritative time broadcast and the node-stats sweep, so a slow node can never
delay the pacer.

## Wire protocol (unchanged from EnkelvoudServer)

Audio, unicast to each subscriber on **:4202**:

```
offset size field        notes
 0      2   magic        0xE1D0 little-endian
 2      1   version      2
 3      1   flags        reserved
 4      4   seq          LE uint32, monotonic per frame
 8      4   captured_ms  LE uint32, EVDSTR's millis() at the frame instant
12      N   opus payload
```

Ctrl, unicast on **:4203** (JSON). Clock sync, request/response on **:4204**.
Discovery probe on **:4201**.

`seq` and `captured_ms` are stamped **here**, not on EVDCTR: the two chips boot
independently, so the producer's `millis()` means nothing on this side. The
players only need a monotonic capture timeline, which this board's clock
already is.

## Pin link (from EVDCTR)

| STR | direction | CTR | signal |
|---|---|---|---|
| GPIO11 | ← | GPIO11 | SCLK |
| GPIO12 | ← | GPIO12 | MOSI |
| GPIO13 | ← | GPIO13 | IRQ (frame ready) |
| GPIO14 | → | GPIO14 | CS (raise when ready, drop when done) |
| GND | ↔ | GND | **required** |

SPI2 slave, mode 0. The header arrives first, is validated (`magic`,
`version`, `rateK` 24/48, `channels` 1/2), then the payload is clocked to the
length the header declared. CS is raised in every exit path, including a bad
header, so a malformed frame costs one frame rather than a stalled producer.

## Endpoints

The player-facing API is **identical to EnkelvoudServer's**, so no player
firmware changes:

```
POST /api/player/register     registration + heartbeat, compat negotiation
POST /api/node_stats          node counters (latency, queue depth, rebuffers)
POST /api/player/ota_result   node OTA outcome
GET  /api/players  /api/zones
POST /api/zones/create|delete|move|config|remove-node
POST /api/config              volume, mute, mode, buffer latency -> every node
GET  /api/audio/encoding      mirror of EVDCTR's encoder (authority: EVDCTR)
POST /api/source | /api/cmd   relayed to EVDCTR (the source board is wired there)
POST /api/link/pair           set the EVDCTR address
POST /api/ota/upload|apply    accepts an image pushed by EVDCTR
GET  /api/state  /api/stats  /api/log/tail
```

## Files

| File | What it is |
|---|---|
| `EVDLink.h` | wire contract (identical copy of EVDCTR's) |
| `EVDSTR.ino` | constants, POD structs, include order, architecture notes |
| `EVDState.h` | every mutable global |
| `EVDHelpers.h` | locks, subscriber/player/zone table helpers, log ring |
| `EVDWiFi.h` | STA + AP fallback, mDNS, discovery, encoder-mirror pull |
| `EVDLinkSpi.h` | SPI2 **slave** half of the link |
| `EVDPacer.h` | absolute frame schedule |
| `EVDUDP.h` | sockets, `udpSendTask`, ctrl push, clock sync |
| `EVDAudio.h` | `audioLinkTask` (core 0), `playerTask` (core 1) |
| `EVDPlayers.h` | registration, zones, config push, node stats |
| `EVDRoutes.h` | route table |
| `EVDOTA.h` | accepts the image EVDCTR pushes |
| `EVDMain.h` | `setup()` / `loop()` |
| `EVDSTRCTRL.h` | dashboard source; `EVDSTRCTRL_GZ.h` is generated from it |

## Build

Same toolchain and libraries as EVDCTR. After editing `EVDSTRCTRL.h` run
`python gen_gz.py`.

## First boot

1. Flash. AP `EVDSTR` at 192.168.4.1, `/` is the dashboard, join a network.
2. Set the EVDCTR address so this board can mirror the encoder parameters the
   players are told about (it pulls `/api/audio/encoding` every 2 s).
3. Point players at this board's IP (or let their mDNS/discovery find it).
   Registration is the same POST they already send.
