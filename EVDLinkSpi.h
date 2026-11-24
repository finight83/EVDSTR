#pragma once
// ============================================================================
//  EVDLinkSpi.h - ESP32-S3 SPI2/HSPI slave half of the EVDCTR -> EVDSTR link
// ============================================================================
//
//  Consumer side. Called from ONE task (audioLink, core 0). The task owns the
//  bus; there is no mutex and no ISR-driven double buffer, because the frame
//  period is 20 ms and the transfer is ~1 ms — a task-level handshake is both
//  simpler and more predictable than an ISR that has to hand a buffer across.
//
//  Handshake, from this side:
//
//    wait IRQ high            (CTR has a frame staged and is waiting for us)
//    CTR asserts CS low, clocks LINK_WIRE_BYTES (8 B header + 1024 B)
//    CTR raises CS            (and then releases IRQ)
//    collect the result, validate the header, take linkPayloadLenFromHeader()
//
//  Neither CS nor the clock count is ours to choose: the master drives CS, and
//  the slave transaction is armed for the fixed maximum because the real
//  payload length is only known after the header has arrived. That is why the
//  frame is a fixed size on the wire (LINK_WIRE_BYTES, in EVDLink.h) and why
//  this file never inspects CS itself.
//
//  A malformed frame costs one frame: it is counted in gLinkBadHeaders and the
//  transaction is re-armed on the next call. Nothing here can stall CTR,
//  because CTR's own send is bounded and never retries.
//
//  Wiring: see the master half (EVDCTR/EVDLinkSpi.h). The two pin maps must
//  be identical; LINK_IRQ and LINK_CS swap direction, nothing else changes.
// ============================================================================
#include <Arduino.h>
#include <driver/spi_slave.h>
#include <driver/gpio.h>
#include "EVDLink.h"

#ifndef PIN_LINK_SCLK
#define PIN_LINK_SCLK 11
#endif
#ifndef PIN_LINK_MOSI
#define PIN_LINK_MOSI 12
#endif
#ifndef PIN_LINK_IRQ
#define PIN_LINK_IRQ  13
#endif
#ifndef PIN_LINK_CS
#define PIN_LINK_CS   14
#endif

#define LINK_SPI_HOST SPI2_HOST

// The SPI slave driver's DMA path needs the buffers to be DMA-capable and
// word-aligned. One static pair, sized for the largest frame, so a receive
// never allocates.
static uint8_t DMA_ATTR gLinkRxBuf[LINK_HDR_BYTES + LINK_MAX_PAYLOAD]
    __attribute__((aligned(4)));

