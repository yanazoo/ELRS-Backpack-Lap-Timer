#pragma once
#include <stdint.h>
#include "config.h"

struct PacketInfo {
    uint8_t mac[6];
    int8_t  rssi;
    bool    isEspNow;
};

struct PilotState {
    uint8_t  uid[6];
    bool     hasUid;
    char     name[32];
    float    emaRssi;
    int      rawRssi;
    bool     crossing;
    int      peakRssi;
    uint32_t peakTime;
    uint32_t lastPeakTime;
    uint32_t lastLapTime;
    int      entryThreshold;
    int      exitThreshold;
};

extern PilotState pilots[MAX_PILOTS];

void initPilots();
void resetPilots();
int  findPilot(const uint8_t* mac);
bool anyPilotRegistered();
void macToStr(const uint8_t* mac, char* buf);
void reportScanMac(const uint8_t* mac, int8_t rssi);
void resetScanTimers();
