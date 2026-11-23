# EVDCTR / EVDSTR — code review pass

Everything below was found by reading the two trees (`EVDCTR/`, `EVDSTR/`), not by
compiling them: there is no `arduino-cli` on this machine. Each item names the file
and what was changed.

---

## 1. Structural errors that would not compile

| # | Where | Problem | Fix |
|---|-------|---------|-----|
| 1 | `EVDCTR/EVDCTR.ino` | `opus.h` was referenced but never included, and `EVDCTRL_GZ.h`, `EVDUart.h`, `EVDOTA.h` were never included at all — the dashboard, the UART bridge and the OTA routes were dead code. | Added the includes; the dashboard is now actually served and OTA routes are registered. |
| 2 | `EVDSTR/EVDSTR.ino` | `EVDSTRCTRL_GZ.h` and `EVDOTA.h` were never included: EVDSTR served no dashboard and had no OTA routes. | Added both. |
| 3 | `EVDCTR/EVDRoutes.h` | Two `AsyncWebServer server(80);` definitions — a duplicate symbol at link time. | Consolidated to one instance. |
| 4 | `EVDCTR/EVDRoutes.h`, `EVDSTR/EVDRoutes.h` | `#include "EVDOTA.h"` *after* the include of `EVDRoutes.h` in the `.ino` — circular and order-dependent. | Both now declare `setupOtaRoutes()` and the `.ino` includes `EVDOTA.h` once, before `EVDRoutes.h`. |
| 5 | `EVDCTR/EVDUsb.h` | `<EspUsbDevice.h>` included unconditionally inside the `EVD_HAS_USB_DEVICE_LIB` block: on a board where the library is absent, the whole firmware fails to build. | Guarded with `__has_include`; the USB input degrades to a no-op and a `#pragma message` says so. |
| 6 | `EVDCTR/EVDOTA.h`, `EVDSTR/EVDOTA.h` | `sendJson()` used in `EVDOTA.h`, defined later in `EVDRoutes.h`. | Both now send the response directly. |
| 7 | `EVDCTR/EVDCTR.ino` | `loadZonesFromNVS()` stub: zones live on EVDSTR, so the stub was misleading dead code. | Removed, replaced with a comment. |

## 2. Forward-declaration and call-order errors

- **EVDSTR**: `audioLinkTask`, `playerTask`, `pollNodeStats`, `sendCtrlToSubscriber`,
  `pruneStaleSubscribers`, `sendAuthoritativeTimeSync`, `handleClockSyncRequests`,
  `loadZonesFromNVS`, `saveZonesToNVS`, `broadcastNodeConfig`, `sendZoneConfigToIp`,
  `updateNodePerf` were all called by headers that are included before their
  definitions. All are now declared up front in `EVDSTR.ino`.
- **EVDCTR**: `serviceDiscovery`, `serviceReceiverLink`, `serviceUsbAudio`,
  `pollUsbAudioEvents`, `serviceVolumePot`, `load/saveWiFiSettings`,
  `refreshDownlinkCache`, `initDlna`, `setupLinkSpi`, `setup_i2s_in` — same story,
  declared in `EVDCTR.ino`.
- A scan of both trees (`chk3.py`) now reports **0 forward-order issues** and
  (`chk5.py`) **0 duplicate definitions**.

## 3. Logic bugs

- **`EVDSTR/EVDWiFi.h` — magic `-1000`.** `refreshEncoderMirror()` wrote `-1000`
  into `g_currentSignal` for "auto". Anything comparing against `OPUS_SIGNAL_AUTO`
  (0) silently failed. Now uses the named constant `OPUS_AUTO`, defined in
  `EVDSTR.ino` together with the other Opus constants the mirror needs.
- **`EVDSTR/EVDAudio.h` — silence frames.** A silence frame counted as a healthy
  link frame, so `linkHealthy` stayed true with no audio crossing. Silence now bumps
  `statLinkStale`, and `g_opusFrameSamples` / `g_opusChannels` / `g_opusSampleRate`
  are populated from each link frame so the dashboard reports what is *actually*
  being encoded rather than what was configured.
- **`EVDSTR/EVDAudio.h`** referenced `OPUS_MAX_PACKET_BYTES`, which does not exist
  (the constant is `MAX_OPUS_PACKET_LEN`). Fixed.
- **`EVDCTR/EVDUart.h` — `CMD:SRC:BT`.** `bridgeSendReceiverCmd()` prefixed every
  command with `CMD:`, so a caller passing `SRC:BT` produced the invalid
  `CMD:SRC:BT`. It now only prefixes when the caller passed a bare verb.
