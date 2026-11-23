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

## Status of this second pass

Items 6.1, 6.2, 6.3 and 6.6 are **fixed** in the trees (see the fix log below).
6.4, 6.5 and 6.7 are still findings. 6.5 (the USB API) is the one that blocks a
build.

### Fix log

**6.1 request-body race - fixed, both boards, 18 handlers.**
`accumulateReqBody()` now keeps the buffer in `request->_tempObject`, so no two
requests can share it, and a new `releaseReqBody()` frees it. Every handler
releases it: the ones that parse JSON release immediately after parsing (the
document owns its own copy), the DLNA handlers release after `String(body)` is
built, and `handleSourceRelay` on EVDSTR releases only after `http.POST()` has
actually sent the body upstream, with a release on each of its three early
returns.

**6.2 zone table wiped on reboot - fixed.** Added `saveZonesToNVSLocked()`,
which serialises `zones` directly instead of calling `snapshotZones()` (which
needed the mutex its caller already held, and therefore returned an empty
vector). All five zone handlers and the auto-assign path in
`handlePlayerRegistration()` now save under the lock they hold.
`LOGI()` moved out of the `zonesMutex` critical section.

**6.3 OTA pushes to any host - fixed.** `isPairableHost()` on both boards
accepts only an IPv4 address, `host:port` or a plain hostname. It is checked
when pairing (EVDCTR `/api/link/pair`, EVDSTR `/api/link/pair`) and again in
`otaPushSpoolToStr()` before the firmware POST, so a stale NVS value cannot
bypass it.

**6.6 two link-down thresholds - fixed.** EVDSTR's dashboard and `/api/state`
now use `LINK_STALE_DROP_MS` (400 ms), the same constant `audioLinkTask` uses,
instead of a hardcoded 200 ms.

**Also fixed while in there:**

- `handleOtaResult()` on EVDSTR returned early on any segment after the first
  without ever sending a response, so a fragmented player report left the
  request hanging until the player timed out. It now answers every segment.
- **6.4** EVDCTR's deferred flush no longer rewrites the WiFi credentials for
  a pairing change; only `handleWiFiSave()` writes them.
- **6.7a** `EVDUart.h` now logs a line it drops for being too long, rate-limited
  to once per 5 s, instead of silently losing a `BT_JSON|` payload.
- **6.7b** `refreshEncoderMirror()` moved off `loop()` into its own task
  (`encoderMirrorTask`, core 0, priority 1, 2 s period). A 1.5 s timeout on an
  unreachable EVDCTR used to stall discovery, the settings flush and clock sync
  with it.

Only **6.5** (the `EspUsbDevice` API in `EVDCTR/EVDUsb.h`) is still open, and
it is the one that blocks a build.

---

## Still to do

- **Nothing has been compiled.** `arduino-cli` is not installed here. Expect small
  first-compile fixes, mainly around the Opus library signatures in
  `EVDCTR/EVDAudio.h` and `EVDSTR/EVDAudio.h`, and the exact `EspUsbDevice` API in
  `EVDCTR/EVDUsb.h`.
- Push the updated files to both repos.

---

## 6. Second pass - concurrency and control-plane defects

### 6.1 `accumulateReqBody()` breaks overlapping requests (both boards, 19 handlers)

```cpp
inline bool accumulateReqBody(...) {
  static String slot;          // ONE buffer for every handler
  if (index == 0) { slot = ""; ... }
```

Every JSON POST handler in both trees calls this same function, so they all
share one `static String`. The comment above it claims the AsyncTCP worker is
single-threaded and one request's segments are serialised - true, but the
buffer is not per-request: request A's body is still being accumulated while
request B's first segment arrives and does `slot = ""`. A's handler then gets
B's bytes, or half of each.

It is reachable in normal use, not just in theory: a player's
`POST /api/player/register` every few seconds lands on the same worker as a
dashboard `POST /api/config`, and the EVDCTR push from the Stream tab runs
concurrently with the 2 s encoder-mirror pull. A spliced JSON body means a
dropped config change at best and a wrongly parsed zone config at worst.

Fix: keep the buffer on the request object, so two requests cannot share it.

### 6.2 EVDSTR's zone config is mutated outside the mutex, then read inside it

In `handlePlayerRegistration()`:

```cpp
if (lockFor(zonesMutex, 80)) {
  ...
      z.nodeIps.push_back(ip);      // grows the vector
      settingsDirty = true;
      settingsDirtySince = millis();
      LOGI(...);                     // inside the critical section
      break;
  ...
  unlockFor(zonesMutex);
}
```

`LOGI()` writes to the log ring under `gLogRingMux` while `zonesMutex` is
held - a nested critical section taken from the HTTP worker, which is exactly
the pattern that produces a lock-order stall. The log line belongs after the
unlock.

More serious is what happens next: `settingsDirty = true` makes `loop()` call
`saveZonesToNVS()` 1.5 s later. That takes a **snapshot** under `zonesMutex`
and then writes NVS - correct. But `handleZonesMove()` and friends call
`saveZonesToNVS()` *while already holding* `zonesMutex` (lines 481, 508, 534,
580, 605). `snapshotZones()` then fails to take the same (non-recursive)
mutex, returns an **empty vector**, and the write persists an empty zone
table. The zones are gone after a reboot.

### 6.3 EVDCTR OTA pushes an image to whatever the operator typed

`EVDDownlink.h` accepts any `host` string, strips `http://` and trailing
slashes, and stores it. `EVDOTA.h` then POSTs a firmware binary to
`http://<host>/api/ota/upload` with no check that the target is EVDSTR.

### 6.4 EVDCTR's deferred flush writes WiFi settings for every dirty flag