// [link-irq-semaphore] CTR holds IRQ high for the whole frame. Polling it in
// a `while` loop at priority 6 starved the Idle task on core 0 - the task
// watchdog then aborted the board after 5 s with no EVDCTR connected. The ISR
// now gives this semaphore and the task BLOCKS on it instead of spinning.
static SemaphoreHandle_t gLinkIrqSem = nullptr;
static void IRAM_ATTR linkIrqIsr(void *) {
  BaseType_t woken = pdFALSE;
  if (gLinkIrqSem) xSemaphoreGiveFromISR(gLinkIrqSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static bool gLinkUp = false;
static spi_slave_transaction_t gLinkTrans;
static bool          gLinkArmed         = false;
static uint32_t      gLinkFramesRx      = 0;
static uint32_t      gLinkBadHeaders    = 0;
static uint32_t      gLinkRxTimeouts    = 0;

// ---- Init -------------------------------------------------------------------
// Called once from setup() on core 0 before the audioLink task exists.
inline bool setupLinkSpi() {
  // CS is driven by us as a plain GPIO: the slave driver's own CS handling
  // expects the *master* to control it, which is the reverse of this link.
  gpio_config_t csCfg = {};
  csCfg.pin_bit_mask = (1ULL << PIN_LINK_CS);
  csCfg.mode         = GPIO_MODE_OUTPUT;
  csCfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  csCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  csCfg.intr_type    = GPIO_INTR_DISABLE;
  if (gpio_config(&csCfg) != ESP_OK) return false;
  gpio_set_level((gpio_num_t)PIN_LINK_CS, 1);   // idle high, active low

  gpio_config_t irqCfg = {};
  irqCfg.pin_bit_mask = (1ULL << PIN_LINK_IRQ);
  irqCfg.mode         = GPIO_MODE_INPUT;
  irqCfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  irqCfg.pull_down_en = GPIO_PULLDOWN_ENABLE;   // no CTR = permanently low
  irqCfg.intr_type    = GPIO_INTR_DISABLE;
  if (gpio_config(&irqCfg) != ESP_OK) return false;

  spi_bus_config_t bus = {};
  bus.mosi_io_num     = PIN_LINK_MOSI;
  bus.miso_io_num     = -1;
  bus.sclk_io_num     = PIN_LINK_SCLK;
  bus.quadwp_io_num   = -1;
  bus.quadhd_io_num   = -1;
  bus.max_transfer_sz = LINK_HDR_BYTES + LINK_MAX_PAYLOAD;

  spi_slave_interface_config_t slv = {};
  slv.mode           = 0;
  slv.spics_io_num   = -1;          // we drive CS ourselves
  slv.queue_size     = 1;
  slv.flags          = 0;

  if (spi_slave_initialize(LINK_SPI_HOST, &bus, &slv, SPI_DMA_CH_AUTO) != ESP_OK) {
    return false;
  }

  // [link-irq-semaphore] Armed before the interrupt, and before any task can
  // call linkReceiveFrame(), so the first frame cannot be missed.
  if (!gLinkIrqSem) gLinkIrqSem = xSemaphoreCreateBinary();
  if (!gLinkIrqSem) return false;
  gpio_set_intr_type((gpio_num_t)PIN_LINK_IRQ, GPIO_INTR_POSEDGE);
  if (gpio_isr_handler_add((gpio_num_t)PIN_LINK_IRQ, linkIrqIsr, nullptr) != ESP_OK) {
    return false;
  }
  gpio_intr_enable((gpio_num_t)PIN_LINK_IRQ);

  gLinkUp = true;
  return true;
}

// One line that answers "is EVDCTR there?", at boot and on demand. EVDCTR
// drives IRQ high for the duration of a frame, so a level sampled here says
// whether the partner is alive and wired - and the pull-down means a missing
// or unpowered partner reads LOW rather than floating.
inline void logLinkPeerState(const char *when) {
  int irq = gpio_get_level((gpio_num_t)PIN_LINK_IRQ);
  Serial.printf("[LINK] %s: IRQ(GPIO%d) %s - %s\n", when, PIN_LINK_IRQ,
                irq ? "HIGH" : "LOW",
                irq ? "EVDCTR is driving it (partner present)"
                    : "no partner signal yet (EVDCTR off, unwired, or silent)");
}

// Queue the single transaction the slave driver accepts. Kept separate so
// setup can arm it once and report whether the peripheral accepted it.
//
// [link-arm-timeout] spi_slave_queue_trans() with a ZERO timeout fails when
// the driver's one queue slot is still busy. The old receive path used 0 and
// never set gLinkArmed on failure, so one busy moment disabled the link for
// good - silently. 20 ms is longer than a frame period.
inline bool linkArm() {
  if (gLinkArmed) return true;
  gLinkTrans.length    = LINK_WIRE_BYTES * 8;
  gLinkTrans.tx_buffer = nullptr;
  gLinkTrans.rx_buffer = gLinkRxBuf;
  if (spi_slave_queue_trans(LINK_SPI_HOST, &gLinkTrans,
                            pdMS_TO_TICKS(20)) != ESP_OK) {
    return false;
  }
  gLinkArmed = true;
  return true;
}

// ---- Frame receive ----------------------------------------------------------
// The ESP-IDF SPI slave driver only accepts bytes while a transaction is
// queued. spi_slave_transmit() queues one, blocks for it, and unqueues it - so
// between calls there is nothing staged, and any CS edge CTR raises in that
// window is clocked into nowhere. At 50 frames/s the producer is ready every
// 20 ms while this task is between polls, so the old code lost a large
// fraction of frames and the link looked "up" while audio stuttered.
//
// The fix is to keep a transaction queued at ALL times: queue it with
// spi_slave_queue_trans(), then wait on spi_slave_get_trans_result(). The
// buffer is never unqueued, so a frame that lands while the task is busy is
// still received. `waitUs` now bounds the RESULT wait, not the queueing.
//
// `samplesOut`, `channelsOut`, `rateHzOut` and `silenceOut` describe the frame
// so the caller can drive the pacer and the wire header without re-parsing.
inline bool linkReceiveFrame(uint8_t *payloadOut, uint32_t payloadCap,
                             uint32_t &lenOut, uint8_t &samplesOut,
                             uint8_t &channelsOut, uint32_t &rateHzOut,
                             bool &silenceOut, uint32_t waitUs) {
  lenOut = 0; samplesOut = 0; channelsOut = 0; rateHzOut = 0; silenceOut = false;
  if (!gLinkUp) return false;

  // Re-arm the permanently-queued transaction. CTR clocks the whole frame -
  // header, payload and zero padding - in one CS assertion, so the armed
  // length is always the maximum and the header tells us how much is real.
  if (!gLinkArmed && !linkArm()) {
    gLinkRxTimeouts++;
    return false;
  }

  // [link-irq-semaphore] Block until CTR raises IRQ, instead of spinning on
  // the pin. A semaphore wait yields the core, so the Idle task runs and the
  // task watchdog stays fed even with no EVDCTR attached. `waitUs` is the
  // bound; it is a real wait now, not a busy loop.
  if (gLinkIrqSem) {
    if (xSemaphoreTake(gLinkIrqSem, pdMS_TO_TICKS(waitUs / 1000 + 1)) != pdTRUE)
      return false;
  } else if (gpio_get_level((gpio_num_t)PIN_LINK_IRQ) == 0) {
    // No semaphore (setup has not run yet): fall back to the pin, but never
    // spin for more than one tick.
    vTaskDelay(1);
    if (gpio_get_level((gpio_num_t)PIN_LINK_IRQ) == 0) return false;
  }

  // The producer is driving IRQ, so the transaction should complete shortly.
  // portMAX_DELAY would be a lie here - a producer that died mid-frame would
  // block this task forever - so keep the tick bound.
  spi_slave_transaction_t *done = nullptr;
  esp_err_t rc = spi_slave_get_trans_result(LINK_SPI_HOST, &done,
                                            pdMS_TO_TICKS(10));
  if (rc != ESP_OK || done == nullptr) {
    // The transaction was not consumed. It is still queued, so leave it armed;
    // a timeout here means CTR went quiet mid-frame.
    gLinkRxTimeouts++;
    return false;
  }
  gLinkArmed = false;   // re-armed on the next call

  if (done->trans_len < LINK_HDR_BYTES * 8) {
    gLinkBadHeaders++;
    return false;
  }

  LinkFrameHeader h;
  memcpy(&h, gLinkRxBuf, LINK_HDR_BYTES);
  uint32_t declaredLen = 0;
  if (linkCheckHeader(h, declaredLen) != LINK_HDR_OK) {
    gLinkBadHeaders++;
    return false;
  }
  // [link-fixed-frame] The frame is always LINK_WIRE_BYTES on the wire; the
  // real payload length comes from the header, not from trans_len.
  const uint32_t payloadLen = linkPayloadLenFromHeader(h);

  // trans_len is the number of BITS actually clocked. A frame the producer
  // aborted mid-way arrives short and must not be handed on as audio - but the
  // test is "enough for the header plus the payload the header declares", not
  // "all LINK_WIRE_BYTES": a producer that clocks only what it has is legal,
  // and demanding the padding would reject a perfectly good frame.
  const uint32_t clockedBytes = (uint32_t)(done->trans_len / 8);
  if (clockedBytes < LINK_HDR_BYTES + payloadLen) {
    gLinkBadHeaders++;
    return false;
  }
  if (payloadLen > payloadCap || payloadLen > LINK_MAX_PAYLOAD) {
    gLinkBadHeaders++;
    return false;
  }

  if (payloadLen > 0) {
    memcpy(payloadOut, gLinkRxBuf + LINK_HDR_BYTES, payloadLen);
  }

  lenOut      = payloadLen;
  samplesOut  = h.samples;
  channelsOut = h.channels;
  rateHzOut   = (uint32_t)h.rateK * 1000u;
  silenceOut  = (h.flags & LINK_FLAG_SILENCE) != 0;
  gLinkFramesRx++;
  return true;
}

// Counters for /api/stats. They live here because this is the only file that
// knows what went wrong on the wire, and the dashboard's "is the link healthy"
// answer depends on them: a link that drops frames looks identical to a
// healthy one without these.
inline uint32_t linkFramesRx()     { return gLinkFramesRx; }
inline uint32_t linkBadHeaders()   { return gLinkBadHeaders; }
inline uint32_t linkRxTimeouts()   { return gLinkRxTimeouts; }
