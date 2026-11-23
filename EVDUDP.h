#pragma once
// ============================================================================
//  EVDUDP.h - the wire: audio fan-out, control push, clock sync
// ============================================================================
//
//  This is the file the players see. It is the same protocol as the unsplit
//  server, byte for byte:
//
//    audio      : unicast to each subscriber on :4202
//                 [2B LE magic 0xE1D0][1B ver=2][1B flags]
//                 [4B LE seq][4B LE captured_ms][Opus frame]
//    ctrl       : unicast to each subscriber on :4203, JSON
//    clock sync : request/response on :4204 (node sends sync_req to us, we
//                 reply to its source port)
//    discovery  : broadcast probe on :4201 (EVDWiFi.h)
//
//  udpSendTask is the only sender of audio and runs on CORE 0. It paces on an
//  absolute frame schedule (EVDPacer.h): one frame slot per 20 ms, never
//  re-anchored by ordinary lateness. That is what keeps every node on the
//  same timeline without any of them having to guess.
// ============================================================================
#include "EVDPacer.h"

// Dead-node back-off. A dead IP can block sendto() for an ARP timeout (~2 s),
// which would stall the pacer; two consecutive failures is enough to skip it
// until its next heartbeat.
static constexpr uint32_t SUB_DEAD_LASTSEEN_MS   = 7000UL;
static constexpr uint8_t  SUB_FAILURE_SKIP_COUNT = 2;

static AudioFramePacer gPacer;
static uint32_t        gPacerLastSeq = 0;
static bool            gPacerSeeded  = false;

// ---- sockets ---------------------------------------------------------------
inline void setup_udp_transport() {
  // Audio and ctrl sockets are bound to ephemeral ports: they only ever send.
  // Players reply to the ctrl socket's source port only for nothing; ctrl is
  // one-way, the node's own stats go back over HTTP.
  audioUdp.begin(0);
  ctrlUdp.begin(0);
  if (!ctrlMutex) ctrlMutex = xSemaphoreCreateMutex();

  // Clock sync must be on a well-known port: the node sends its request to
  // CLOCK_SYNC_UDP_PORT and expects the reply at its source port.
  bool csOk = clockSyncUdp.begin(CLOCK_SYNC_UDP_PORT);
  if (!csOk) LOGE("clockSyncUdp.begin(%d) FAILED", CLOCK_SYNC_UDP_PORT);

  LOGI("UDP ready: audio -> unicast:%d, ctrl -> unicast:%d, clock-sync :%d",
       AUDIO_UDP_PORT, CTRL_UDP_PORT, CLOCK_SYNC_UDP_PORT);
}

// ---- pacer health ----------------------------------------------------------
// The pacer runs on core 0 and its late/lag counters are read by the HTTP task
// and by playerTask (core 1). A volatile uint32_t is atomic on this core, but
// the "last second" value is published in one store so a reader never sees a
// half-updated pair.
inline void publishPacerHealth(uint32_t lateMs) {
  statPacerLateMax = (lateMs > statPacerLateMax) ? lateMs : statPacerLateMax;
  statPacerLateLastSec = lateMs;
}

