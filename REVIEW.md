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

---

## 8. Automatic CTR/STR discovery

Pairing used to be manual in both directions: you typed EVDSTR's IP on the
controller and EVDCTR's IP on the streamer. Both boards already answered the
`ENKELVOUDP/1 DISCOVER` probe, but **nothing ever sent it** - `DISCOVERY_MAGIC_REQ`
was only ever compared, never broadcast. So the responder was dead code and the
only way to pair was to type two addresses.

### What it does now

One UDP socket on port 4201, used in both directions:

| direction | CTR | STR |
|---|---|---|
| answers probes | `role=ctr name=... ip=...` | `role=str name=... ip=... ctr=...` |
| probes while unpaired | every 3 s (`DISCOVERY_TX_MS`) | every 3 s |
| on hearing the partner | takes `role=str`'s `ip`, **verifies it over HTTP**, saves it | takes `role=ctr`'s `ip`, saves it |

- The probe goes to the **subnet broadcast** (`ip | ~mask`), not
  `255.255.255.255`, which most APs and every client-isolation rule drop.
- CTR only auto-pairs when `strHost` is empty. A stored address is a deliberate
  choice and a stray board on the LAN must not overwrite it. The manual
  `/api/link/pair` on the Stream tab still wins.
- CTR does not trust the reply: it calls `/api/state` on the address first, and
  only keeps it if that answers. Otherwise it keeps probing, so it cannot lock
  onto a half-booted board.
- EVDSTR never pairs *itself*: the controller owns the pairing (it is the side
  that pushes audio and OTA). EVDSTR just becomes findable and records where its
  controller is, so the encoder mirror and the status push have a destination.
- Both dashboards say what is happening: the CTR Stream panel shows
  `Paired by: discovery | typed` and `Probes sent`, the pill reads
  `STR searching...` while unpaired, and the diagnosis line explains that guest
  WiFi / client isolation blocks discovery. EVDSTR's page shows the probe count
  in place of a bare "not paired".

### Also fixed

`chk3.py`, the forward-order checker, counted a header as "declared" only if the
prototype was in the `.ino`. Prototypes in headers were invisible, which hid
real ordering bugs. It now sees prototypes in any header, and ignores macros
(`LOGI`, `portENTER_CRITICAL`, `pdMS_TO_TICKS`) that it had been mistaking for
function calls. Both trees report 0.

### Build after the change

| | sketch | flash | RAM |
|---|---|---|---|
| EVDCTR | 1 454 191 B | 8% | 95 452 B (29%) |
| EVDSTR | 1 238 303 B | 7% | 66 380 B (20%) |

---

## 9. USB audio ported to EspUsbDevice 3.x

`EVDUsb.h` was a stub that called `setVid()`, `begin()` and `connected()` on
`EspUsbDevice` - methods that never existed. It could not compile, so USB was
compiled out and the source selector answered 501. It is now a real port.

### The library changed under the old code

EspUsbDevice **3.x is a rewrite**, not an evolution. The old block in
`srv_EVDUsb.h` targets 2.x, and every one of these is different now:

| 2.x | 3.x |
|---|---|
| `CFG_TUD_*` build flags size the buffers | `configure()` sizes them at run time |
| `device.task()` called from `loop()` | the library runs its own worker task |
| `audio.begin()` / function `begin()` | only `device.begin()`; a Function's is private |
| buffers reserved at link time | allocated in `begin()`, freed in `end()` |
| `ready()` | `started()`, `ready()`, `suspended()`, `speed()` |

Order matters and is enforced: **declare -> `addFormat()` -> `configure()` ->
`device.begin()`**. `addFormat()` and `configure()` are refused while running,
and the Feature Unit accessors (`hasVolume`, `getVolumeRange`, ...) are only
valid after `begin()` has built the audio graph.

### What the port does

- UAC1 **playback** (host -> device), 48 kHz / 16-bit / stereo - the same rate
  as the I2S path, so the resampler ratio downstream stays valid.
- 960-frame ring (20 ms), `DropOldest`: for a live input, stale audio is worse
  than a gap.
- `addFormat()`'s return value is **checked** - it is false for a shape whose
  packet would exceed the 1023-byte full-speed limit, which is the one failure
  that otherwise shows up only as silence.
