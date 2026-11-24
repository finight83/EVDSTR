#pragma once
// ============================================================================
//  EVDHelpers.h - locks, escaping, and the subscriber/player table helpers
// ============================================================================
//  Every table access goes through one of these, so the audio task (core 0)
//  and the player task (core 1) can never see a partially updated entry.
// ============================================================================

struct PrefsGuard {
  Preferences prefs;
  bool        ok = false;
  bool        locked = false;
  explicit PrefsGuard(const char *ns, bool ro = false) {
    // [nvs-lock-no-forever] This used portMAX_DELAY. The board carried two
    // NVS handles (the zones loader and the settings loader) taken from
    // setup() with no bound, so a slow or stuck NVS operation on one task
    // froze every later reader with no timeout and no log - the whole
    // firmware stops at the next PrefsGuard, and setup() never finishes.
    // 400 ms is longer than any NVS read/write here and short enough that a
    // real failure is reported instead of hanging.
    locked = (!prefsMutex) || (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(400)) == pdTRUE);
    if (!locked) {
      LOGW("NVS: '%s' busy after 400 ms - skipped this access", ns);
      return;
    }
    ok = prefs.begin(ns, ro);
  }
  ~PrefsGuard() {
    if (ok) prefs.end();
    if (locked) xSemaphoreGive(prefsMutex);
  }
  PrefsGuard(const PrefsGuard &) = delete;
  PrefsGuard &operator=(const PrefsGuard &) = delete;
};

inline bool lockFor(SemaphoreHandle_t m, uint32_t ms) {
  if (!m) return false;
  return xSemaphoreTake(m, pdMS_TO_TICKS(ms)) == pdTRUE;
}
inline void unlockFor(SemaphoreHandle_t m) { if (m) xSemaphoreGive(m); }

// Accept "192.168.1.42", "192.168.1.42:8080", "evdctr.local", "EVDCTR".
// Kept identical to EVDCTR's copy: both boards validate a peer address before
// it becomes the destination of an HTTP POST.
inline bool isPairableHost(const String &h) {
  if (h.length() == 0 || h.length() > 253) return false;
  for (size_t i = 0; i < h.length(); ++i) {
    char c = h[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';
    if (!ok) return false;
  }
  if (h.indexOf("..") >= 0 || h.startsWith(".") || h.endsWith(".")) return false;
  return true;
}

inline String jsonEscape(const String &in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    unsigned char c = (unsigned char)in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  return out;
}

inline String htmlEscape(const String &in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '&': out += "&amp;";  break;
      case '"': out += "&quot;"; break;
      case '\'':out += "&#39;";  break;
      default:  out += c;
    }
  }
  return out;
}

inline String sanitizeServerName(const String &in) {
  String out; out.reserve(in.length());
  for (size_t i = 0; i < in.length() && out.length() < 32; ++i) {
    unsigned char c = (unsigned char)in[i];
    if (isAlphaNumeric(c)) out += (char)c;
  }
  return out;
}

// Zone names are alphanumeric + space + dash + underscore, 1-24 chars.
inline bool isValidZoneName(const String &n) {
  if (n.length() == 0 || n.length() > 24) return false;
  for (size_t i = 0; i < n.length(); ++i) {
    unsigned char c = (unsigned char)n[i];
    if (!(isAlphaNumeric(c) || c == ' ' || c == '-' || c == '_')) return false;
  }
  return true;
}

// ---- Subscriber table ------------------------------------------------------
// A snapshot is what the audio task works from, so a node that registers or
// dies mid-packet cannot change the fan-out set under it.
struct SubscriberSnap {
  String ip;
  bool   audioCompatible;
  int    negotiatedAudioProtocolVersion;
  bool   provisional;
};

