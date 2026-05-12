#pragma once
#include <stdint.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

// ── Data structures ─────────────────────────────────────────────────────────

struct PilotRoster {
    char    name[32];
    char    yomi[32];
    uint8_t uid[6];
    bool    hasUid;
};

struct CalibConfig {
    int enterRssi;
    int exitRssi;
};

struct LapRecord {
    int      slot;
    int      rosterIdx;
    uint32_t lapTimeMs;
    uint32_t timestamp;
};

struct SlotRuntime {
    int      rssi;
    int      rawRssi;
    bool     crossing;
    uint32_t lastTs;
    uint32_t lapCount;
    uint32_t bestLapMs;
    uint32_t lastLapTs;
};

struct ScanMac {
    char     mac[18];
    int      rssi;
    uint32_t ts;
};

// ── Global variable declarations ─────────────────────────────────────────────
// Each variable is *defined* in exactly one .cpp file (noted in comments).

extern PilotRoster roster[MAX_REGISTERED];    // nvs_store.cpp
extern CalibConfig rosterCal[MAX_REGISTERED]; // nvs_store.cpp
extern int         rosterCount;               // nvs_store.cpp
extern int         activePilots[MAX_ACTIVE];  // nvs_store.cpp
extern Preferences prefs;                     // nvs_store.cpp

extern SlotRuntime rt[MAX_ACTIVE];            // gate_comm.cpp
extern LapRecord   laps[MAX_LAPS];            // gate_comm.cpp
extern int         lapCount;                  // gate_comm.cpp
extern bool        raceRunning;               // gate_comm.cpp
extern uint32_t    raceStartMs;               // gate_comm.cpp
extern bool        sdPresent;                 // gate_comm.cpp
extern uint8_t     lapMode;                   // gate_comm.cpp
extern uint32_t    gateRaceStartTs;           // gate_comm.cpp
extern uint32_t    cooldownMs;                // gate_comm.cpp
extern String      restoreBuffer[MAX_REGISTERED]; // gate_comm.cpp
extern int         restoreCount;              // gate_comm.cpp
extern ScanMac     scanMacs[MAX_SCAN_MACS];   // gate_comm.cpp
extern int         scanMacCount;              // gate_comm.cpp

extern AsyncWebServer server;                 // ws_handler.cpp
extern AsyncWebSocket ws;                     // ws_handler.cpp
extern DNSServer      dnsServer;              // ws_handler.cpp

// ── Inline helper ─────────────────────────────────────────────────────────────
inline void uidToStr(const uint8_t* uid, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
}
