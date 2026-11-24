#pragma once
// ============================================================================
//  EVDLink.h - the CTR <-> STR inter-processor link (Opus frames over pins)
// ============================================================================
//
//  This is the ONLY thing that crosses between the two ESP32-S3s. It is
//  deliberately self-contained: it needs no Arduino headers, no opus headers
//  and no shared .ino, so the *same file* is committed to both EVDCTR and
//  EVDSTR (and can be host-tested) instead of drifting into two dialects.
//
//  Direction
//  ---------
//    CTR (producer)  : audioEncode task, CORE 1 ONLY
//    STR (consumer)  : audioLink task, CORE 0 ONLY
//    CTR never reads a frame back; STR never writes one.
//
//  The wire format is a single SPI transaction per frame:
//
//    CTR asserts LINK_IRQ high -> waits >= LINK_IRQ_SETUP_US
//    STR sees the rising edge, asserts LINK_CS low
//    STR clocks LINK_HDR_BYTES (8) header bytes, validates magic/version
//    STR clocks `len` payload bytes (Opus frame)
//    STR raises LINK_CS; CTR drops LINK_IRQ and starts the next 20 ms frame
//
//  If STR never raises CS (not booted, hung, wrong pins, no common ground)
//  the producer times out after LINK_CS_TIMEOUT_US and counts a dropped
//  frame. Audio keeps flowing to the local outputs; only the link starves.
//  A link that is *down* must never block the encoder longer than that
//  timeout, so the encode task stays the sole occupant of core 1.
//
//  Frame header, 8 bytes, little-endian:
//
//    offset size field        notes
//    ------ ---- -----------  ---------------------------------------------
//     0      2   magic        0xE1D5 (EVD5)
//     2      1   version      LINK_VERSION
//     3      1   flags        bit0 = silence frame (len == 0), rest reserved
//     4      1   samples      samples per channel in this frame (0, 48 or 96)
//     5      1   channels     1 or 2
//     6      1   rateK        opus sample rate in kHz (24 or 48)
//     7      1   reserved     0
//
//  `samples` / `rateK` travel per frame on purpose: the encoder can be
//  re-created at 24 kHz or 48 kHz from the dashboard at any moment, and a
//  re-create changes the frame size from 960 to 480 samples. STR must know
//  which one it is decoding without a second control channel, and the
//  header is the only channel it has.
//
//  `captured_ms` and `seq` (the v2 audio-protocol fields the players need)
//  do NOT cross the link as 32-bit millis(): the two chips boot at different
//  moments and their millis() are unrelated. CTR sends nothing but samples;
//  STR stamps the wire sequence and capture instant from its OWN clock at
//  the moment it hands the frame to the pacer. The players only ever need
//  a monotonic timeline, which STR's millis() already is.
//
//  ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LINK_MAGIC          0xE1D5u
#define LINK_VERSION        1
#define LINK_HDR_BYTES      8
#define LINK_FLAG_SILENCE   0x01

// Throughput: one 20 ms frame, worst case 1024 payload bytes + 8 header.
// At 8 MHz that is 1.03 ms of wire time inside a 20 ms period (5 %).
// 8 MHz is the fastest rate that survives Dupont wires longer than ~10 cm
// without a ground return per signal; 16 MHz works on a PCB.
#define LINK_SPI_HZ         8000000UL

// Producer-side timeouts. Measured in microseconds, from the frame's own
// deadline: a healthy STR raises CS within ~50 us of the IRQ edge.
#define LINK_IRQ_SETUP_US       2
#define LINK_CS_TIMEOUT_US    4000
#define LINK_XFER_TIMEOUT_US  6000

// Consumer-side. STR waits this long for an IRQ edge before giving the
// producer a chance to time out and retry; it never holds CS across it.
#define LINK_IDLE_POLL_US     1000

