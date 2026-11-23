#pragma once
// ============================================================================
//  EVDMain.h - setup() / loop() for EVDSTR
//  NOTE: setup() and loop() MUST NOT be 'inline' (Arduino main.cpp).
// ============================================================================
#include <esp_system.h>

inline void setup_serial() {
  Serial.begin(LOG_BAUD);
  delay(300);
  LOGI("========================================================");
  LOGI("EVDSTR %s - streaming half of the split server", EVD_FW_VERSION);
  LOGI("========================================================");
  esp_reset_reason_t reason = esp_reset_reason();
  const char *why = "UNKNOWN";
  switch (reason) {
    case ESP_RST_POWERON:  why = "POWERON";  break;
    case ESP_RST_SW:       why = "SOFTWARE"; break;
    case ESP_RST_PANIC:    why = "PANIC";    break;
    case ESP_RST_TASK_WDT: why = "TASK_WDT"; break;
    case ESP_RST_BROWNOUT: why = "BROWNOUT"; break;
    default: break;
  }
  LOGI("reset reason: %s", why);
}

inline void createSyncPrimitives() {
  prefsMutex    = xSemaphoreCreateMutex();
  subsMutex     = xSemaphoreCreateMutex();
  playersMutex  = xSemaphoreCreateMutex();
  zonesMutex    = xSemaphoreCreateMutex();
  perfMutex     = xSemaphoreCreateMutex();
  ctrlMutex     = xSemaphoreCreateMutex();
  ctrPullMutex  = xSemaphoreCreateMutex();
  if (!prefsMutex || !subsMutex || !playersMutex || !zonesMutex || !perfMutex ||
      !ctrlMutex || !ctrPullMutex) {
    LOGE("mutex creation failed");
  }
}

inline void serviceDeferredSettingsFlush() {
  if (!settingsDirty) return;
  if (millis() - settingsDirtySince < NVM_SAVE_DELAY_MS) return;
  settingsDirty = false;
  saveWiFiSettings();
  saveZonesToNVS();
}

inline void serviceFactoryResetPin() {
  static unsigned long heldSince = 0;
  pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
  if (digitalRead(PIN_FACTORY_RESET) == LOW) {
    if (heldSince == 0) heldSince = millis();
    if (millis() - heldSince > FACTORY_RESET_HOLD_MS) {
      LOGW("factory reset: clearing NVS and restarting");
      PrefsGuard g("evdstr", false);
      if (g.ok) g.prefs.clear();
      delay(300);
      ESP.restart();
    }
  } else {
    heldSince = 0;
  }
}

// ============================================================================
//  setup()
// ============================================================================
void setup() {
  setup_serial();
  createSyncPrimitives();

  if (!LittleFS.begin(true)) LOGW("LittleFS mount failed");
  loadWiFiSettings();
  setup_wifi_sta();
  serverEpoch = esp_random();

  // Slave side of the pin link before the audio task exists, so the SPI
  // peripheral is configured on core 0 and then owned by one task.
  if (!setupLinkSpi()) {
    LOGE("pin link init FAILED - no audio will arrive from EVDCTR");
  } else {
    LOGI("pin link ready: SCLK %d MOSI %d IRQ %d CS %d (slave)",
         PIN_LINK_SCLK, PIN_LINK_MOSI, PIN_LINK_IRQ, PIN_LINK_CS);
  }

  loadZonesFromNVS();
  setupDiscoveryResponder();
  setup_udp_transport();
  setup_http_server();
  setup_queues_and_tasks();

  LOGI("boot complete: http://%s/  (pair EVDCTR on the Stream tab)",
       WiFi.localIP().toString().c_str());
}

// ============================================================================
//  loop() - core 0, priority 1
// ============================================================================
void loop() {
  serviceFactoryResetPin();
  serviceDiscovery();
  handleClockSyncRequests();   // polled here: one packet per node per few s
  serviceDeferredSettingsFlush();

  static unsigned long lastMirror = 0;
  if (millis() - lastMirror > 2000) {
    lastMirror = millis();
    refreshEncoderMirror();
  }

  delay(5);
}
