#pragma once
// ============================================================================
//  EVDWiFi.h - WiFi bring-up, mDNS, discovery for EVDSTR
// ============================================================================
#include <WiFi.h>
#include <ESPmDNS.h>

inline void loadWiFiSettings() {
  PrefsGuard g("evdstr", true);
  if (!g.ok) { LOGW("NVS: cannot open evdstr, using defaults"); return; }
  serverName    = g.prefs.getString("srvname", "EVDSTR");
  wifiSsid      = g.prefs.getString("ssid",    "");
  wifiPassword  = g.prefs.getString("pass",    "");
  useStaticIp   = g.prefs.getBool  ("static",  false);
  staticIp      = g.prefs.getString("ip",      "");
  staticGateway = g.prefs.getString("gw",      "");
  staticSubnet  = g.prefs.getString("sn",      "");
  staticDns     = g.prefs.getString("dns",     "");
  audioBufferMs = g.prefs.getUShort("bufms",   200);
  masterVolume  = g.prefs.getUChar ("vol",     100);
  masterMuted   = g.prefs.getBool  ("mute",    false);
  playbackModeIdx = g.prefs.getUChar("mode",   1);
  ctrHost       = g.prefs.getString("ctrhost", "");
  LOGI("settings: name=%s ssid=%s ctr=%s", serverName.c_str(),
       wifiSsid.c_str(), ctrHost.c_str());
}

inline void saveWiFiSettings() {
  PrefsGuard g("evdstr", false);
  if (!g.ok) { LOGE("NVS: cannot open evdstr for write"); return; }
  g.prefs.putString("srvname", serverName);
  g.prefs.putString("ssid",    wifiSsid);
  g.prefs.putString("pass",    wifiPassword);
  g.prefs.putBool  ("static",  useStaticIp);
  g.prefs.putString("ip",      staticIp);
  g.prefs.putString("gw",      staticGateway);
  g.prefs.putString("sn",      staticSubnet);
  g.prefs.putString("dns",     staticDns);
  g.prefs.putUShort("bufms",   audioBufferMs);
  g.prefs.putUChar ("vol",     masterVolume);
  g.prefs.putBool  ("mute",    masterMuted);
  g.prefs.putUChar ("mode",    playbackModeIdx);
  g.prefs.putString("ctrhost", ctrHost);
}

inline void startSetupAccessPoint() {
  String apName = sanitizeServerName(serverName);
  if (!apName.length()) apName = "EVDSTR";
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), strlen(default_ap_password) ? default_ap_password : nullptr);
  accessPointActive = true;
  LOGW("WiFi: AP fallback '%s' at %s", apName.c_str(),
       WiFi.softAPIP().toString().c_str());
}

inline void setup_wifi_sta() {
  if (!wifiSsid.length()) { startSetupAccessPoint(); return; }
  WiFi.mode(WIFI_STA);
  // No sleep: a 20 ms frame schedule on a dozing radio is a stutter generator.
  WiFi.setSleep(false);
  if (useStaticIp && staticIp.length()) {
    IPAddress ip, gw, sn, dns;
    if (ip.fromString(staticIp) && gw.fromString(staticGateway) &&
        sn.fromString(staticSubnet)) {
      if (!dns.fromString(staticDns)) dns = gw;
      WiFi.config(ip, gw, sn, dns);
    }
  }
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  const unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) { startSetupAccessPoint(); return; }

  accessPointActive = false;
  LOGI("WiFi: connected to '%s' as %s (rssi %d)", wifiSsid.c_str(),
       WiFi.localIP().toString().c_str(), WiFi.RSSI());

  String host = sanitizeServerName(serverName);
  if (!host.length()) host = "EVDSTR";
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS: http://%s.local/", host.c_str());
  }
}

inline void handleRestart(AsyncWebServerRequest *request) {
  request->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
  delay(150);
  ESP.restart();
}

// ---- Discovery -------------------------------------------------------------
// EVDSTR answers the same probe, but identifies itself as the streamer. This
// is what lets a freshly flashed CTR find its partner without anyone typing
// an IP, and what lets an operator tell the two boards apart in a scan.
inline void setupDiscoveryResponder() {
  if (!discoveryUdp.begin(DISCOVERY_PORT)) {
    LOGE("discovery: bind %d failed", DISCOVERY_PORT);
  }
}

inline void serviceDiscovery() {
  int sz = discoveryUdp.parsePacket();
  if (sz <= 0) return;
  char buf[96];
  int n = discoveryUdp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';
  if (strncmp(buf, DISCOVERY_MAGIC_REQ, strlen(DISCOVERY_MAGIC_REQ)) != 0) return;
  String reply = String(DISCOVERY_MAGIC_RPY) + " role=str name=" +
                 sanitizeServerName(serverName) + " ip=" +
                 WiFi.localIP().toString() + " ctr=" + ctrHost;
  discoveryUdp.beginPacket(discoveryUdp.remoteIP(), discoveryUdp.remotePort());
  discoveryUdp.write((const uint8_t *)reply.c_str(), reply.length());
  discoveryUdp.endPacket();
}

// ---- Encoder mirror pull ---------------------------------------------------
// The players are told the encoder's parameters on registration and on every
// ctrl push, and those parameters live on EVDCTR. One small GET every 2 s
// keeps this board's mirror current, so a dashboard change reaches the
// players without a return channel on the pin link.
inline void refreshEncoderMirror() {
  if (!ctrHost.length()) return;
  if (!lockFor(ctrPullMutex, 1000)) return;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(1500);
  if (http.begin(String("http://") + ctrHost + "/api/audio/encoding")) {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      DynamicJsonDocument d(512);
      if (!deserializeJson(d, body)) {
        if (d["bitrate"].is<int>())        g_currentBitrate       = d["bitrate"];
        if (d["complexity"].is<int>())     currentOpusComplexity  = d["complexity"];
        if (d["vbr"].is<bool>())           g_currentVbr           = d["vbr"] ? 1 : 0;
        if (d["vbrConstraint"].is<bool>()) g_currentVbrConstraint = d["vbrConstraint"] ? 1 : 0;
        if (d["sampleRate"].is<int>()) {
          uint32_t r = (uint32_t)(int)d["sampleRate"];
          if (r == OPUS_SAMPLE_RATE_48K || r == OPUS_SAMPLE_RATE_24K) g_opusSampleRate = r;
        }
        if (d["bufferMs"].is<int>()) audioBufferMs = (uint16_t)(int)d["bufferMs"];
        String sig = d["signal"] | "auto";
        g_currentSignal = (sig == "music") ? 2 : (sig == "voice") ? 1 : -1000;
        String app = d["application"] | "audio";
        g_currentApplication = (app == "audio") ? 1 : (app == "voip") ? 0 : 2;
        ctrLastPullMs = millis();
      }
    }
    http.end();
  }
  unlockFor(ctrPullMutex);
}
