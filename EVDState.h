#pragma once
// ============================================================================
//  EVDState.h - EVDSTR mutable globals
// ============================================================================
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <atomic>

// ---- Encoder mirror --------------------------------------------------------
// This board does not encode, but the players are told the encoder's
// parameters on every registration and ctrl push, and the dashboard shows
// them. The values arrive from EVDCTR over the relay (see EVDRoutes.h
// /api/audio/encoding) and from the link header (rate, per frame).
uint32_t g_opusSampleRate   = OPUS_SAMPLE_RATE_DEFAULT;
uint16_t g_opusFrameSamples = OPUS_SAMPLE_RATE_DEFAULT / (1000 / OPUS_FRAME_MS);
uint8_t  g_opusChannels     = CHANNELS;
uint32_t g_sourceSampleRate = 48000;
int g_currentBitrate        = 96000;
int currentOpusComplexity   = 10;
int g_currentVbr            = 1;
int g_currentVbrConstraint  = 1;
int g_currentSignal         = OPUS_AUTO;
int g_currentApplication    = 1;   // OPUS_APPLICATION_AUDIO

// ---- Stream settings -------------------------------------------------------
uint16_t audioBufferMs = 200;
uint8_t  masterVolume  = 100;
bool     masterMuted   = false;
uint8_t  playbackModeIdx = 1;      // 0 off, 1 music, 2 movie
inline const char *playbackModeCStr() {
  switch (playbackModeIdx) {
    case 0:  return "off";
    case 2:  return "movie";
    default: return "music";
  }
}

// ---- Queues ----------------------------------------------------------------
QueueHandle_t opusQueue = nullptr;

// ---- Subscriber / player tables -------------------------------------------
// Both are guarded by their own mutex and only ever touched through the
// helpers in EVDHelpers.h, so the audio task and the HTTP task never see a
// half-built entry.
Subscriber subscribers[MAX_UDP_SUBSCRIBERS];
int        subscriberCount = 0;
SemaphoreHandle_t subsMutex = nullptr;

std::vector<PlayerNode> playerNodes;
SemaphoreHandle_t playersMutex = nullptr;

std::vector<Zone> zones;
SemaphoreHandle_t zonesMutex = nullptr;

std::vector<NodePerf> nodePerf;
SemaphoreHandle_t perfMutex = nullptr;

// ---- Sockets ---------------------------------------------------------------
WiFiUDP audioUdp;
WiFiUDP ctrlUdp;
WiFiUDP clockSyncUdp;
WiFiUDP discoveryUdp;
SemaphoreHandle_t ctrlMutex = nullptr;

// ---- Multi-server election (P3: servers decide, see EVDSTRPeers.h) ---------
// Read by udpSendTask, the discovery responder and the registration handler;
// written by peerEval() from loop(). Single-server installs stay true.
volatile bool g_peerIsLeader = true;
int           g_peerPriority = 2;   // 1 = highest; PC defaults to 1

// ---- Statistics ------------------------------------------------------------
uint32_t statOpusEncoded   = 0;   // frames queued from the link
uint32_t statOpusDropped   = 0;   // queue full / link stale
uint32_t statUdpSent       = 0;   // datagrams actually put on the wire
uint32_t statUdpErrors     = 0;
uint32_t statSyncRequests  = 0;
uint32_t statSyncRequestErrors = 0;
uint32_t statPacerLateMax  = 0;
uint32_t statPacerLateLastSec = 0;
uint32_t statPacerStaleSkips  = 0;
uint32_t statFramesSentTotal  = 0;
uint32_t statFramesDroppedTotal = 0;
uint32_t statQueueDepth    = 0;
uint32_t statQueueTarget   = 3;

portMUX_TYPE gStatsMux = portMUX_INITIALIZER_UNLOCKED;

// ---- Link health ----------------------------------------------------------
// DEFINED in EVDAudio.h, which comes later in the include order, so this is a
// declaration only. All of these are written by audioLinkTask (core 0) and
// read by the HTTP task and playerTask.
extern uint32_t statLinkIn;
// (no statLinkInBad: the counters live in EVDLinkSpi.h, see linkBadHeaders())
extern uint32_t statLinkSilence;
extern uint32_t statLinkStale;
extern uint32_t statLinkLastOkMs;
extern uint32_t statLinkGapMs;
extern uint32_t gWireSeq;
extern uint32_t gLastLinkedRate;

// ---- Log ring --------------------------------------------------------------
#define EVD_LOG_RING_LEN 32
struct LogRingEntry {
  uint32_t seq;
  uint32_t ms;
  char     level;
  char     text[192];
};
LogRingEntry      gLogRing[EVD_LOG_RING_LEN];
uint32_t          gLogRingSeq = 0;
portMUX_TYPE      gLogRingMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t  gSerialLogLevel = 3;

// ---- Identity / settings ---------------------------------------------------
String serverName    = "EVDSTR";
String wifiSsid      = "";
String wifiPassword  = "";
bool   useStaticIp   = false;
String staticIp      = "";
String staticGateway = "";
String staticSubnet  = "";
String staticDns     = "";
bool   accessPointActive = false;
uint32_t serverEpoch = 0;

bool          settingsDirty      = false;
unsigned long settingsDirtySince = 0;

SemaphoreHandle_t prefsMutex = nullptr;

// ---- Control-plane pull from EVDCTR ---------------------------------------
// The encoder parameters the players are told about live on EVDCTR. This board
// pulls them every 2 s from http://<CTR_IP>/api/audio/encoding so a dashboard
// change reaches the players without a control channel on the pin link.
String ctrHost = "";
String ctrVersion = "";
uint32_t ctrLastPullMs = 0;
SemaphoreHandle_t ctrPullMutex = nullptr;
