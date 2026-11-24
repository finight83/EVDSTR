#pragma once
// ============================================================================
//  EVDAudio.h - EVDSTR audio path: link in -> Opus queue -> UDP pacer out
// ============================================================================
//
//  Two tasks, one per core, with one thing on each:
//
//    CORE 0  audioLinkTask   pin link -> opusQueue
//            (the pacer runs here too, see EVDUDP.h's udpSendTask)
//    CORE 1  playerTask      registration, heartbeats, node stats, zone/config
//                            pushes, clock-sync bookkeeping
//
//  There is no encoder here, and no resampler: frames arrive already encoded.
//  What this board adds is the only thing the players can see — a monotonic
//  wire sequence, a capture timestamp on its own clock, and an absolute send
//  schedule.
// ============================================================================

// ---- Silence and re-anchor policy -----------------------------------------
// A silence frame (len 0) still occupies a pacer slot, so the schedule does not
// re-anchor across a mute. It is never sent to players: it marks "the encoder
// had nothing", which is not the same as "the encoder stopped", and players
// already conceal a gap correctly.
static const uint32_t LINK_STALE_DROP_MS = 400;

uint32_t statLinkIn          = 0;
// [link-in-bad-unused] There used to be a statLinkInBad here, mirrored from
// EVDLinkSpi.h into /api/stats. Nothing ever wrote it, so it read 0 even while
// frames were being rejected. The real counters live in EVDLinkSpi.h
// (linkBadHeaders / linkRxTimeouts) and are the ones the routes read.
uint32_t statLinkSilence     = 0;
// Frames that arrived AFTER a gap longer than LINK_STALE_DROP_MS. Since the
// silence frames were given their own counter this is a genuine fault counter:
// it counts recoveries from a producer that went away and came back.
uint32_t statLinkStale       = 0;
uint32_t statLinkLastOkMs    = 0;
uint32_t statLinkGapMs       = 0;
uint32_t gWireSeq            = 1;
uint32_t gLastLinkedRate     = OPUS_SAMPLE_RATE_DEFAULT;

// The one receive buffer. File scope, not function-local: see
// [link-rx-no-big-frame] in audioLinkTask.
OpusPacket gLinkRxPkt;

