#pragma once
// ============================================================================
//  EVDRoutes.h - EVDSTR route table
// ============================================================================
//  The player-facing API is exactly the unsplit server's, so no player
//  firmware changes. The dashboard is smaller: the Audio tab lives on EVDCTR.
// ============================================================================
#include <ESPAsyncWebServer.h>

// EVDOTA.h is included by EVDSTR.ino before this file (it defines globals, so
// including it twice would be a redefinition). These are its entry points.
inline void setupOtaRoutes();

// The single server instance for this board.
AsyncWebServer server(80);

inline void sendJson(AsyncWebServerRequest *r, const String &body) {
  AsyncWebServerResponse *res = r->beginResponse(200, "application/json", body);
  res->addHeader("Cache-Control", "no-store, max-age=0");
  r->send(res);
}

// ---- /api/state ------------------------------------------------------------
// Read by EVDCTR's dashboard relay, so the field names match what the relay
// expects: subscribers, queueDepth, queueTarget, pacerLateMs, framesSent,
// framesDropped, rssi, heapFree, heapMinFree, upMs, ip, version.
inline void handleApiState(AsyncWebServerRequest *request) {
  uint32_t qd   = statQueueDepth;      // 32-bit store, atomic on this core
  uint32_t late = statPacerLateLastSec;

  int subs = 0;
  if (lockFor(subsMutex, 20)) { subs = subscriberCount; unlockFor(subsMutex); }

  String j = "{\"role\":\"str\",";
  j += "\"version\":\"" EVD_FW_VERSION "\",";
  j += "\"name\":\"" + jsonEscape(serverName) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"upMs\":" + String((unsigned long)millis()) + ",";
  j += "\"subscribers\":" + String(subs) + ",";
  j += "\"queueDepth\":" + String((unsigned long)qd) + ",";
  j += "\"queueTarget\":" + String((unsigned long)statQueueTarget) + ",";
  j += "\"pacerLateMs\":" + String((unsigned long)late) + ",";
  j += "\"framesSent\":" + String((unsigned long)statFramesSentTotal) + ",";
  j += "\"framesDropped\":" + String((unsigned long)statFramesDroppedTotal) + ",";
  j += "\"linkIn\":" + String((unsigned long)statLinkIn) + ",";
  j += "\"linkGapMs\":" + String((unsigned long)statLinkGapMs) + ",";
  j += "\"opusSampleRate\":" + String((unsigned)g_opusSampleRate) + ",";
  j += "\"opusFrameSamples\":" + String((unsigned)g_opusFrameSamples) + ",";
  j += "\"channels\":" + String((unsigned)g_opusChannels) + ",";
  j += "\"linkLastOkMs\":" + String((unsigned long)statLinkLastOkMs) + ",";
  j += "\"linkStale\":" + String((unsigned long)statLinkStale) + ",";
  j += "\"encBitrate\":" + String(g_currentBitrate) + ",";
  j += "\"encComplexity\":" + String(currentOpusComplexity) + ",";
  j += "\"bufferMs\":" + String((unsigned)audioBufferMs) + ",";
  j += "\"volume\":" + String((int)masterVolume) + ",";
  j += "\"muted\":" + String(masterMuted ? "true" : "false") + ",";
  j += "\"mode\":\"" + String(playbackModeCStr()) + "\",";
  j += "\"heapFree\":" + String((unsigned long)ESP.getFreeHeap()) + ",";
  j += "\"heapMinFree\":" + String((unsigned long)ESP.getMinFreeHeap()) + ",";
  j += "\"ctr\":\"" + jsonEscape(ctrHost) + "\",";
  j += "\"discoveryProbes\":" + String((unsigned long)statDiscoveryProbes) + "}";
  sendJson(request, j);
}