`serviceDeferredSettingsFlush()` in `EVDMain.h` calls `saveWiFiSettings()`
unconditionally whenever *any* setting is dirty. Pairing with EVDSTR sets
`settingsDirty`, so a pairing change also rewrites the WiFi credentials for no
reason. Harmless today (the values are unchanged), but it means one dirty flag
drives two unrelated writes; it should be two flags.

### 6.5 `EspUsbDevice` API is not the one this code calls

`EVDCTR/EVDUsb.h` calls `setVid`/`setPid`/`setProduct`/`setManufacturer`/
`begin()`/`read()`/`connected()` on `EspUsbDevice`, and expects
`read(buf, cap)` to return captured PCM. The unsplit original
(`srv_EVDUsb.h`) uses a different shape entirely: `new EspUsbDevice()`,
`EspUsbAudioFunction`, `addPlaybackStream()`, `addFormat({rate, ch, 2, 16})`,
`begin(EspUsbDeviceConfig{vid, pid, ...})`, and a mutex-guarded running flag.

So the split's USB input is a stub that will not compile against the library
the project already depends on. Either port the original block verbatim, or
drop the USB source until it is ported. **Do not ship the current file** - it
is worse than no USB support because it looks like it works.

### 6.6 EVDSTR uses two different definitions of "link is down"

- `audioLinkTask` calls the link stale only after **400 ms** of no frames
  (`LINK_STALE_DROP_MS`).
- `EVDRoutes.h` and the dashboard call it down at **200 ms**
  (`s.linkGapMs < 200`).

With the 20 ms frame cadence, a single lost frame is 40 ms and neither
notices, but a 250 ms hiccup shows "link idle" on the dashboard while the
audio task still considers the link live and keeps the pacer anchored. Pick
one threshold and use it in both places.

### 6.7 Small ones

- `EVDUart.h` reads the source board's lines into a 1200-byte buffer and
  **silently discards** a longer line (`else lineLen = 0;`). A `BT_JSON|`
  payload larger than that vanishes with no log line, and the dashboard's BT
  device list just stops updating. Log it.
- EVDSTR's `refreshEncoderMirror()` holds `ctrPullMutex` for the whole HTTP
  GET (1.5 s timeout). `loop()` blocks for that long every 2 s when EVDCTR is
  unreachable - the discovery responder and the settings flush stall with it.
  The mirror should be moved to its own task, or the GET should skip while a
  previous one is still outstanding.
- `EVDCTR/EVDWiFi.h` `handleNetworkTestStatus()` starts the test on
  `?start=1` but returns `running:true` even when a test is *already*
  running, so the dashboard's "testing..." poll can latch onto an older
  result.

---

## 7. It builds

Toolchain pinned to **esp32:esp32 3.3.0** (4.0.0 removed `driver/i2s.h`, which
this firmware uses) and **ArduinoJson 6.21.5** (`DynamicJsonDocument` is
deprecated in 7.x). Libraries: ESP Async WebServer 3.12.1, Async TCP 3.6.1,
esp32_opus 1.0.3.

| | sketch | flash | RAM |
|---|---|---|---|
| EVDCTR | 1 452 592 B | 8% of 16 MB | 95 444 B (29%) |
| EVDSTR | 1 237 136 B | 7% of 16 MB | 66 380 B (20%) |

**Build command** (the partition scheme matters, see below):

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=custom" \
  --build-property "build.partitions=partitions.csv" \
  EVDCTR
```

### Errors the first real build found (all fixed)

| Where | Error | Cause |
|---|---|---|
| `EVDCTR/EVDWiFi.h` | `otaInProgress` / `otaUploadInProgress` not declared | EVDOTA.h is included after EVDWiFi.h; added `extern` declarations |
| `EVDCTR/EVDAudio.h` | `statPendingBufDroppedLastSec` not declared | the 1 s rollover variable was used but never defined |
| `EVDCTR/EVDOTA.h` | `server` not declared | the server instance lives in EVDRoutes.h; added `extern AsyncWebServer server` |
| `EVDCTR/EVDRoutes.h` | cannot convert `DeserializationError` to `bool` | `bool x = deserializeJson(...)` needs `!= DeserializationError::Ok` |
| `EVDCTR/EVDRoutes.h` | `networkSettingsPage` not declared | `EVDNTW.h` was documented but never included - the whole `/settings` AP-fallback page was dead code |
| `EVDSTR/EVDState.h` | `statUdpSent` not declared | two statements had been joined onto one line by an earlier edit, so the declaration was inside a comment |
| `EVDSTR/EVDPlayers.h` | `otaInProgress` / `otaUploadInProgress` not declared | same include-order issue as EVDCTR |
| `EVDSTR/EVDOTA.h` | `server` not declared | same as EVDCTR |

### The partition trap

Without `PartitionScheme=custom`, arduino-cli silently ignores `partitions.csv`
and uses the *default 4 MB* layout. EVDCTR is 1.45 MB, which is **110%** of the
1.31 MB app slot in that default scheme, so the build fails with
`text section exceeds available space in board` - a message that says nothing
about partitions. With the custom scheme both boards report 8% of 16 MB.

### Remaining warnings (not errors)

- `build_opt.h` redefines `ARDUINO_RUNNING_CORE` and four `CONFIG_*` macros that
  the platform's `sdkconfig.h` already sets. Harmless, but the `-D` values are
  *not* taking effect - the platform wins. If those pins matter, they have to be
  set in a custom board variant, not in `build_opt.h`.
- `EVDAudio.h` uses the legacy I2S driver's `dma_buf_count` / `dma_buf_len`,
  deprecated in 3.x. Still present in 3.3.0, gone in 4.x.
- Two `/*` inside comments in `EVDCTR.ino`'s header block.
