#pragma once
// ============================================================================
//  EVDMain.h - setup() / loop() for EVDSTR
//  NOTE: setup() and loop() MUST NOT be 'inline' (Arduino main.cpp).
// ============================================================================
#include <esp_system.h>

// ---- Boot banner -----------------------------------------------------------
// Same shape as EVDCTR's, so a two-board bring-up reads as one log: which
// board, which firmware, why it rebooted, how the radio came up, and the
// compile-time constants a stale binary would get wrong. Serial.printf and not
// LOGI, because this runs before gSerialLogLevel means anything.
inline void printBootBanner(const char *phase) {
  Serial.printf("\n");
  Serial.printf("+------------------------------------------------------+\n");
  Serial.printf("|  EVDSTR  %-10s  stream / players half           |\n", EVD_FW_VERSION);
  Serial.printf("|  %-50s|\n", phase);
  Serial.printf("+------------------------------------------------------+\n");
}

inline const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (cold boot)";
    case ESP_RST_SW:        return "SOFTWARE (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (exception - see backtrace above)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (a task blocked too long)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power supply too weak)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

inline void setup_serial() {
  Serial.begin(LOG_BAUD);
  delay(300);

  printBootBanner("booting");
  Serial.printf("[BOOT] firmware      : EVDSTR %s\n", EVD_FW_VERSION);
  Serial.printf("[BOOT] reset reason  : %s\n", resetReasonStr(esp_reset_reason()));
  Serial.printf("[BOOT] chip          : %s rev %d, %d MHz, %d core(s)\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(),
                (int)getCpuFrequencyMhz(), (int)ESP.getChipCores());
  Serial.printf("[BOOT] flash         : %u MB, sketch %u KB\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)(ESP.getSketchSize() / 1024));
  Serial.printf("[BOOT] psram         : %u KB free\n",
                (unsigned)(ESP.getFreePsram() / 1024));
  Serial.printf("[BOOT] free heap     : %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
  Serial.printf("[BOOT] dashboard     : %u B gzip (EVDSTRCTRL.h via gen_gz.py)\n",
                (unsigned)EVDSTRCTRL_GZ_LEN);
  Serial.printf("[BOOT] audio in      : UDP %d out, %u Hz opus, %d ms frames\n",
                AUDIO_UDP_PORT, (unsigned)OPUS_SAMPLE_RATE_DEFAULT, OPUS_FRAME_MS);
  Serial.printf("[BOOT] pin link      : SCLK %d MOSI %d IRQ %d CS %d (slave)\n",
                PIN_LINK_SCLK, PIN_LINK_MOSI, PIN_LINK_IRQ, PIN_LINK_CS);
  Serial.printf("[BOOT] discovery     : UDP %d, probe every %d ms while unpaired\n",
                DISCOVERY_PORT, DISCOVERY_TX_MS);
  Serial.printf("\n");
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
  if (wifiSsid.length() && !wifiPassword.length()) {
    // A saved SSID with no password is almost always a password that was
    // cleared by a failed save, not an open network. Say so: this is the most
    // common reason a board falls back to its AP while the operator believes
    // it is configured.
    Serial.printf("[BOOT] NOTE          : ssid '%s' has NO password saved - "
                  "if that network is protected, the join will fail and this "
                  "board will open its own AP\n", wifiSsid.c_str());
  }
  Serial.printf("[BOOT] stored config : name '%s', ssid '%s', ctr '%s'\n",
                serverName.c_str(),
                wifiSsid.length() ? wifiSsid.c_str() : "(none - AP fallback)",
                ctrHost.length() ? ctrHost.c_str() : "(unpaired - searching)");

  // The radio before the audio pipeline and the HTTP server: this is the line
  // that answers "why can I not see it", and it must not be behind anything
  // that can fail.
  Serial.printf("[BOOT] wifi          : bringing up (timeout %d ms)\n",
                WIFI_CONNECT_TIMEOUT_MS);
  setup_wifi_sta();
  if (accessPointActive) {
    Serial.printf("[BOOT] wifi          : AP FALLBACK '%s' at http://%s/\n",
                  WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());
    if (!wifiSsid.length())
      Serial.printf("[BOOT] reason        : no Wi-Fi credentials stored yet\n");
    else
      Serial.printf("[BOOT] reason        : could not join '%s' within %d ms "
                    "(status %d) - wrong password, out of range, or 5 GHz only\n",
                    wifiSsid.c_str(), WIFI_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    Serial.printf("[BOOT] next          : join '%s' (no password), open "
                  "http://%s/ and enter your Wi-Fi\n",
                  WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[BOOT] wifi          : connected to '%s', ip %s, rssi %d dBm\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  (int)WiFi.RSSI());
    Serial.printf("[BOOT] dashboard     : http://%s/  or  http://%s.local/\n",
                  WiFi.localIP().toString().c_str(), serverName.c_str());
  }
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

  logLinkPeerState("boot");
  printBootBanner("boot complete");
  Serial.printf("[BOOT] reach me at  : http://%s/  (pair EVDCTR on the Stream tab)\n",
                accessPointActive ? WiFi.softAPIP().toString().c_str()
                                  : WiFi.localIP().toString().c_str());
  Serial.printf("[BOOT] steady state : errors only; raise the level with "
                "GET /api/log/level?level=3 or the Log tab\n\n");
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

  // ---- 10 s heartbeat ------------------------------------------------------
  // One line that says whether this board is actually doing its job: frames
  // arriving from EVDCTR over the pins, datagrams leaving over Wi-Fi, how many
  // players are listening, and whether the pairing exists. Level 1 (steady
  // state) on purpose - a box that is up but silent has to say so somewhere.
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    Serial.printf("[HB] up %lus | heap %uK | wifi %s %d dBm | ctr %s | "
                  "link %lu ms gap | udp %lu | players %d\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  accessPointActive ? "AP" : WiFi.SSID().c_str(), (int)WiFi.RSSI(),
                  ctrHost.length() ? ctrHost.c_str() : "(unpaired)",
                  (unsigned long)statLinkGapMs,
                  (unsigned long)statUdpSent, subscriberCount);
    if (statLinkGapMs > 400) logLinkPeerState("heartbeat");
  }

  delay(1);
}
