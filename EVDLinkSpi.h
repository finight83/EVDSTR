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
//    poll IRQ high            (CTR has a frame staged and is waiting for us)
//    assert CS low
//    clock LINK_HDR_BYTES, validate
//    clock `len` payload bytes into the receive ring
//    raise CS                 (CTR sees this and considers the frame sent)
//
//  We raise CS in every exit path, including a bad header: CTR is blocked on
//  it, and a malformed frame must cost one frame, not a stalled producer.
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

static volatile bool gLinkUp = false;
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

// ---- Frame receive ----------------------------------------------------------
// Waits up to `waitUs` for the producer's IRQ edge. Returns true when a
// complete, valid frame was clocked into `payloadOut`. On false, nothing was
// consumed and the caller should simply loop (the next frame is 20 ms away).
//
// `samplesOut`, `channelsOut`, `rateHzOut` and `silenceOut` describe the frame
// so the caller can drive the pacer and the wire header without re-parsing.
inline bool linkReceiveFrame(uint8_t *payloadOut, uint32_t payloadCap,
                             uint32_t &lenOut, uint8_t &samplesOut,
                             uint8_t &channelsOut, uint32_t &rateHzOut,
                             bool &silenceOut, uint32_t waitUs) {
  lenOut = 0; samplesOut = 0; channelsOut = 0; rateHzOut = 0; silenceOut = false;
  if (!gLinkUp) return false;

  const uint32_t t0 = micros();
  while (gpio_get_level((gpio_num_t)PIN_LINK_IRQ) == 0) {
    if ((uint32_t)(micros() - t0) > waitUs) return false;
  }

  gpio_set_level((gpio_num_t)PIN_LINK_CS, 0);

  // Header first: we must know the payload length before we can clock it,
  // and the master has already staged the whole frame, so it simply waits in
  // its shifter while we validate.
  spi_slave_transaction_t th = {};
  th.length    = LINK_HDR_BYTES * 8;
  th.tx_buffer = nullptr;
  th.rx_buffer = gLinkRxBuf;
  esp_err_t rc = spi_slave_transmit(LINK_SPI_HOST, &th, pdMS_TO_TICKS(10));
  if (rc != ESP_OK) {
    gpio_set_level((gpio_num_t)PIN_LINK_CS, 1);
    gLinkRxTimeouts++;
    return false;
  }

  LinkFrameHeader h;
  memcpy(&h, gLinkRxBuf, LINK_HDR_BYTES);
  uint32_t payloadLen = 0;
  if (linkCheckHeader(h, payloadLen) != LINK_HDR_OK ||
      payloadLen > payloadCap) {
    // Still clock the payload out of the shifter when the length is known,
    // otherwise CTR's next transaction starts mid-frame.
    if (payloadLen > 0 && payloadLen <= LINK_MAX_PAYLOAD) {
      spi_slave_transaction_t ts = {};
      ts.length    = payloadLen * 8;
      ts.tx_buffer = nullptr;
      ts.rx_buffer = gLinkRxBuf;
      spi_slave_transmit(LINK_SPI_HOST, &ts, pdMS_TO_TICKS(10));
    }
    gpio_set_level((gpio_num_t)PIN_LINK_CS, 1);
    gLinkBadHeaders++;
    return false;
  }

  if (payloadLen > 0) {
    spi_slave_transaction_t ts = {};
    ts.length    = payloadLen * 8;
    ts.tx_buffer = nullptr;
    ts.rx_buffer = gLinkRxBuf;
    rc = spi_slave_transmit(LINK_SPI_HOST, &ts, pdMS_TO_TICKS(10));
    if (rc != ESP_OK) {
      gpio_set_level((gpio_num_t)PIN_LINK_CS, 1);
      gLinkRxTimeouts++;
      return false;
    }
    memcpy(payloadOut, gLinkRxBuf, payloadLen);
  }

  gpio_set_level((gpio_num_t)PIN_LINK_CS, 1);

  lenOut      = payloadLen;
  samplesOut  = h.samples;
  channelsOut = h.channels;
  rateHzOut   = (uint32_t)h.rateK * 1000u;
  silenceOut  = (h.flags & LINK_FLAG_SILENCE) != 0;
  gLinkFramesRx++;
  return true;
}
