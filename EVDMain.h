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

// ---- Deferred NVS flush ----------------------------------------------------
// Zones are NOT saved here. Every zone mutation already writes the table under
// zonesMutex (saveZonesToNVSLocked), and this flush runs on the loop task while
// a dashboard request runs on the AsyncTCP task: taking a snapshot and writing
// it 3 s later can persist a table that a create/delete has since changed,
// i.e. resurrect a deleted zone. Only the WiFi/settings block is deferred.
inline void serviceDeferredSettingsFlush() {
  if (!settingsDirty) return;
  if (millis() - settingsDirtySince < NVM_SAVE_DELAY_MS) return;
  settingsDirty = false;
  saveWiFiSettings();
}

inline void serviceFactoryResetPin() {
  static unsigned long heldSince = 0;
  pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);
  if (digitalRead(PIN_FACTORY_RESET) == LOW) {
    if (heldSince == 0) heldSince = millis();
    if (millis() - heldSince > FACTORY_RESET_HOLD_MS) {
      LOGW("factory reset: clearing NVS and restarting");
      // Scope the guard: ESP.restart() does not return, so a PrefsGuard still
      // in scope would never release prefsMutex or close the NVS handle. The
      // restart is a last resort anyway, but the clear must be committed
      // before it, which only end() (the destructor) guarantees.
      {
        PrefsGuard g("evdstr", false);
        if (g.ok) g.prefs.clear();
      }
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

  // LittleFS is mounted because the OTA path and the dashboard expect it; 
  // EVDSTR itself keeps no files there.
  if (!LittleFS.begin(true)) LOGW("LittleFS mount failed");

  // Zones before the HTTP server: the registration handler can run the moment
  // the server is up and it reads the zone table.
  loadZonesFromNVS();

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
  serviceDeferredSettingsFlush();

  // Clock sync: a request/response exchange, NOT a poll.
  // handleClockSyncRequests() parses at most one datagram per call and replies
  // to the node's source port. Draining the whole backlog per pass (bounded)
  // matters: one call per loop() iteration with a 5 ms sleep could not keep up
  // with a fleet of nodes, the socket's RX buffer filled during any stall (an
  // OTA flash write, a deferred NVS flush), and lwIP dropped the overflow with
  // no trace - which showed up only as nodes that never locked their clock.
  for (int i = 0; i < 4; ++i) {
    if (!handleClockSyncRequests()) break;   // true = a packet was handled
  }

  // The encoder mirror is an HTTP GET, and HTTPClient is not re-entrant. This
  // runs on the loop task (core 0, pri 1) every 2 s; the relay in EVDRoutes.h
  // takes the same mutex, so a dashboard request and this pull can never
  // overlap on one socket.
  // Not from here. loop() is the only task that services discovery, the
  // settings flush and clock sync; a blocking GET with a 1.5 s timeout every
  // 2 s (EVDCTR unreachable) stalled all three. The pull runs in its own task,
  // started in setup().
  delay(1);
}