- `begin()` failure logs `device.lastErrorName()`, which distinguishes
  `ESP_ERR_INVALID_SIZE` (endpoints/FIFO do not fit) from
  `ESP_ERR_INVALID_STATE` (wrong USB mode - needs USB-OTG/TinyUSB).
- The host's volume/mute is **applied here**: the library reports it as events
  and never touches the PCM, so without this the Windows slider would be inert.
  Start at -20 dB, not full scale.
- `stopServerUsbAudio()` takes the lock with a 500 ms bound and, on timeout,
  leaves the device up rather than calling `end()` under an in-flight `read()`.

### What the dashboard shows

`/api/state` gained `usbHostVolumeDb`, `usbHostMuted` and `usbError`. The
capture panel reads `waiting for host (begin_failed)` instead of a bare `-`
when the source is USB but no PCM is arriving, and shows the host's volume.

### Build

EVDCTR 1 488 411 B (8% of 16 MB), RAM 114 604 B (34%). The USB stack costs
about 36 KB of flash and 19 KB of static RAM.

**Board settings that matter:** `USBMode=default` (USB-OTG / TinyUSB), *not*
Hardware CDC and JTAG - the library owns the OTG controller and cannot coexist
with the core's `USB.begin()`. D- is GPIO19, D+ is GPIO20: the second USB-C
port on the board, the one not labelled UART.

---

## 10. Third pass — a sceptic reads the fixed code

The first two passes fixed what was broken. This one went back over the result
and assumed the fixes were wrong too. Nine defects, six of them in code that
had just been "fixed".

### 10.1 `releaseReqBody()` turned the parse into a use-after-free (both boards)

`accumulateReqBody()` hands back a pointer **into** the buffer that
`releaseReqBody()` frees. Four handlers did

```c
bool parsed = !deserializeJson(in, body, bodyLen) && in["host"].is<const char*>();
releaseReqBody(request);
```

which is fine (the order of evaluation is left-to-right), but `EVDSTR`'s
`handleApiConfig` and `handlePairCtr` did

```c
bool badBody = deserializeJson(in, body, bodyLen) || !in["x"].is<const char*>();
releaseReqBody(request);
if (badBody) ...
```

and five EVDSTR zone handlers did

```c
bool badBody = (deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["id"].is<...>();
```

ArduinoJson's `operator!=` against `DeserializationError` is not a member of the
parse call, so the two halves of that expression have **no sequencing** between
them and the release is another full-expression later — but the real fault is
simpler and worse: `releaseReqBody()` frees the memory, and every `in["..."]`
read afterwards still consults the *document*, not the body, so it survived only
because nothing in between reallocated. It is a latent heap corruption on every
zone mutation and every POST /api/config.

**Fixed:** every handler now captures `DeserializationError bodyErr =
deserializeJson(...)` into a named variable on its own statement, reads the
field it needs while the body is still alive, and releases after. In EVDSTR the
field checks moved to their own statement too (`if (!badBody &&
!in["name"].is<const char*>()) badBody = true;`) so nothing straddles the
release. `EVDCTR`'s `/api/log/level` had the same shape and now reads
`in["level"]` before the release as well.

### 10.2 EVDSTR answered a fragmented body by returning without a response

`handlePlayerRegistration` and `handleNodeStats` did `if (index != 0) return;`
with a comment claiming the node would retry. It cannot: AsyncWebServer sends
`413` and **aborts the connection** on that segment, so the node sees a socket
reset, and the second and later segments of the same request are never answered
at all — a player stalls until its own timeout, every heartbeat.

**Fixed:** one fragment policy for the whole board,
`bodyIsFragmented()` / `answerFragmentedBody()` in `EVDHelpers.h`: the first
segment gets the `413` verdict, any later segment is answered `200
{"dropped":true}` so nothing is left hanging. `handleOtaResult` uses it too
instead of its own hand-rolled version.

### 10.3 Five zone handlers lost their required-field checks

When `deserializeJson` was moved out of the `badBody` expression, the
`!in["name"].is<const char*>()` half went with it: a POST of `{}` to
`/api/zones/create` created a zone named `""`, and `/api/zones/delete` with
`{}` was a silent no-op that returned `ok`. Both restored, one per handler,
on their own statement.