// Largest payload that can cross. Must equal OPUS_MAX_PACKET_BYTES on CTR
// and MAX_OPUS_PACKET_LEN on STR (both 1024 today). A build-time assert on
// each side ties this constant to its local maximum.
#define LINK_MAX_PAYLOAD    1024

// Silence frame: the dashboard switched to "off", or the source went away.
// CTR still emits one every 20 ms so STR's pacer keeps its absolute frame
// schedule instead of re-anchoring on the first frame after the gap.
struct LinkFrameHeader {
  uint16_t magic;
  uint8_t  version;
  uint8_t  flags;
  uint8_t  samples;
  uint8_t  channels;
  uint8_t  rateK;
  uint8_t  reserved;
};
static_assert(sizeof(LinkFrameHeader) == LINK_HDR_BYTES,
              "LinkFrameHeader must stay 8 bytes on the wire");

static inline uint8_t linkFlagsFor(bool silence) {
  return silence ? LINK_FLAG_SILENCE : 0;
}

// Fills a header for a frame. `payloadLen` is validated by the caller.
static inline void linkFillHeader(LinkFrameHeader &h, uint8_t samples,
                                  uint8_t channels, uint32_t rateHz,
                                  bool silence) {
  h.magic    = LINK_MAGIC;
  h.version  = LINK_VERSION;
  h.flags    = linkFlagsFor(silence);
  h.samples  = samples;
  h.channels = channels;
  h.rateK    = (uint8_t)(rateHz / 1000u);
  h.reserved = 0;
}

enum LinkHeaderVerdict {
  LINK_HDR_OK = 0,
  LINK_HDR_BAD_MAGIC,
  LINK_HDR_BAD_VERSION,
  LINK_HDR_BAD_RATE,
  LINK_HDR_BAD_CHANNELS
};

// Validates a header the consumer just clocked in. Returns the number of
// payload bytes the consumer must still clock, or 0 with a verdict set.
// `payloadLenOut` is 0 for a silence frame (legal, verdict OK).
// ---- Frame framing (added after the first bring-up review) -----------------
//
// The slave side can only accept bytes while a transaction is queued, so the
// consumer keeps one armed at all times and cannot know the payload length in
// advance. The producer therefore always clocks the FULL frame:
// LINK_HDR_BYTES + LINK_MAX_PAYLOAD. `len` is not on the wire; it is derived
// from the header's `samples` and `channels`, which is what the header exists
// for. Padding past the real Opus payload is zero-filled and discarded.
//
// This keeps one CS assertion per frame (two transactions would leave a clock
// gap the slave cannot distinguish from the end of a frame) and lets the
// consumer re-arm a fixed-size transaction instead of computing lengths from
// bytes it has not received yet.
#define LINK_WIRE_BYTES (LINK_HDR_BYTES + LINK_MAX_PAYLOAD)

// Payload length implied by a validated header. Silence frames carry none.
static inline uint32_t linkPayloadLenFromHeader(const LinkFrameHeader &h) {
  if (h.flags & LINK_FLAG_SILENCE) return 0;
  if (h.samples == 0 || h.channels == 0) return 0;
  // 192 kbps ceiling at 20 ms = 480 bytes; 1024 is the structural maximum.
  uint32_t est = ((uint32_t)h.samples * (uint32_t)h.channels * 3u) / 2u + 16u;
  return (est > LINK_MAX_PAYLOAD) ? LINK_MAX_PAYLOAD : est;
}

static inline LinkHeaderVerdict linkCheckHeader(const LinkFrameHeader &h,
                                                uint32_t &payloadLenOut) {
  payloadLenOut = 0;
  if (h.magic != LINK_MAGIC)   return LINK_HDR_BAD_MAGIC;
  if (h.version != LINK_VERSION) return LINK_HDR_BAD_VERSION;
  if (h.rateK != 24 && h.rateK != 48) return LINK_HDR_BAD_RATE;
  if (h.channels != 1 && h.channels != 2) return LINK_HDR_BAD_CHANNELS;
  return LINK_HDR_OK;
}