// ---- audioLinkTask (core 0) ------------------------------------------------
// Polls the link, validates, stamps and queues. The wait is short (2 ms) so a
// frame is never more than one poll interval late; the task blocks on the GPIO
// read in between, so the CPU cost is nil.
inline void audioLinkTask(void *param) {
  (void)param;
  LOGI("audioLinkTask started on core %d", xPortGetCoreID());
  Serial.printf("[LINK] audioLink running on core %d, %u B rx buffer\n",
                (int)xPortGetCoreID(), (unsigned)sizeof(OpusPacket));
  // [link-arm-at-start] Arm the first transaction here, before the loop, and
  // say whether it worked. The receive path arms lazily; if that first arm
  // fails there is nothing to receive into and the task would spin silently.
  Serial.printf("[LINK] arming first SPI transaction...\n");
  if (!linkArm()) {
    Serial.printf("[LINK] ARM FAILED - the SPI slave driver rejected the "
                  "transaction; no audio can arrive\n");
  } else {
    Serial.printf("[LINK] armed, waiting for EVDCTR on IRQ(GPIO%d)\n",
                  PIN_LINK_IRQ);
  }
  // [link-rx-no-big-frame] This used to be a function-local `static
  // OpusPacket pkt`, i.e. a 1040-byte object. Static storage does not consume
  // task stack, but the compiler still needs the destination address live and
  // the receive path is the hottest code on a 4096-byte stack - and the first
  // crash after boot was inside the heap allocator while this task was the
  // running one. The buffer now lives at file scope, allocated once, so no
  // amount of inlining can put it on the stack.
  static uint32_t lastRxMs = 0;
  static uint32_t loops = 0;
  static uint32_t lastBeat = 0;

  for (;;) {
    uint32_t len = 0;
    uint8_t  samples = 0, channels = 0;
    uint32_t rate = 0;
    bool silence = false;

    // [link-irq-wait-100ms] This used to ask for a 2000 us wait per call. With
    // EVDCTR absent the call returned in 2 ms and the loop re-armed, so the
    // task never slept - the pin poll was a busy-wait and it starved the Idle
    // task on core 0 until the watchdog aborted the board. The wait is now a
    // real block on the IRQ semaphore, so it can be long: 100 ms per attempt
    // means a dead producer costs one tick per call instead of a spin.
    bool got = linkReceiveFrame(gLinkRxPkt.data, MAX_OPUS_PACKET_LEN, len,
                                samples, channels, rate, silence, 100000);
    uint32_t now = millis();
    loops++;

    // [link-alive-beat] If this line never appears the task is not looping at
    // all (blocked in the SPI driver or killed); if it appears with linkIn=0
    // the loop is fine and EVDCTR is simply not clocking frames. Those are two
    // completely different faults and the old log could not tell them apart.
    if (now - lastBeat > 5000) {
      lastBeat = now;
      // Stack high-water: the smallest free stack seen since the task
      // started, in bytes. If this is near zero the task is about to
      // overflow, which corrupts the heap next to it rather than crashing
      // here - the first crash after boot looked exactly like that.
      Serial.printf("[LINK] loops %lu | frames in %lu | bad %lu | timeouts %lu "
                    "| silence %lu | gap %lu ms | irq %d | stack free %u B\n",
                    (unsigned long)loops, (unsigned long)statLinkIn,
                    (unsigned long)linkBadHeaders(),
                    (unsigned long)linkRxTimeouts(),
                    (unsigned long)statLinkSilence,
                    (unsigned long)statLinkGapMs,
                    gpio_get_level((gpio_num_t)PIN_LINK_IRQ),
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }

    if (!got) {
      // Distinguish "no frame this poll" from "the producer has stopped".
      if (lastRxMs && (now - lastRxMs) > LINK_STALE_DROP_MS) {
        statLinkGapMs = now - lastRxMs;
        if (statLinkGapMs > 1000) {
          static uint32_t lastWarn = 0;
          if (now - lastWarn > 5000) {
            lastWarn = now;
            LOGW("link: no frames for %u ms (EVDCTR down or wiring)",
                 (unsigned)statLinkGapMs);
          }
        }
      }
      continue;
    }

    // [link-stale-on-recovery] statLinkStale is exported as linkStale, so it
    // has to mean something: count the frame that ends a gap longer than
    // LINK_STALE_DROP_MS. Before this it was only ever read, never written,
    // and the dashboard showed a permanent 0 next to real gap numbers.
    if (lastRxMs && (now - lastRxMs) > LINK_STALE_DROP_MS) statLinkStale++;

    lastRxMs = now;
    statLinkLastOkMs = now;
    statLinkIn++;
    statLinkGapMs = 0;

    if (rate != gLastLinkedRate) {
      LOGI("link: encoder rate %u -> %u Hz", (unsigned)gLastLinkedRate,
           (unsigned)rate);
      gLastLinkedRate = rate;
    }

    // A silence frame keeps the encoder's 20 ms cadence but carries no audio.
    // It is counted and never queued: players already conceal a gap, and an
    // empty UDP datagram would only reset their concealment counters.
    // [link-silence-counter] This used to bump statLinkStale, the counter for
    // "the producer stopped", so a muted source was indistinguishable from a
    // dead EVDCTR on /api/stats - the first thing anyone checks for no audio.
    if (silence || len == 0) {
      statLinkSilence++;
      continue;
    }

    // The frame is self-describing: the header carries the encoder's rate and
    // channel count, so a dashboard re-create on EVDCTR reaches the players'
    // registrations without any control channel. EVDSTR does not encode, but
    // it does tell the players what the encoder is doing.
    if (samples)  g_opusFrameSamples = samples;
    if (channels) g_opusChannels     = channels;
    if (rate && rate != g_opusSampleRate) g_opusSampleRate = rate;

    // Stamp on THIS board's clock. The two chips boot independently, so the
    // producer's millis() means nothing here; the players only need a
    // monotonic capture timeline, which is exactly what they get.
    gLinkRxPkt.len     = (int)len;
    gLinkRxPkt.seq     = gWireSeq++;
    gLinkRxPkt.captured_ms = now;
    gLinkRxPkt.samples = samples;

    if (xQueueSend(opusQueue, &gLinkRxPkt, 0) != pdTRUE) {
      statOpusDropped++;
      continue;
    }
    statOpusEncoded++;
  }
}

// ---- playerTask (core 1) ---------------------------------------------------
// Everything that talks to a player: the heartbeat loop that keeps the
// subscriber table fresh, the node-stats sweep, and the 30 s authoritative
// time broadcast. It runs on core 1 so a slow node never delays the audio
// pacer on core 0.
inline void playerTask(void *param) {
  (void)param;
  LOGI("playerTask started on core %d", xPortGetCoreID());
  uint32_t lastTimeSync = 0;

  for (;;) {
    // 1. Heartbeat bookkeeping: prune nodes that stopped checking in.
    pruneStaleSubscribers(millis());

    // 2. Authoritative time broadcast every 30 s. Nodes that lost their RTT
    //    lock (a WiFi hiccup, a roaming event) get pulled back onto a common
    //    timeline by this; nothing else in the system can do it.
    if (millis() - lastTimeSync > 30000) {
      lastTimeSync = millis();
      sendAuthoritativeTimeSync();
    }

    // 3. Node stats sweep: ask every subscriber for its counters. The reply
    //    is an HTTP POST to /api/node_stats, handled on core 0.
    pollNodeStats();

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ---- setup -----------------------------------------------------------------
inline void setup_queues_and_tasks() {
  LOGI("Creating queues and tasks...");
  opusQueue = xQueueCreate(OPUS_QUEUE_LEN, sizeof(OpusPacket));
  if (!opusQueue) {
    Serial.printf("[TASK] opusQueue creation FAILED (free heap %u B) - "
                  "no audio will flow\n", (unsigned)ESP.getFreeHeap());
    LOGE("opusQueue creation failed");
    return;
  }

  // [task-create-progress] Every creation prints an unconditional line BEFORE
  // it runs. The first bring-up died with TASK_WDT right after "audioLinkTask
  // started on core 0" and no further output, which left no way to tell
  // whether the task creation failed, the task blocked, or the next creation
  // hung. Serial.printf, not LOGI: LOGI can be filtered by gSerialLogLevel.
  BaseType_t rc;
  Serial.printf("[TASK] creating audioLink (4096 B, pri 6, core 0)...\n");
  // [link-task-stack] 4096 was too tight for the SPI slave driver plus the
  // WiFi/IPC paths it touches. 8192 matches udpSend and players.
  rc = xTaskCreatePinnedToCore(audioLinkTask, "audioLink", 8192, nullptr, 6,
                               nullptr, 0);
  if (rc != pdPASS) { LOGE("FAILED to create audioLinkTask (%d)", (int)rc);
                      Serial.printf("[TASK] audioLink FAILED (%d)\n", (int)rc); }

  Serial.printf("[TASK] creating udpSend (8192 B, pri 3, core 0)...\n");
  rc = xTaskCreatePinnedToCore(udpSendTask, "udpSend", 8192, nullptr, 3,
                               nullptr, 0);
  if (rc != pdPASS) { LOGE("FAILED to create udpSendTask (%d)", (int)rc);
                      Serial.printf("[TASK] udpSend  FAILED (%d)\n", (int)rc); }

  Serial.printf("[TASK] creating players (8192 B, pri 3, core 1)...\n");
  rc = xTaskCreatePinnedToCore(playerTask, "players", 8192, nullptr, 3,
                               nullptr, 1);
  if (rc != pdPASS) { LOGE("FAILED to create playerTask (%d)", (int)rc);
                      Serial.printf("[TASK] players  FAILED (%d)\n", (int)rc); }

  // The encoder mirror is an HTTP client, not audio: it lives on core 0 at a
  // low priority so a slow EVDCTR can never delay the link or the pacer.
  Serial.printf("[TASK] creating mirror (4096 B, pri 1, core 0)...\n");
  rc = xTaskCreatePinnedToCore(encoderMirrorTask, "mirror", 4096, nullptr, 1,
                               nullptr, 0);
  if (rc != pdPASS) { LOGE("FAILED to create encoderMirrorTask (%d)", (int)rc);
                      Serial.printf("[TASK] mirror   FAILED (%d)\n", (int)rc); }

  Serial.printf("[TASK] core 0: audioLink(p6) udpSend(p3) mirror(p1) | "
                "core 1: players(p3)\n");
  LOGI("Task layout: core 0 = link in + UDP pacer + mirror, core 1 = players");
}