### 10.4 EVDSTR's deferred flush could resurrect a deleted zone

`serviceDeferredSettingsFlush()` ran `saveZonesToNVS()` — which takes a
*snapshot* of the zone table — three seconds after the last settings change, on
the loop task, while a dashboard request could be mutating the same table on the
AsyncTCP task. Delete a zone, then a flush that snapshotted a microsecond
earlier writes it back to NVS: the zone returns after the next reboot. Every
zone mutation already persists under `zonesMutex` via `saveZonesToNVSLocked()`,
so the flush now writes only the WiFi/settings block. (This is the same class of
bug as 6.2, from the other side.)

### 10.5 A relay mutex leaked on one path

`handleSourceRelay()` on EVDSTR took `ctrPullMutex` and released it on four of
five exits. A fragmented POST calls the handler twice: the first segment
returned `502` *before* taking the lock, the second took the lock and failed
`http.begin()`, and that `return` did not unlock. From then on the relay
answered `503 relay busy` until reboot. Replaced with an RAII guard so no path
can miss it.

### 10.6 `clientMs: 0` got no clock-sync reply

`in["clientMs"].is<uint32_t>()` is **false** for `0` in ArduinoJson 6: an
unsigned type rejects any integer it cannot prove non-negative, and a JSON `0`
is stored as a signed integer. A player that had just booted (`clientMs == 0`)
was counted as a bad request and got no `sync_resp`, so its very first sync —
the one that matters — always failed. Now accepts any integer that fits.

`handleClockSyncRequests()` also returned `void` and handled exactly one
datagram per call while `loop()` slept; a fleet of nodes plus any stall (an OTA
flash write is ~1 s of no loop()) overflowed the socket's RX buffer and lwIP
dropped the rest silently. It now returns "a packet was handled" and `loop()`
drains up to four per pass with a 1 ms sleep.

### 10.7 DLNA skipped a hard-coded 44 bytes of WAV header

Every WAV written by an encoder with metadata puts `LIST`/`fact`/`bext` chunks
between `fmt ` and `data`, so the real header is 60–400 bytes, not 44. The
fetch task skipped 44 and fed the rest of the *header* into `rawQueue` as PCM:
a burst of noise at the start of every DLNA track. It also never read the
sample rate, so a 44.1 kHz file was resampled as if it were 48 kHz (audible
pitch error) and mono files were read as interleaved stereo (wrong speed and
garbage).

**Fixed:** `parseWavHeader()` walks the chunk list (`fmt `, then any number of
other chunks, then `data`), returns the true skip offset, and accepts only
16-bit PCM or `WAVE_FORMAT_EXTENSIBLE` with 16 bits at 8–192 kHz. It sets
`g_sourceSampleRate` and resets the rate estimator, so a 44.1 kHz file is
resampled from 44.1 kHz. A header that straddles two socket segments is
finished off (`headerSkipRemaining`) instead of aborting the stream. Mono is
duplicated into L/R, because everything downstream is hard-wired to stereo.

### 10.8 `ResamplerToOpusRate::process()` read out of bounds on a 1-frame chunk

The cubic needs samples `i-1` … `i+2`. With `inFrames == 1` the loop condition
`pos_ < inFrames - 1` is `pos_ < 0` and never runs, but the priming block then
reads `in[1]`, which for a mono-in-a-stereo-buffer chunk is one sample past the
end. Guarded with `if (inFrames < 2) return 0;`.

### 10.9 Small ones

- `handleEncoderRecreate()`'s stale-frame skip in `audioEncodeTask` dropped
  forever with no log if the producer's frame size stopped matching the
  encoder's (a stuck `g_opusFrameSamples`): the link stayed "up" and the
  players were silent with nothing in the log. Now counted
  (`statOpusDropped`) and logged every 2 s.
- Both boards' factory-reset path called `ESP.restart()` with a live
  `PrefsGuard`: `restart()` never returns, so the destructor never ran and the
  namespace was never closed or committed. Scoped so the guard dies first.
- `chk5.py` (duplicate-definition scan) reported six false positives in
  `EVDUsb.h`, because it read both halves of an `#if`/`#else`. It now strips the
  inactive branch; both trees report 0 again.

### Build after the third pass

