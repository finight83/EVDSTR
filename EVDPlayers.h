#pragma once
// ============================================================================
//  EVDPlayers.h - player registration, zones, config push, node stats
// ============================================================================
//
//  Everything a player can say to this board arrives here. The handlers run on
//  the AsyncTCP worker (core 0); the 30 s broadcast and the node-stats sweep
//  run on the player task (core 1). Both sides only touch the tables through
//  the helpers in EVDHelpers.h.
// ============================================================================
#include <ArduinoJson.h>

// Owned by EVDOTA.h (included after this file). Declared here because the
// zone handlers refuse to mutate config while an update is being written.
extern volatile bool otaInProgress;
extern volatile bool otaUploadInProgress;

// ---- Zones persistence -----------------------------------------------------
// Bar-encoded in NVS, one string: "id|name|vol|mute|ip,ip;id|name|..." — small
// enough for one NVS blob and atomic on write.
// Caller MUST already hold zonesMutex. snapshotZones() takes the same
// non-recursive mutex, so calling it from under the lock returned an EMPTY
// vector and persisted an empty zone table - zones gone after a reboot.
inline void saveZonesToNVSLocked() {
  String enc;
  for (size_t i = 0; i < zones.size(); ++i) {
    if (i) enc += ";";
    enc += zones[i].id + "|" + zones[i].name + "|" + String(zones[i].volume) + "|" +
           (zones[i].muted ? "1" : "0") + "|";
    for (size_t j = 0; j < zones[i].nodeIps.size(); ++j) {
      if (j) enc += ",";
      enc += zones[i].nodeIps[j];
    }
  }
  PrefsGuard g("evdstr", false);
  if (!g.ok) return;
  g.prefs.putString("zones", enc);
}

// Safe from anywhere: takes its own snapshot.
inline void saveZonesToNVS() {
  std::vector<Zone> zc = snapshotZones();
  String enc;
  for (size_t i = 0; i < zc.size(); ++i) {
    if (i) enc += ";";
    enc += zc[i].id + "|" + zc[i].name + "|" + String(zc[i].volume) + "|" +
           (zc[i].muted ? "1" : "0") + "|";
    for (size_t j = 0; j < zc[i].nodeIps.size(); ++j) {
      if (j) enc += ",";
      enc += zc[i].nodeIps[j];
    }
  }
  PrefsGuard g("evdstr", false);
  if (!g.ok) return;
  g.prefs.putString("zones", enc);
}

inline void loadZonesFromNVS() {
  String enc;
  {
    PrefsGuard g("evdstr", true);
    if (g.ok) enc = g.prefs.getString("zones", "");
  }
  std::vector<Zone> loaded;
  if (enc.length()) {
    int start = 0;
    while (start < (int)enc.length()) {
      int end = enc.indexOf(';', start);
      if (end < 0) end = enc.length();
      String rec = enc.substring(start, end);
      start = end + 1;
      int p1 = rec.indexOf('|');
      int p2 = rec.indexOf('|', p1 + 1);
      int p3 = rec.indexOf('|', p2 + 1);
      int p4 = rec.indexOf('|', p3 + 1);
      if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) continue;
      Zone z;
      z.id      = rec.substring(0, p1);
      z.name    = rec.substring(p1 + 1, p2);
      z.volume  = rec.substring(p2 + 1, p3).toInt();
      z.muted   = rec.substring(p3 + 1, p4) == "1";
      String ips = rec.substring(p4 + 1);
      int s = 0;
      while (s < (int)ips.length()) {
        int e = ips.indexOf(',', s);
        if (e < 0) e = ips.length();
        String ip = ips.substring(s, e);
        if (ip.length()) z.nodeIps.push_back(ip);
        s = e + 1;
      }
      loaded.push_back(z);
    }
  }
  if (loaded.empty()) {
    Zone main;
    main.id = MAIN_ZONE_ID; main.name = "Main Room"; main.volume = 100; main.muted = false;
    loaded.push_back(main);
  }
  if (lockFor(zonesMutex, 200)) {
    zones = loaded;
    unlockFor(zonesMutex);
  }
  LOGI("zones: loaded %u", (unsigned)loaded.size());
}