// ---- audio fan-out (core 0) ------------------------------------------------
inline void udpSendTask(void *param) {
  (void)param;
  LOGI("udpSendTask started on core %d", xPortGetCoreID());
  static OpusPacket pkt;
  static uint8_t hdr[AUDIO_HEADER_BYTES];

  for (;;) {
    if (xQueueReceive(opusQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

    // Seed or re-anchor the schedule only on a real gap, never on lateness.
    bool gap = gPacerSeeded && (uint32_t)(pkt.seq - gPacerLastSeq) > 1;
    uint32_t now = millis();
    if (!gPacerSeeded) {
      gPacer.start(now);
      gPacerSeeded = true;
      gap = false;                 // first frame after boot is not a gap
    }
    gPacerLastSeq = pkt.seq;

    uint32_t queued = uxQueueMessagesWaiting(opusQueue);
    portENTER_CRITICAL(&gStatsMux);
      statQueueDepth = queued;
    portEXIT_CRITICAL(&gStatsMux);

    bool sendThis = gPacer.prepareFrame(now, OPUS_FRAME_MS, queued, gap);
    if (!sendThis) {
      statPacerStaleSkips++;
      statFramesDroppedTotal++;
      continue;
    }

    int32_t late = gPacer.latenessMs(now);
    if (late > 0) publishPacerHealth((uint32_t)late);
    else publishPacerHealth(0);

    // Header, little-endian, exactly as the players parse it.
    hdr[0] = (uint8_t)(AUDIO_PROTO_MAGIC & 0xFF);
    hdr[1] = (uint8_t)((AUDIO_PROTO_MAGIC >> 8) & 0xFF);
    hdr[2] = (uint8_t)AUDIO_PROTO_VERSION;
    hdr[3] = 0;                                  // flags: reserved
    hdr[4] = (uint8_t)(pkt.seq & 0xFF);
    hdr[5] = (uint8_t)((pkt.seq >> 8) & 0xFF);
    hdr[6] = (uint8_t)((pkt.seq >> 16) & 0xFF);
    hdr[7] = (uint8_t)((pkt.seq >> 24) & 0xFF);
    hdr[8]  = (uint8_t)(pkt.captured_ms & 0xFF);
    hdr[9]  = (uint8_t)((pkt.captured_ms >> 8) & 0xFF);
    hdr[10] = (uint8_t)((pkt.captured_ms >> 16) & 0xFF);
    hdr[11] = (uint8_t)((pkt.captured_ms >> 24) & 0xFF);

    std::vector<SubscriberSnap> subs = snapshotSubscribers(now);
    for (auto &s : subs) {
      IPAddress dst;
      if (!dst.fromString(s.ip)) continue;
      if (!audioUdp.beginPacket(dst, AUDIO_UDP_PORT)) {
        statUdpErrors++;
        continue;
      }
      audioUdp.write(hdr, AUDIO_HEADER_BYTES);
      audioUdp.write(pkt.data, (size_t)pkt.len);
      if (audioUdp.endPacket() != 1) {
        statUdpErrors++;
        if (lockFor(subsMutex, 5)) {
          int slot = findSubscriberLocked(s.ip);
          if (slot >= 0 && subscribers[slot].consecutiveSendFailures < 255)
            subscribers[slot].consecutiveSendFailures++;
          unlockFor(subsMutex);
        }
      } else {
        statUdpSent++;
        if (lockFor(subsMutex, 5)) {
          int slot = findSubscriberLocked(s.ip);
          if (slot >= 0) subscribers[slot].consecutiveSendFailures = 0;
          unlockFor(subsMutex);
        }
      }
    }
    statFramesSentTotal++;
  }
}

// ---- control push ----------------------------------------------------------
// WiFiUDP's packet-building state is not re-entrant and this is called from
// core 0 (registration, zone handlers) and core 1 (the 30 s time broadcast),
// so every write is serialised.
inline void sendCtrlToSubscriber(const String &ip, const String &json) {
  if (WiFi.status() != WL_CONNECTED && !accessPointActive) return;

  if (lockFor(subsMutex, 10)) {
    int slot = findSubscriberLocked(ip);
    if (slot >= 0 &&
        subscribers[slot].consecutiveSendFailures >= SUB_FAILURE_SKIP_COUNT) {
      unlockFor(subsMutex);
      return;
    }
    unlockFor(subsMutex);
  }

  IPAddress dst;
  if (!dst.fromString(ip)) { statUdpErrors++; return; }
  if (!ctrlMutex) { statUdpErrors++; return; }
  if (xSemaphoreTake(ctrlMutex, pdMS_TO_TICKS(50)) != pdTRUE) { statUdpErrors++; return; }

  if (!ctrlUdp.beginPacket(dst, CTRL_UDP_PORT)) {
    statUdpErrors++;
    xSemaphoreGive(ctrlMutex);
    return;
  }
  ctrlUdp.write((const uint8_t *)json.c_str(), json.length());
  if (ctrlUdp.endPacket() != 1) statUdpErrors++;
  xSemaphoreGive(ctrlMutex);
}

// ---- clock sync ------------------------------------------------------------
// RTT exchange. The node timestamps its send, we stamp receipt before parsing
// (parsing time would bias the offset asymmetrically), and reply with both.
// Returns true when a datagram was consumed, so the caller can drain the
// socket in a bounded loop instead of handling one packet per loop() pass.
inline bool handleClockSyncRequests() {
  int sz = clockSyncUdp.parsePacket();
  if (sz <= 0) return false;
  const uint32_t serverRxMs = (uint32_t)(esp_timer_get_time() / 1000ULL);

  uint8_t buf[192];
  int n = clockSyncUdp.read(buf, sizeof(buf) - 1);
  if (n <= 0) { statSyncRequestErrors++; return true; }
  buf[n] = '\0';

  IPAddress remote = clockSyncUdp.remoteIP();
  uint16_t  remotePort = clockSyncUdp.remotePort();

  DynamicJsonDocument in(256);
  if (deserializeJson(in, (const char *)buf, (size_t)n)) {
    statSyncRequestErrors++;
    return true;
  }
  // is<uint32_t>() is FALSE for 0: ArduinoJson 6 refuses an integer that could
  // be negative, so a node whose clock had just started (clientMs == 0) was
  // counted as a bad request and got no reply. Accept any unsigned fit.
  if (!(in["clientMs"].is<unsigned long>() || in["clientMs"].is<int>())) {
    statSyncRequestErrors++;
    return true;
  }
  uint32_t clientMs = in["clientMs"].as<uint32_t>();
  uint32_t serverTxMs = (uint32_t)(esp_timer_get_time() / 1000ULL);

  String ip = remote.toString();
  // A node that syncs before it registers still gets a reply; it is created
  // provisionally so it receives ctrl but no audio until it declares compat.
  if (lockFor(subsMutex, 20)) {
    int slot = findSubscriberLocked(ip);
    if (slot < 0) {
      slot = findOrCreateSubscriberLocked(ip, "Player Node", -1, 0, false);
      if (slot >= 0) subscribers[slot].provisional = true;
    }
    if (slot >= 0) subscribers[slot].hasCompletedClockSync = true;
    unlockFor(subsMutex);
  }

  char reply[192];
  int rn = snprintf(reply, sizeof(reply),
      "{\"type\":\"sync_resp\",\"clientMs\":%lu,\"serverRxMs\":%lu,"
      "\"serverTxMs\":%lu,\"epoch\":%lu,\"protoVersion\":%d}",
      (unsigned long)clientMs, (unsigned long)serverRxMs,
      (unsigned long)serverTxMs, (unsigned long)serverEpoch, AUDIO_PROTO_VERSION);
  if (rn > 0 && rn < (int)sizeof(reply)) {
    clockSyncUdp.beginPacket(remote, remotePort);
    clockSyncUdp.write((const uint8_t *)reply, (size_t)rn);
    clockSyncUdp.endPacket();
    statSyncRequests++;
  } else {
    statSyncRequestErrors++;
  }
  return true;
}

// ---- authoritative time broadcast (core 1) --------------------------------
inline void sendAuthoritativeTimeSync() {
  std::vector<SubscriberSnap> subs = snapshotSubscribers(millis());
  char body[128];
  int n = snprintf(body, sizeof(body),
      "{\"type\":\"time_sync\",\"serverMs\":%lu,\"epoch\":%lu,\"protoVersion\":%d}",
      (unsigned long)(esp_timer_get_time() / 1000ULL),
      (unsigned long)serverEpoch, AUDIO_PROTO_VERSION);
  if (n <= 0 || n >= (int)sizeof(body)) return;
  String payload(body, (size_t)n);
  for (auto &s : subs) sendCtrlToSubscriber(s.ip, payload);
}
