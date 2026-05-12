#pragma once
#include <stdint.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

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

extern PilotRoster roster[MAX_REGISTERED];
extern CalibConfig rosterCal[MAX_REGISTERED];
extern int         rosterCount;
extern int         activePilots[MAX_ACTIVE];
extern Preferences prefs;

extern SlotRuntime rt[MAX_ACTIVE];
extern LapRecord   laps[MAX_LAPS];
extern int         lapCount;
extern bool        raceRunning;
extern uint32_t    raceStartMs;
extern bool        sdPresent;
extern uint8_t     lapMode;
extern uint32_t    gateRaceStartTs;
extern uint32_t    cooldownMs;
extern String      restoreBuffer[MAX_REGISTERED];
extern int         restoreCount;
extern ScanMac     scanMacs[MAX_SCAN_MACS];
extern int         scanMacCount;

extern AsyncWebServer server;
extern AsyncWebSocket ws;
extern DNSServer      dnsServer;

inline void uidToStr(const uint8_t* uid, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
}