// ---- node perf -------------------------------------------------------------
inline void updateNodePerf(const String &ip, uint32_t latencyMs, uint32_t queueDepth,
                           uint32_t target, uint16_t rebuffers, unsigned long now) {
  if (!lockFor(perfMutex, 40)) return;
  for (auto &p : nodePerf) {
    if (p.ip == ip) {
      p.latencyMs = latencyMs; p.queueDepth = queueDepth; p.target = target;
      p.rebuffers = rebuffers; p.updatedMs = now; p.hasData = true;
      unlockFor(perfMutex);
      return;
    }
  }
  pruneStalePerfLocked(now);
  if (nodePerf.size() >= NODE_PERF_MAX_ENTRIES) { unlockFor(perfMutex); return; }
  NodePerf np;                    // aggregate init would not fill the strings
  np.ip = ip; np.latencyMs = latencyMs; np.queueDepth = queueDepth;
  np.target = target; np.rebuffers = rebuffers; np.updatedMs = now;
  np.hasData = true;
  nodePerf.push_back(np);
  unlockFor(perfMutex);
}

// ---- config push -----------------------------------------------------------
inline void sendZoneConfigToIp(const String &ip, int zvol, bool zmute) {
  bool haveCompat = false, compatible = false;
  int negotiatedVer = 0;
  String compatReason;
  {
    std::vector<PlayerNode> ps = snapshotPlayers(millis());
    for (auto &p : ps) {
      if (p.ip == ip) {
        haveCompat = true; compatible = p.audioCompatible;
        negotiatedVer = p.negotiatedAudioProtocolVersion;
        compatReason = p.audioCompatReason;
        break;
      }
    }
  }
  String json = String("{\"target\":\"") + ip + "\",\"type\":\"host_audio_sync\"" +
                ",\"zoneVolume\":" + String((float)zvol / 100.0f, 3) +
                ",\"zoneMuted\":"  + String(zmute ? "true" : "false") +
                ",\"sourceSampleRate\":" + String((unsigned)g_sourceSampleRate) +
                ",\"opusSampleRate\":"   + String((unsigned)g_opusSampleRate) +
                ",\"bufferMs\":"         + String(audioBufferMs) +
                ",\"mode\":\""           + String(playbackModeCStr()) + "\"" +
                ",\"epoch\":"            + String(serverEpoch) +
                ",\"clockSyncPort\":"    + String(CLOCK_SYNC_UDP_PORT);
  if (haveCompat) {
    json += ",\"audioProtocolVersion\":" + String(AUDIO_PROTO_VERSION) +
            ",\"negotiatedAudioProtocolVersion\":" + String(negotiatedVer) +
            ",\"audioCompatible\":" + String(compatible ? "true" : "false") +
            ",\"audioCompatReason\":\"" + compatReason + "\"";
  }
  json += "}";
  sendCtrlToSubscriber(ip, json);
}