inline std::vector<SubscriberSnap> snapshotSubscribers(unsigned long now) {
  std::vector<SubscriberSnap> out;
  out.reserve(MAX_UDP_SUBSCRIBERS);
  if (!lockFor(subsMutex, 20)) return out;
  for (int i = 0; i < subscriberCount; ++i) {
    const Subscriber &s = subscribers[i];
    if (s.ip.length() == 0) continue;
    if (now - s.lastSeenMs > PLAYER_STALE_TIMEOUT_MS) continue;
    if (s.consecutiveSendFailures >= MAX_SEND_FAILURES_BEFORE_SKIP) continue;
    if (!s.audioCompatible) continue;
    out.push_back({s.ip, s.audioCompatible, s.negotiatedAudioProtocolVersion,
                   s.provisional});
  }
  unlockFor(subsMutex);
  return out;
}

inline int findSubscriberLocked(const String &ip) {
  for (int i = 0; i < subscriberCount; ++i) {
    if (subscribers[i].ip == ip) return i;
  }
  return -1;
}

inline int findOrCreateSubscriberLocked(const String &ip, const String &name,
                                        int declaredProto, int negotiatedProto,
                                        bool compatible) {
  int slot = findSubscriberLocked(ip);
  if (slot >= 0) {
    subscribers[slot].name       = name;
    subscribers[slot].lastSeenMs = millis();
    subscribers[slot].declaredAudioProtocolVersion   = declaredProto;
    subscribers[slot].negotiatedAudioProtocolVersion = negotiatedProto;
    subscribers[slot].audioCompatible                = compatible;
    subscribers[slot].consecutiveSendFailures        = 0;
    return slot;
  }
  if (subscriberCount >= MAX_UDP_SUBSCRIBERS) return -1;
  slot = subscriberCount++;
  subscribers[slot] = Subscriber();
  subscribers[slot].ip    = ip;
  subscribers[slot].name  = name;
  subscribers[slot].lastSeenMs = millis();
  subscribers[slot].declaredAudioProtocolVersion   = declaredProto;
  subscribers[slot].negotiatedAudioProtocolVersion = negotiatedProto;
  subscribers[slot].audioCompatible                = compatible;
  return slot;
}

inline void pruneStaleSubscribers(unsigned long now) {
  if (!lockFor(subsMutex, 50)) return;
  for (int i = subscriberCount - 1; i >= 0; --i) {
    if (now - subscribers[i].lastSeenMs <= PLAYER_STALE_TIMEOUT_MS) continue;
    LOGW("subscriber %s pruned (silent %lu ms)", subscribers[i].ip.c_str(),
         (unsigned long)(now - subscribers[i].lastSeenMs));
    for (int j = i; j < subscriberCount - 1; ++j) subscribers[j] = subscribers[j + 1];
    subscriberCount--;
  }
  unlockFor(subsMutex);
}

// ---- Player table ----------------------------------------------------------
inline void pruneStalePlayersLocked(unsigned long now) {
  for (int i = (int)playerNodes.size() - 1; i >= 0; --i) {
    if (now - playerNodes[i].lastSeenMs > PLAYER_STALE_TIMEOUT_MS) {
      playerNodes.erase(playerNodes.begin() + i);
    }
  }
}

inline std::vector<PlayerNode> snapshotPlayers(unsigned long now) {
  std::vector<PlayerNode> out;
  if (!lockFor(playersMutex, 20)) return out;
  pruneStalePlayersLocked(now);
  out = playerNodes;
  unlockFor(playersMutex);
  return out;
}

// ---- Zones -----------------------------------------------------------------
inline std::vector<Zone> snapshotZones() {
  std::vector<Zone> out;
  if (!lockFor(zonesMutex, 50)) return out;
  out = zones;
  unlockFor(zonesMutex);
  return out;
}

// ---- Node perf -------------------------------------------------------------
inline void pruneStalePerfLocked(unsigned long now) {
  for (int i = (int)nodePerf.size() - 1; i >= 0; --i) {
    if (now - nodePerf[i].updatedMs > PLAYER_STALE_TIMEOUT_MS) {
      nodePerf.erase(nodePerf.begin() + i);
    }
  }
}

inline std::vector<NodePerf> snapshotPerf() {
  std::vector<NodePerf> out;
  if (!lockFor(perfMutex, 20)) return out;
  pruneStalePerfLocked(millis());
  out = nodePerf;
  unlockFor(perfMutex);
  return out;
}

