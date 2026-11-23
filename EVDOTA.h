#pragma once
// ============================================================================
//  EVDOTA.h - EVDSTR firmware update
// ============================================================================
//
//  EVDSTR is flashed from EVDCTR: the operator uploads the image once, to the
//  board with the LittleFS spool and the dashboard, and EVDCTR pushes it over
//  WiFi to /api/ota/upload here. The pins carry audio, not firmware.
//
//  This endpoint is the receiving end, and it is the same one the unsplit
//  server exposed to its nodes, so a tool that already knows that protocol
//  works unchanged.
// ============================================================================
#include <Update.h>

volatile bool otaUploadInProgress = false;
volatile bool otaInProgress       = false;
bool          otaUploadFailed     = false;
String        otaUploadMessage    = "";

inline void handleOtaStatus(AsyncWebServerRequest *request) {
  String j = String("{\"ok\":true,\"uploading\":") + (otaUploadInProgress ? "true" : "false") +
             ",\"applying\":" + (otaInProgress ? "true" : "false") +
             ",\"message\":\"" + jsonEscape(otaUploadMessage) + "\"" +
             ",\"version\":\"" EVD_FW_VERSION "\"}";
  sendJson(request, j);
}

inline void handleOtaUpload(AsyncWebServerRequest *request, String filename,
                            size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    otaUploadFailed  = false;
    otaUploadInProgress = true;
    otaUploadMessage = "receiving " + filename;
    LOGI("OTA: receiving %s", filename.c_str());
    // Pause nothing: this board has no encoder. The pacer keeps running on
    // the frames already in the queue and simply starves while the flash
    // writes, which the players conceal exactly like any other gap.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaUploadFailed = true;
      otaUploadMessage = Update.errorString();
      LOGE("OTA: Update.begin failed: %s", Update.errorString());
      return;
    }
  }
  if (!otaUploadFailed && len) {
    if (Update.write(data, len) != len) {
      otaUploadFailed = true;
      otaUploadMessage = Update.errorString();
      LOGE("OTA: write failed: %s", Update.errorString());
    }
  }
  if (final) {
    otaUploadInProgress = false;
    if (!otaUploadFailed) {
      if (Update.end(true)) {
        otaUploadMessage = "applied, restarting";
        LOGI("OTA: applied, restarting");
        delay(300);
        ESP.restart();
      } else {
        otaUploadFailed = true;
        otaUploadMessage = Update.errorString();
      }
    }
  }
}

inline void handleOtaApply(AsyncWebServerRequest *request, uint8_t *data,
                           size_t len, size_t index, size_t total) {
  // The upload path already restarts on success, so Apply is a no-op ack for
  // callers that send it separately (EVDCTR does, for a clean error path).
  request->send(200, "application/json",
                "{\"ok\":true,\"message\":\"image applied by upload\"}");
}

inline void setupOtaRoutes() {
  server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  server.on("/api/ota/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      bool ok = !otaUploadFailed;
      request->send(ok ? 200 : 500, "application/json",
                    String("{\"ok\":") + (ok ? "true" : "false") +
                    ",\"message\":\"" + jsonEscape(otaUploadMessage) + "\"}");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      handleOtaUpload(request, filename, index, data, len, final);
    });
  server.on("/api/ota/apply", HTTP_POST, [](AsyncWebServerRequest *) {}, nullptr, handleOtaApply);
  LOGI("OTA: upload endpoint ready");
}