- **`EVDCTR/EVDDownlink.h` — blocking save from the audio path.** The settings save
  was called from a place where a LittleFS write could stall the capture task.
  Replaced with a `settingsDirty` flag, flushed from `serviceDeferredSettingsFlush()`
  on the main loop.
- **`EVDSTR/EVDPlayers.h` — zone config sent with the wrong volume.** Both
  `handleZonesConfig` and `handleZonesMove` pushed a hardcoded `100` / `false` as
  the zone volume and mute, so moving a player into a quiet zone turned it up to
  full. Both now send the zone's real `volume` / `muted`.
- **`EVDSTR/EVDPlayers.h` — blocking HTTP poll.** Node statistics were gathered by
  an HTTP GET from inside the HTTP handler that serves the dashboard. That is a
  re-entrant blocking call on the same server and can deadlock. Replaced with a
  control-plane `stats_request` datagram every 5 s; players answer on the existing
  control socket.
- **`EVDSTR/EVDPlayers.h` — fragile braced init.** `nodePerf.push_back({...})` and
  `playerNodes.push_back({...})` used positional brace lists containing `String`
  members; one reordering of the struct silently shuffles fields. Both now use
  named-field initialisation.
- **`EVDSTR/EVDUDP.h`** — gap detection flagged a gap on the very first frame after
  a seed (nothing to compare against). Fixed, and pacer health is published through
  one helper instead of being written from two places.
- **`EVDSTR/EVDRoutes.h` — CTR relay had no lock** and `handleLogTail` built JSON
  while holding the ring mutex (a long `String` concatenation inside a critical
  section). The relay is now under `ctrPullMutex`, and `handleLogTail` copies the
  entries under the lock and builds JSON outside it.

## 4. Memory / robustness

- **`EVDCTR/EVDOTA.h` — the STR push.** The whole multipart body (a ~2 MB image
  plus envelope) was assembled on the stack of the HTTP task. It now comes from
  PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, with a short-read check and
  a 120 s timeout for the POST.
- **`EVDSTR/EVDMain.h`** — zones were restored *after* the HTTP server started, so
  an early dashboard request could see an empty zone list. Reordered.

---

## 5. New: Stream tab — live pin-link monitor

The Stream tab now opens with a full-width **Pin link — live** panel, fed by
`/api/state` and `/api/link/state` (both polled every 2 s).

What it shows:

- **health** — `up` / `DOWN`, colour-coded;
- **frames sent** — the encoder's own success counter, not a guess;
- **fps** — measured as a delta of that counter between polls, so a link that is
  "up" but running at 12 fps shows 12, not a green light;
- **last frame** — `now` while frames flow;
- **dropped**, **CS timeouts** — the two failure modes of the SPI link;
- **wire time** — how long the last SPI transfer took (new `statLinkLastFrameUs`,
  published from `EVDAudio.h` on each successful frame);
- a **2-minute sparkline** of frames/s with a 50 fps reference line;
- a **one-line diagnosis** chosen in the order a person would check things:
  CS timeouts → EVDSTR not clocking (power, GPIO11/12/13/14, ground wire);
  drops without CS timeouts → transfer starts but never finishes (long/noisy
  ribbon, starved slave task); no frames at all → no audio running;
  healthy link but zero subscribers → players pointed at the wrong board.

Supporting changes:

- `EVDCTR/EVDState.h` — `statLinkLastFrameUs`;
- `EVDCTR/EVDAudio.h` — publishes it on each good frame;
- `EVDCTR/EVDRoutes.h` — `linkLastFrameUs` and `linkLastOkMs` in `/api/state`;
- `EVDCTR/EVDDownlink.h` — the same fields in `/api/link/state`, and the handler no
  longer calls the blocking `refreshDownlinkCache()` (loop() already refreshes every
  2 s; the dashboard was making a second, stalling request per tick);
- `EVDCTR/EVDCTRL_GZ.h` — regenerated (23 331 B → 7 259 B).

A preview with sample data is served locally; the layout and the monitor are the
real ones.

---

## Still to do

- **Nothing has been compiled.** `arduino-cli` is not installed here. Expect small
  first-compile fixes, mainly around the Opus library signatures in
  `EVDCTR/EVDAudio.h` and `EVDSTR/EVDAudio.h`, and the exact `EspUsbDevice` API in
  `EVDCTR/EVDUsb.h`.
- Push the updated files to both repos.