// ---- Fragment policy for player-facing bodies ------------------------------
// A body that arrives in more than one TCP segment must be refused, not
// half-parsed: register would omit audioProtocolVersion and mark the node
// incompatible (silencing it), and a zone mutation would parse a truncated
// object as valid JSON. AsyncWebServer also aborts the connection on a 413, so
// "wait for the next segment" can never work - every segment has to be
// answered, and only the first one carries the verdict.
//
// Lives here rather than in EVDRoutes.h because EVDPlayers.h is included
// before it and uses both.
inline bool bodyIsFragmented(size_t index, size_t len, size_t total) {
  return index != 0 || len != total;
}

inline void answerFragmentedBody(AsyncWebServerRequest *request, size_t index,
                                 size_t len, size_t total) {
  if (index == 0) {
    request->send(413, "application/json",
                  "{\"ok\":false,\"error\":\"body fragmented; retry\"}");
  } else if (index + len >= total) {
    // Tail of a body whose head was already answered.
    request->send(200, "application/json", "{\"ok\":true,\"dropped\":true}");
  }
}

// ---- Request-body accumulation --------------------------------------------
// AsyncWebServer delivers a POST body in segments, and several requests can be
// in flight at once on the same AsyncTCP worker: a player's
// POST /api/player/register every few seconds lands on the same worker as a
// dashboard POST /api/config. A single `static String` here was therefore a
// race: request B's first segment reset the buffer while request A was still
// being accumulated, and A's handler parsed B's bytes.
//
// The buffer now lives on the request object (`request->_tempObject`), which
// AsyncWebServer frees when the request is destroyed. It is never shared, so
// overlapping requests cannot corrupt each other.
//
// Ownership: if the function returns true, the CALLER owns the buffer and must
// release it with releaseReqBody(request). If it returns false the buffer has
// already been released (and an error response sent, or a partial body
// abandoned).
inline bool accumulateReqBody(AsyncWebServerRequest *request,
                              const uint8_t *data, size_t len, size_t index,
                              size_t total, size_t cap,
                              const char *tooLargeJson,
                              const char *&bodyOut, size_t &lenOut) {
  String *slot = (String *)request->_tempObject;
  if (index == 0) {
    if (total > cap) {
      request->send(413, "application/json", tooLargeJson);
      return false;
    }
    if (slot) { delete slot; request->_tempObject = nullptr; }
    slot = new (std::nothrow) String();
    if (!slot) {
      request->send(500, "application/json", "{\"ok\":false,\"error\":\"no memory\"}");
      return false;
    }
    request->_tempObject = slot;
    slot->reserve(total + 1);
  } else if (!slot) {
    // A continuation with no head: the first segment was dropped (413) or this
    // is a stray tail. Nothing to append to.
    return false;
  }

  if (slot->length() + len > cap) {
    request->_tempObject = nullptr;
    delete slot;
    request->send(413, "application/json", tooLargeJson);
    return false;
  }

  // Bulk append: String::concat(const char*, n) copies n bytes in one pass.
  slot->concat((const char *)data, len);

  if (index + len >= total) {
    bodyOut = slot->c_str();
    lenOut  = slot->length();
    return true;
  }
  return false;
}

// Hand the accumulated body back. Call once the handler has finished reading
// bodyOut; nothing may touch bodyOut after this.
inline void releaseReqBody(AsyncWebServerRequest *request) {
  String *slot = (String *)request->_tempObject;
  if (!slot) return;
  request->_tempObject = nullptr;
  delete slot;
}

// ---- Log ring --------------------------------------------------------------
#include <cstdarg>
inline void evdLogPrintf(char level, const char *fmt, ...) {
  char buf[192];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&gLogRingMux);
    LogRingEntry &e = gLogRing[gLogRingSeq % EVD_LOG_RING_LEN];
    e.seq = ++gLogRingSeq; e.ms = millis(); e.level = level;
    strncpy(e.text, buf, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';
  portEXIT_CRITICAL(&gLogRingMux);
  Serial.printf("[%c] %s\n", level, buf);
}