inline void broadcastNodeConfig() {
  if (!subsMutex) return;
  std::vector<SubscriberSnap> subs = snapshotSubscribers(millis());
  std::vector<Zone> zcopy = snapshotZones();

  for (auto &s : subs) {
    int  zvol = 100;
    bool zmute = false;
    for (auto &z : zcopy) {
      if (z.id == MAIN_ZONE_ID) { zvol = z.volume; zmute = z.muted; break; }
    }
    for (auto &z : zcopy) {
      bool member = false;
      for (auto &n : z.nodeIps) if (n == s.ip) { member = true; break; }
      if (member) { zvol = z.volume; zmute = z.muted; break; }
    }

    const char *encSignalStr = (g_currentSignal == 2) ? "music" :
                               (g_currentSignal == 1) ? "voice" : "auto";
    const char *encAppStr = (g_currentApplication == 1) ? "audio" :
                            (g_currentApplication == 0) ? "voip" : "lowdelay";

    String json = String("{\"target\":\"") + s.ip +
                  "\",\"type\":\"host_audio_sync\",\"volume\":" +
                  String((float)masterVolume / 100.0f, 3) +
                  ",\"muted\":"      + String(masterMuted ? "true" : "false") +
                  ",\"zoneVolume\":" + String((float)zvol / 100.0f, 3) +
                  ",\"zoneMuted\":"  + String(zmute ? "true" : "false") +
                  ",\"bufferMs\":"   + String(audioBufferMs) +
                  ",\"sourceSampleRate\":" + String((unsigned)g_sourceSampleRate) +
                  ",\"opusSampleRate\":"   + String((unsigned)g_opusSampleRate) +
                  ",\"mode\":\"" + String(playbackModeCStr()) +
                  "\",\"audioProtocolVersion\":" + String(AUDIO_PROTO_VERSION) +
                  ",\"negotiatedAudioProtocolVersion\":" + String(s.negotiatedAudioProtocolVersion) +
                  ",\"audioCompatible\":" + String(s.audioCompatible ? "true" : "false") +
                  ",\"epoch\":" + String(serverEpoch) +
                  ",\"clockSyncPort\":" + String(CLOCK_SYNC_UDP_PORT) +
                  ",\"encBitrate\":" + String(g_currentBitrate) +
                  ",\"encComplexity\":" + String(currentOpusComplexity) +
                  ",\"encVbr\":" + String(g_currentVbr ? "true" : "false") +
                  ",\"encVbrConstraint\":" + String(g_currentVbrConstraint ? "true" : "false") +
                  ",\"encSignal\":\"" + encSignalStr + "\"" +
                  ",\"encApplication\":\"" + encAppStr + "\"" +
                  "}";
    sendCtrlToSubscriber(s.ip, json);
  }
}

