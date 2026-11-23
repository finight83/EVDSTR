#pragma once
// ============================================================================
//  EVDPacer.h - absolute frame schedule
// ============================================================================
//  Identical to the unsplit server's pacer, because it solved the same
//  problem there: send one frame per 20 ms slot, never drift, and recover
//  from a backlog by discarding whole slots rather than by hurrying.
//
//  The caller advances one slot per sent OR explicitly discarded packet;
//  ordinary lateness never moves the origin. Only a genuine gap with an empty
//  queue re-anchors.
// ============================================================================
#include <stdint.h>

static const uint32_t AUDIO_PACER_MAX_LAG_MS = 100;

struct AudioFramePacer {
  bool     initialized = false;
  uint32_t deadlineMs = 0;
  bool     drainingStaleBacklog = false;

  void start(uint32_t nowMs) {
    deadlineMs = nowMs;
    initialized = true;
    drainingStaleBacklog = false;
  }

  int32_t untilMs(uint32_t nowMs) const { return (int32_t)(deadlineMs - nowMs); }
  int32_t latenessMs(uint32_t nowMs) const { return (int32_t)(nowMs - deadlineMs); }

  bool shouldReanchorAfterIdle(bool packetGap, uint32_t queuedFrames) const {
    return packetGap && queuedFrames == 0;
  }

  bool prepareFrame(uint32_t nowMs, uint32_t frameMs,
                    uint32_t queuedFrames, bool packetGap) {
    if (shouldReanchorAfterIdle(packetGap, queuedFrames)) start(nowMs);

    int32_t late = latenessMs(nowMs);
    if (late > (int32_t)AUDIO_PACER_MAX_LAG_MS) drainingStaleBacklog = true;

    if (drainingStaleBacklog && late > 0) {
      // Once the queue is empty, advancing one slot per arriving frame can
      // never catch up with a producer running at the same rate.
      if (queuedFrames == 0) {
        start(nowMs);
        advance(frameMs);
        return true;
      }
      advance(frameMs);
      return false;
    }

    drainingStaleBacklog = false;
    advance(frameMs);
    return true;
  }

  void advance(uint32_t frameMs) { deadlineMs += frameMs; }
};