| | flash | RAM |
|---|---|---|
| EVDCTR | 1 489 295 B (8% of 16 MB) | 114 636 B (34%) |
| EVDSTR | 1 270 427 B (7% of 16 MB) | 85 532 B (26%) |

EVDSTR's RAM grew by 19 KB: `bodyIsFragmented()`/`answerFragmentedBody()` are
`inline` in a header included before `EVDPlayers.h`, which is where they had to
go for the include order to work.

Still hardware-only, unchanged from §9: the pin link between two real boards,
Opus at runtime, USB enumeration, and discovery on a LAN that does not isolate
clients.

## 12. Fifth pass - a serial monitor that says what the board is doing

The report was "I flashed EVDCTR and I do not see it as an AP". Nothing in the
firmware said *why*: the boot log was six `LOGI` lines, they were filtered by
a log level that had no meaning yet, and the one line that mattered - "AP
fallback ... 192.168.4.1" - was a `LOGW` that also vanished once the level
settled. A board with no Wi-Fi looked the same on serial as a board with no
code in it.

| # | Where | Change |
|---|-------|--------|
| 12.1 | both `EVDMain.h` | **Boot banner, printed with `Serial.printf`, never filtered.** Board, firmware version, reset reason in words, chip/flash/PSRAM/heap, dashboard gzip size, encoder constants, pin-link map, discovery port, and whether USB audio is actually compiled in. |
| 12.2 | both `EVDMain.h` | **The radio reports itself before anything else can fail.** Stored name/SSID/pairing, then either `connected ... ip ... rssi` or `AP FALLBACK: ssid '...', open (no password), http://192.168.4.1/ channel N` **with the reason**: no credentials stored, or "could not join 'x' within 3000 ms (status N)". |
| 12.3 | both `EVDWiFi.h` | `WiFi.softAP()` returning false was ignored - the board then had no STA and no AP and said nothing. Now logged as a failure on serial and in the log ring. |
| 12.4 | both `EVDMain.h` | **A saved SSID with an empty password is called out at boot.** That is the usual reason a board the operator believes is configured opens its own AP. |
| 12.5 | both `EVDMain.h` | **10 s heartbeat** at level 1: uptime, heap, Wi-Fi/AP + RSSI, source, encoder frames, link health and counters, pairing. A box that is up but doing nothing now says so once every ten seconds. |
| 12.6 | both `EVDLinkSpi.h` | `logLinkPeerState()`: at boot and whenever the link is idle, prints the IRQ/CS levels and the frame counters, so "link IDLE" is not the first sign of a wiring fault. |
| 12.7 | both `EVDAudio.h` | Every task/queue creation failure now prints which one failed and the free heap, plus the task->core layout once at boot. |
| 12.8 | `EVDCTR/EVDAudio.h` | **`encProbeTask`**: with no EVDSTR attached, prints encoder frames per 5 s and whether the partner is clocking them. This is how a lone CTR proves its whole capture -> resample -> opus chain without a second board. |

### Can each board boot on its own?

Yes, and this pass makes that verifiable rather than assumed:

- **EVDCTR alone**: no stored SSID -> opens AP `EVDCTR` at 192.168.4.1 (open,
  no password) and serves the dashboard there. No EVDSTR -> discovery probes
  every 3 s, pairing stays "(unpaired)", and `encProbeTask` reports the encoder
  running and its frames being dropped. The pin link initialises as master with
  nothing on the other end; `spi_bus_initialize` does not wait for a slave.
- **EVDSTR alone**: same AP fallback (`EVDSTR`), dashboard on 192.168.4.1,
  discovery probes every 3 s, players register and are served silence-free
  (the pacer has nothing to send). Its link is a slave - it only clocks when
  EVDCTR drives IRQ - so with no partner `audioLinkTask` blocks on its IRQ
  wait and the heartbeat prints `link <gap> ms` with the GPIO levels.
- Neither board blocks on the other at boot: no HTTP call, no SPI transfer and
  no UDP handshake is on the `setup()` path.

### Build after this pass

| Board | Flash | RAM | Audits |
|-------|-------|-----|--------|
| EVDCTR | 1 558 823 B (9 %) | 115 284 B (35 %) | chk3 0, chk5 0 |
| EVDSTR | 1 276 351 B (7 %) | 85 532 B (26 %) | chk3 0, chk5 0 |