// ---- /api/stats ------------------------------------------------------------
inline void handleApiStats(AsyncWebServerRequest *request) {
  String j = "{\"role\":\"str\",";
  j += "\"udpSent\":" + String((unsigned long)statUdpSent) + ",";
  j += "\"udpErrors\":" + String((unsigned long)statUdpErrors) + ",";
  j += "\"syncRequests\":" + String((unsigned long)statSyncRequests) + ",";
  j += "\"syncErrors\":" + String((unsigned long)statSyncRequestErrors) + ",";
  j += "\"pacerLateMaxMs\":" + String((unsigned long)statPacerLateMax) + ",";
  j += "\"pacerStaleSkips\":" + String((unsigned long)statPacerStaleSkips) + ",";
  j += "\"framesSent\":" + String((unsigned long)statFramesSentTotal) + ",";
  j += "\"framesDropped\":" + String((unsigned long)statFramesDroppedTotal) + ",";
  j += "\"linkIn\":" + String((unsigned long)statLinkIn) + ",";
  // [link-counters-live] linkInBad was declared but never written, so this
  // field read 0 forever - including while the link was corrupting frames.
  j += "\"linkInBad\":" + String((unsigned long)linkBadHeaders()) + ",";
  j += "\"linkRxTimeouts\":" + String((unsigned long)linkRxTimeouts()) + ",";
  j += "\"linkSilence\":" + String((unsigned long)statLinkSilence) + ",";
  j += "\"linkStale\":" + String((unsigned long)statLinkStale) + ",";
  j += "\"linkGapMs\":" + String((unsigned long)statLinkGapMs) + ",";
  j += "\"heapFree\":" + String((unsigned long)ESP.getFreeHeap()) + ",";
  j += "\"heapMinFree\":" + String((unsigned long)ESP.getMinFreeHeap()) + ",";
  j += "\"uptimeMs\":" + String((unsigned long)millis()) + "}";
  sendJson(request, j);
}

// ---- /api/config -----------------------------------------------------------
// Volume, mute, mode and buffer latency are stream settings: they live here
// and are pushed to every node. EVDCTR relays its dashboard's /api/config to
// this handler.
inline void handleApiConfig(AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
  if (otaInProgress || otaUploadInProgress) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"OTA in progress\"}");
    return;
  }
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 1024,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(512);
  // Parse and judge BEFORE the release: `body` points into the buffer that
  // releaseReqBody() frees, so a parse folded into the same expression as the
  // release reads freed memory (in practice it still parses, which is why it
  // "worked", but it is a use-after-free on every POST /api/config).
  DeserializationError bodyErr = deserializeJson(in, body, bodyLen);
  if (bodyErr != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  bool changed = false;

  if (in["masterMuted"].is<bool>()) { masterMuted = in["masterMuted"].as<bool>(); changed = true; }
  if (in["masterVolume"].is<int>()) {
    int v = in["masterVolume"];
    if (v < 0 || v > 100) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"volume 0-100\"}");
      return;
    }
    masterVolume = (uint8_t)v;
    if (!in.containsKey("masterMuted")) masterMuted = false;
    changed = true;
  }
  if (in["mode"].is<const char*>()) {
    String m = in["mode"].as<String>(); m.toLowerCase();
    if (!(m == "off" || m == "music" || m == "movie")) {
      request->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"mode off|music|movie\"}");
      return;
    }
    playbackModeIdx = (m == "off") ? 0 : (m == "movie") ? 2 : 1;
    changed = true;
  }
  if (in["audioBufferMs"].is<int>()) {
    int bm = in["audioBufferMs"];
    if (bm < AUDIO_BUFFER_MIN_MS || bm > AUDIO_BUFFER_MAX_MS) {
      request->send(400, "application/json",
                    String("{\"ok\":false,\"error\":\"buffer ") +
                    String(AUDIO_BUFFER_MIN_MS) + "-" + String(AUDIO_BUFFER_MAX_MS) + " ms\"}");
      return;
    }
    audioBufferMs = (uint16_t)bm;
    changed = true;
  }
  if (in["serverName"].is<const char*>()) {
    String v = sanitizeServerName(in["serverName"].as<String>());
    if (v.length()) { serverName = v; changed = true; }
  }
  if (changed) {
    broadcastNodeConfig();
    settingsDirty = true;
    settingsDirtySince = millis();
  }
  handleApiState(request);
}