// ---- registration ----------------------------------------------------------
inline void handlePlayerRegistration(AsyncWebServerRequest *request, uint8_t *data,
                                     size_t len, size_t index, size_t total) {
  if (otaUploadInProgress || otaInProgress) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"OTA in progress\"}");
    return;
  }
  // All player firmwares send the whole register body in one segment. A
  // fragmented body must not be parsed chunk-wise: the first partial chunk
  // would omit audioProtocolVersion and the node would be marked
  // incompatible, which silences it. Reject and let it retry.
  if (index != 0 || len != total) {
    if (index != 0) return;
    request->send(413, "application/json",
                  "{\"ok\":false,\"error\":\"register body fragmented; retry\"}");
    return;
  }
  String ip = request->client()->remoteIP().toString();
  String pName = "Player Node";
  String pVer;
  int declaredProtoVer = -1;

  if (len > 0 && data) {
    DynamicJsonDocument in(384);
    if (!deserializeJson(in, data, len)) {
      if (in["name"].is<const char*>()) {
        String raw = in["name"].as<String>();
        String safe; safe.reserve(raw.length());
        for (size_t i = 0; i < raw.length() && safe.length() < 32; ++i) {
          unsigned char c = (unsigned char)raw[i];
          if (isAlphaNumeric(c) || c == ' ' || c == '-' || c == '_' || c == '.')
            safe += (char)c;
        }
        pName = safe.length() ? safe : String("Player Node");
      }
      if (in["version"].is<const char*>()) pVer = in["version"].as<String>();
      if (in["audioProtocolVersion"].is<int>())
        declaredProtoVer = in["audioProtocolVersion"].as<int>();
    }
  }

  // Compat policy, unchanged: declare the version or get no audio. There is
  // no silent v1 fallback, because a v1 node would mis-parse a v2 header.
  bool compatible = false;
  int  negotiated = 0;
  const char *reason = "no_protocol_declared";
  if (declaredProtoVer == AUDIO_PROTO_VERSION) { compatible = true; negotiated = AUDIO_PROTO_VERSION; reason = "ok"; }
  else if (declaredProtoVer > AUDIO_PROTO_VERSION) { compatible = true; negotiated = AUDIO_PROTO_VERSION; reason = "negotiated_down"; }
  else if (declaredProtoVer >= 0) { reason = "player_protocol_too_old"; }

  bool isNew = false;
  if (lockFor(playersMutex, 50)) {
    pruneStalePlayersLocked(millis());
    bool known = false;
    for (auto &p : playerNodes) {
      if (p.ip == ip) {
        p.name = pName; p.version = pVer; p.lastSeenMs = millis();
        p.declaredAudioProtocolVersion = declaredProtoVer;
        p.negotiatedAudioProtocolVersion = negotiated;
        p.audioCompatible = compatible;
        p.audioCompatReason = reason;
        known = true; break;
      }
    }
    if (!known && playerNodes.size() < MAX_UDP_SUBSCRIBERS * 2) {
      PlayerNode np;                // named fields: PlayerNode has Strings and
      np.name = pName;              // defaults, so a braced list is fragile
      np.ip = ip;
      np.version = pVer;
      np.lastSeenMs = millis();
      np.declaredAudioProtocolVersion   = declaredProtoVer;
      np.negotiatedAudioProtocolVersion = negotiated;
      np.audioCompatible = compatible;
      np.audioCompatReason = reason;
      playerNodes.push_back(np);
      isNew = true;
    }
    unlockFor(playersMutex);
  }

  if (lockFor(subsMutex, 50)) {
    int slot = findOrCreateSubscriberLocked(ip, pName, declaredProtoVer,
                                            negotiated, compatible);
    if (slot >= 0) {
      subscribers[slot].provisional = false;
      if (isNew) subscribers[slot].lastSeenMs = millis();
    }
    unlockFor(subsMutex);
  }

  // A node seen for the first time joins Main Room, so per-zone volume/mute
  // reach it immediately instead of waiting for the operator to drag it in.
  if (isNew) {
    bool assigned = false;
    if (lockFor(zonesMutex, 80)) {
      bool inAny = false;
      for (auto &z : zones) for (auto &zip : z.nodeIps) if (zip == ip) { inAny = true; break; }
      if (!inAny) {
        for (auto &z : zones) {
          if (z.id == MAIN_ZONE_ID) {
            z.nodeIps.push_back(ip);
            // Save under the lock we already hold. The deferred settings
            // flush would do it later, but only through snapshotZones(),
            // which needs this same mutex.
            saveZonesToNVSLocked();
            assigned = true;
            break;
          }
        }
      }
      unlockFor(zonesMutex);
    }
    // LOGI() takes the log-ring lock. Calling it inside zonesMutex nests two
    // locks from the HTTP worker, which is how a lock-order stall starts.
    if (assigned) LOGI("zone: %s auto-assigned to Main Room", ip.c_str());
  }

  if (isNew) {
    LOGI("player registered: %s v%s from %s (proto=%d compatible=%s reason=%s)",
         pName.c_str(), pVer.length() ? pVer.c_str() : "?", ip.c_str(),
         declaredProtoVer, compatible ? "true" : "false", reason);

    int zvol = 100; bool zmute = false;
    {
      std::vector<Zone> zc = snapshotZones();
      bool found = false;
      for (auto &z : zc) {
        for (auto &zip : z.nodeIps) if (zip == ip) { zvol = z.volume; zmute = z.muted; found = true; break; }
        if (found) break;
      }
      if (!found) for (auto &z : zc) if (z.id == MAIN_ZONE_ID) { zvol = z.volume; zmute = z.muted; break; }
    }

    String json = String("{\"target\":\"") + ip +
                  "\",\"type\":\"host_audio_sync\",\"volume\":" +
                  String((float)masterVolume / 100.0f, 3) +
                  ",\"muted\":" + String(masterMuted ? "true" : "false") +
                  ",\"zoneVolume\":" + String((float)zvol / 100.0f, 3) +
                  ",\"zoneMuted\":" + String(zmute ? "true" : "false") +
                  ",\"bufferMs\":" + String(audioBufferMs) +
                  ",\"sourceSampleRate\":" + String((unsigned)g_sourceSampleRate) +
                  ",\"opusSampleRate\":" + String((unsigned)g_opusSampleRate) +
                  ",\"mode\":\"" + String(playbackModeCStr()) +
                  "\",\"audioProtocolVersion\":" + String(AUDIO_PROTO_VERSION) +
                  ",\"negotiatedAudioProtocolVersion\":" + String(negotiated) +
                  ",\"audioCompatible\":" + String(compatible ? "true" : "false") +
                  ",\"audioCompatReason\":\"" + String(reason) + "\"" +
                  ",\"epoch\":" + String(serverEpoch) +
                  ",\"clockSyncPort\":" + String(CLOCK_SYNC_UDP_PORT) + "}";
    sendCtrlToSubscriber(ip, json);

    char ts[128];
    int n = snprintf(ts, sizeof(ts),
        "{\"type\":\"time_sync\",\"serverMs\":%lu,\"epoch\":%lu,\"protoVersion\":%d}",
        (unsigned long)millis(), (unsigned long)serverEpoch, AUDIO_PROTO_VERSION);
    if (n > 0 && n < (int)sizeof(ts)) sendCtrlToSubscriber(ip, String(ts, (size_t)n));
  } else if (!compatible) {
    LOGW("player %s (%s) heartbeat: audio DISABLED, reason=%s",
         pName.c_str(), ip.c_str(), reason);
  }

  String resp = String("{\"ok\":true,\"audioCompatible\":") +
                (compatible ? "true" : "false") +
                ",\"negotiatedAudioProtocolVersion\":" + String(negotiated) +
                ",\"audioCompatReason\":\"" + String(reason) + "\"" +
                ",\"epoch\":" + String(serverEpoch) +
                ",\"clockSyncPort\":" + String(CLOCK_SYNC_UDP_PORT) +
                ",\"bufferMs\":" + String(audioBufferMs) + "}";
  request->send(200, "application/json", resp);
}

// ---- node stats ------------------------------------------------------------
inline void handleNodeStats(AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
  if (index != 0 || len != total) {
    if (index != 0) return;
    request->send(413, "application/json", "{\"ok\":false,\"error\":\"fragmented\"}");
    return;
  }
  String ip = request->client()->remoteIP().toString();
  if (len > 0 && data) {
    DynamicJsonDocument in(384);
    if (!deserializeJson(in, data, len)) {
      updateNodePerf(ip,
                     in["latencyMs"]  | 0,
                     in["queueDepth"] | 0,
                     in["target"]     | 0,
                     (uint16_t)(int)(in["rebuffers"] | 0),
                     millis());
      if (lockFor(subsMutex, 20)) {
        int slot = findSubscriberLocked(ip);
        if (slot >= 0) subscribers[slot].lastSeenMs = millis();
        unlockFor(subsMutex);
      }
    }
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

inline void handleOtaResult(AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
  // A player reports one small JSON object. Parse only the first segment: the
  // old code returned early on every later segment without ever sending a
  // response, so a fragmented body left the player's request hanging until its
  // own timeout.
  if (index != 0) { request->send(200, "application/json", "{\"ok\":true}"); return; }
  String ip = request->client()->remoteIP().toString();
  bool success = false;
  if (len > 0 && data) {
    DynamicJsonDocument in(192);
    if (!deserializeJson(in, data, len) && in["success"].is<bool>())
      success = in["success"].as<bool>();
  }
  LOGI("OTA result from %s: %s", ip.c_str(), success ? "ok" : "FAILED");
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---- node stats sweep (core 1) --------------------------------------------
// Player management, so it runs on core 1. What it does NOT do is speak HTTP:
// the node answers this sweep by POSTing its counters back to /api/node_stats,
// which the HTTP task on core 0 handles. Firing an HTTP GET at every node from
// here would put an HTTPClient (and a TCP handshake, and up to 1.2 s of
// blocking) on the core that also serves the fleet, once per node per 250 ms.
//
// So the sweep is the ctrl-plane nudge: one small datagram per node telling it
// "report now". The node's reply lands in updateNodePerf() above. This is the
// same shape the unsplit server used, minus the HTTP client.
inline void pollNodeStats() {
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 5000) return;   // once per 5 s per node
  lastPoll = millis();

  std::vector<SubscriberSnap> subs = snapshotSubscribers(millis());
  for (auto &s : subs) {
    String json = String("{\"target\":\"") + s.ip +
                  "\",\"type\":\"stats_request\"}";
    sendCtrlToSubscriber(s.ip, json);
  }
}

// ---- /api/players and /api/zones (read by EVDCTR's dashboard relay) -------
inline void handleApiPlayers(AsyncWebServerRequest *request) {
  std::vector<PlayerNode> ps = snapshotPlayers(millis());
  std::vector<NodePerf>   pf = snapshotPerf();
  String j = "[";
  for (size_t i = 0; i < ps.size(); ++i) {
    if (i) j += ",";
    j += "{\"name\":\"" + jsonEscape(ps[i].name) + "\",\"ip\":\"" + ps[i].ip +
         "\",\"version\":\"" + jsonEscape(ps[i].version) + "\"" +
         ",\"compatible\":" + (ps[i].audioCompatible ? "true" : "false") +
         ",\"reason\":\"" + jsonEscape(ps[i].audioCompatReason) + "\"";
    for (auto &p : pf) {
      if (p.ip == ps[i].ip && p.hasData) {
        j += ",\"latencyMs\":" + String((unsigned long)p.latencyMs) +
             ",\"queueDepth\":" + String((unsigned long)p.queueDepth) +
             ",\"target\":" + String((unsigned long)p.target) +
             ",\"rebuffers\":" + String((unsigned)p.rebuffers);
      }
    }
    j += "}";
  }
  j += "]";
  request->send(200, "application/json", j);
}

inline void handleApiZones(AsyncWebServerRequest *request) {
  std::vector<Zone> zc = snapshotZones();
  String j = "[";
  for (size_t i = 0; i < zc.size(); ++i) {
    if (i) j += ",";
    j += "{\"id\":\"" + jsonEscape(zc[i].id) + "\",\"name\":\"" +
         jsonEscape(zc[i].name) + "\",\"volume\":" + String(zc[i].volume) +
         ",\"muted\":" + (zc[i].muted ? "true" : "false") + ",\"nodes\":[";
    for (size_t k = 0; k < zc[i].nodeIps.size(); ++k) {
      if (k) j += ",";
      j += "\"" + zc[i].nodeIps[k] + "\"";
    }
    j += "]}";
  }
  j += "]";
  request->send(200, "application/json", j);
}

// ---- zone mutations --------------------------------------------------------
inline void handleZonesCreate(AsyncWebServerRequest *request, uint8_t *data,
                              size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the body buffer is still alive, then release it: the
  // document has its own copy, and nothing below reads body.
  bool badBody = ((deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["name"].is<const char*>());
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String name = in["name"].as<String>();
  if (!isValidZoneName(name)) {
    request->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"zone name: letters, digits, space, - _\"}");
    return;
  }
  String id = "z" + String(millis() & 0xFFFFFF, HEX);
  if (lockFor(zonesMutex, 100)) {
    Zone z; z.id = id; z.name = name; z.volume = 100; z.muted = false;
    zones.push_back(z);
    saveZonesToNVSLocked();
    unlockFor(zonesMutex);
  }
  broadcastNodeConfig();
  request->send(200, "application/json",
                String("{\"ok\":true,\"id\":\"") + id + "\"}");
}

inline void handleZonesDelete(AsyncWebServerRequest *request, uint8_t *data,
                              size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the body buffer is still alive, then release it: the
  // document has its own copy, and nothing below reads body.
  bool badBody = ((deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["id"].is<const char*>());
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String id = in["id"].as<String>();
  if (id == MAIN_ZONE_ID) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"Main Room cannot be deleted\"}");
    return;
  }
  if (lockFor(zonesMutex, 100)) {
    for (int i = (int)zones.size() - 1; i >= 0; --i) {
      if (zones[i].id == id) { zones.erase(zones.begin() + i); break; }
    }
    saveZonesToNVSLocked();
    unlockFor(zonesMutex);
  }
  broadcastNodeConfig();
  request->send(200, "application/json", "{\"ok\":true}");
}

inline void handleZonesMove(AsyncWebServerRequest *request, uint8_t *data,
                            size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the body buffer is still alive, then release it.
  bool badBody = ((deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["ip"].is<const char*>() ||
                  !in["zone"].is<const char*>());
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String ip = in["ip"].as<String>(), zone = in["zone"].as<String>();
  if (lockFor(zonesMutex, 120)) {
    for (auto &z : zones) {
      for (int i = (int)z.nodeIps.size() - 1; i >= 0; --i) {
        if (z.nodeIps[i] == ip) z.nodeIps.erase(z.nodeIps.begin() + i);
      }
    }
    for (auto &z : zones) if (z.id == zone) { z.nodeIps.push_back(ip); break; }
    saveZonesToNVSLocked();
    unlockFor(zonesMutex);
  }
  broadcastNodeConfig();
  // Tell the moved node its new zone's volume/mute, not a made-up 100/unmuted.
  // One lock, read once: two lockFor() calls around one lookup would let a
  // concurrent delete change the answer between them.
  int  zvol = 100;
  bool zmute = false;
  if (lockFor(zonesMutex, 80)) {
    for (auto &z : zones) if (z.id == zone) { zvol = z.volume; zmute = z.muted; break; }
    unlockFor(zonesMutex);
  }
  sendZoneConfigToIp(ip, zvol, zmute);
  request->send(200, "application/json", "{\"ok\":true}");
}

inline void handleZonesConfig(AsyncWebServerRequest *request, uint8_t *data,
                              size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the body buffer is still alive, then release it: the
  // document has its own copy, and nothing below reads body.
  bool badBody = ((deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["id"].is<const char*>());
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String id = in["id"].as<String>();
  int  vol  = in["volume"] | -1;
  bool muteSet = in["muted"].is<bool>();
  bool mute = in["muted"] | false;

  // The zone's own values are the ones that must reach its nodes: a request
  // that only sets `muted` must not reset the volume to 100, and one that only
  // sets `volume` must keep the zone muted.
  std::vector<String> targets;
  int  zvol = 100;
  bool zmute = false;
  if (lockFor(zonesMutex, 120)) {
    for (auto &z : zones) {
      if (z.id != id) continue;
      if (vol >= 0 && vol <= 100) z.volume = vol;
      if (muteSet) z.muted = mute;
      zvol = z.volume;
      zmute = z.muted;
      targets = z.nodeIps;
      break;
    }
    saveZonesToNVSLocked();
    unlockFor(zonesMutex);
  }
  for (auto &ip : targets) sendZoneConfigToIp(ip, zvol, zmute);
  broadcastNodeConfig();
  request->send(200, "application/json", "{\"ok\":true}");
}

inline void handleZonesRemoveNode(AsyncWebServerRequest *request, uint8_t *data,
                                  size_t len, size_t index, size_t total) {
  const char *body = nullptr; size_t bodyLen = 0;
  if (!accumulateReqBody(request, data, len, index, total, 512,
                         "{\"ok\":false,\"error\":\"too large\"}", body, bodyLen)) return;
  DynamicJsonDocument in(256);
  // Parse while the body buffer is still alive, then release it: the
  // document has its own copy, and nothing below reads body.
  bool badBody = ((deserializeJson(in, body, bodyLen) != DeserializationError::Ok) || !in["ip"].is<const char*>());
  releaseReqBody(request);
  if (badBody) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  String ip = in["ip"].as<String>();
  if (lockFor(zonesMutex, 120)) {
    for (auto &z : zones) {
      for (int i = (int)z.nodeIps.size() - 1; i >= 0; --i) {
        if (z.nodeIps[i] == ip) z.nodeIps.erase(z.nodeIps.begin() + i);
      }
    }
    saveZonesToNVSLocked();
    unlockFor(zonesMutex);
  }
  broadcastNodeConfig();
  request->send(200, "application/json", "{\"ok\":true}");
}