// ---- /api/audio/encoding ---------------------------------------------------
// The encoder is on EVDCTR. This endpoint exists so a player or a tool that
// asks this board gets an honest answer rather than a 404: the values are the
// mirror this board keeps, refreshed every 2 s from EVDCTR.
inline void handleGetAudioEncoding(AsyncWebServerRequest *request) {
  String j = "{\"ok\":true,\"authority\":\"EVDCTR\",";
  j += "\"bitrate\":" + String(g_currentBitrate) + ",";
  j += "\"complexity\":" + String(currentOpusComplexity) + ",";
  j += "\"vbr\":" + String(g_currentVbr ? "true" : "false") + ",";
  j += "\"vbrConstraint\":" + String(g_currentVbrConstraint ? "true" : "false") + ",";
  j += "\"signal\":\"" + String(g_currentSignal == 2 ? "music" :
                                g_currentSignal == 1 ? "voice" : "auto") + "\",";
  j += "\"application\":\"" + String(g_currentApplication == 1 ? "audio" :
                                     g_currentApplication == 0 ? "voip" : "lowdelay") + "\",";
  j += "\"sampleRate\":" + String((unsigned)g_opusSampleRate) + ",";
  j += "\"bufferMs\":" + String((unsigned)audioBufferMs) + "}";
  sendJson(request, j);
}

// ---- source / cmd ----------------------------------------------------------
// The source board is wired to EVDCTR's UART, so these are relayed there.
inline void handleSourceRelay(AsyncWebServerRequest *request, uint8_t *data,
                              size_t len, size_t index, size_t total,
                              const char *path) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  if (!ctrHost.length()) {
    releaseReqBody(request);
    request->send(503, "application/json",
                  "{\"ok\":false,\"error\":\"EVDCTR not paired\"}");
    return;
  }
  // The HTTP task is on core 0 with the pacer. A relay to a slow EVDCTR must
  // not hold it, so the timeout is short and the same mutex the mirror pull
  // uses keeps two relays from stacking up on one socket.
  if (!lockFor(ctrPullMutex, 300)) {
    releaseReqBody(request);
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"relay busy\"}");
    return;
  }
  // From here on the mutex MUST be released on every path, including an early
  // return: an unlock missed on one branch leaves every later relay answering
  // 503 "relay busy" until the board reboots. An RAII guard cannot be missed.
  struct RelayUnlock {
    SemaphoreHandle_t m;
    ~RelayUnlock() { unlockFor(m); }
  } relayUnlock{ctrPullMutex};
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(1500);
  if (!http.begin(String("http://") + ctrHost + path)) {
    releaseReqBody(request);
    request->send(502, "application/json", "{\"ok\":false,\"error\":\"EVDCTR unreachable\"}");
    return;   // relayUnlock releases the mutex
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)body, bodyLen);
  // The relay is the one handler that needs the body after parsing: EVDCTR
  // receives it verbatim. Release only now.
  releaseReqBody(request);
  String out = code > 0 ? http.getString() : "";
  http.end();
  if (code <= 0) request->send(502, "application/json", "{\"ok\":false,\"error\":\"relay failed\"}");
  else request->send(code, "application/json", out);
}

// ---- pairing ---------------------------------------------------------------
// The Stream tab on EVDCTR sets its STR address; this is the reverse, so a
// freshly flashed EVDSTR can be pointed at its controller from its own page.
inline void handlePairCtr(AsyncWebServerRequest *request, uint8_t *data,
                          size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the buffer is alive (see handleApiConfig): the release frees
  // the memory `body` points at.
  DeserializationError bodyErr = deserializeJson(in, body, bodyLen);
  bool badBody = (bodyErr != DeserializationError::Ok) || !in["host"].is<const char*>();
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String host = in["host"].as<String>();
  host.trim();
  if (host.startsWith("http://")) host = host.substring(7);
  while (host.endsWith("/")) host.remove(host.length() - 1);
  // Same gate as EVDCTR's pairing: a hostname or IPv4 address, nothing that
  // could be a path or a scheme.
  if (!isPairableHost(host)) {
    request->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"expected an IPv4 address or hostname\"}");
    return;
  }
  ctrHost = host;
  saveWiFiSettings();
  request->send(200, "application/json",
                "{\"ok\":true,\"host\":\"" + jsonEscape(ctrHost) + "\"}");
}

// ---- log -------------------------------------------------------------------
// The ring is written from both cores (LOGI from setup/loop and from the
// player task), so the entries are copied out under the lock and the JSON is
// built outside it. Building a String inside a critical section would block
// the other core for the whole allocation, and on a fragmented heap that is
// milliseconds, not microseconds.
inline void handleLogTail(AsyncWebServerRequest *request) {
  uint32_t since = request->hasParam("since")
                   ? (uint32_t)request->getParam("since")->value().toInt() : 0;
  struct Line { uint32_t seq, ms; char level; char text[192]; };
  static Line lines[EVD_LOG_RING_LEN];
  int n = 0;
  uint32_t end = 0;
  portENTER_CRITICAL(&gLogRingMux);
    end = gLogRingSeq;
    uint32_t oldest = (end >= EVD_LOG_RING_LEN) ? (end - EVD_LOG_RING_LEN + 1) : 1;
    uint32_t start = since + 1;
    if (start < oldest) start = oldest;
    for (uint32_t s = start; s <= end && n < EVD_LOG_RING_LEN; ++s) {
      const LogRingEntry &e = gLogRing[(s - 1) % EVD_LOG_RING_LEN];
      if (e.seq != s) continue;
      lines[n].seq = e.seq; lines[n].ms = e.ms; lines[n].level = e.level;
      memcpy(lines[n].text, e.text, sizeof(lines[n].text));
      lines[n].text[sizeof(lines[n].text) - 1] = '\0';
      ++n;
    }
  portEXIT_CRITICAL(&gLogRingMux);

  String body = "{\"ok\":true,\"lines\":[";
  for (int i = 0; i < n; ++i) {
    if (i) body += ",";
    body += "{\"seq\":" + String(lines[i].seq) + ",\"ms\":" + String(lines[i].ms) +
            ",\"level\":\"" + String(lines[i].level) + "\",\"text\":\"" +
            jsonEscape(String(lines[i].text)) + "\"}";
  }
  body += "],\"nextSeq\":" + String(end) + "}";
  sendJson(request, body);
}

// ---- setup -----------------------------------------------------------------
inline void setup_http_server() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *res = request->beginResponse_P(
        200, "text/html", EVDSTRCTRL_GZ, EVDSTRCTRL_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-store, max-age=0");
    request->send(res);
  });

  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/stats", HTTP_GET, handleApiStats);
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleApiConfig);
  server.on("/api/players", HTTP_GET, handleApiPlayers);
  server.on("/api/zones",   HTTP_GET, handleApiZones);
  server.on("/api/audio/encoding", HTTP_GET, handleGetAudioEncoding);

  // ---- player API (must match EnkelvoudESPPlayer exactly) ----
  server.on("/api/player/register", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handlePlayerRegistration);
  server.on("/api/node_stats",      HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleNodeStats);
  server.on("/api/player/ota_result", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleOtaResult);

  // ---- zones ----
  server.on("/api/zones/create",      HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleZonesCreate);
  server.on("/api/zones/delete",      HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleZonesDelete);
  server.on("/api/zones/move",        HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleZonesMove);
  server.on("/api/zones/config",      HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleZonesConfig);
  server.on("/api/zones/remove-node", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleZonesRemoveNode);

  // ---- relayed to EVDCTR ----
  server.on("/api/source", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr,
    [](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) {
      handleSourceRelay(r, d, l, i, t, "/api/source");
    });
  server.on("/api/cmd", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr,
    [](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) {
      handleSourceRelay(r, d, l, i, t, "/api/cmd");
    });

  // ---- pairing / log / misc ----
  server.on("/api/link/pair", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handlePairCtr);
  server.on("/api/log/tail",  HTTP_GET, handleLogTail);
  server.on("/api/restart",   HTTP_POST, handleRestart);
  server.on("/api/discover",  HTTP_GET, [](AsyncWebServerRequest *req) {
    sendJson(req, String("{\"role\":\"str\",\"name\":\"") + jsonEscape(serverName) +
                   "\",\"ip\":\"" + WiFi.localIP().toString() +
                   "\",\"ctr\":\"" + jsonEscape(ctrHost) + "\"}");
  });

  setupOtaRoutes();

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });

  server.begin();
  LOGI("HTTP server started on :80");
}
