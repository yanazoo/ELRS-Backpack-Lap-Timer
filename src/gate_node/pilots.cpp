#include <Arduino.h>
#include <string.h>
#include "pilots.h"
#include "config.h"

PilotState pilots[MAX_PILOTS];

void initPilots() {
    for (int i = 0; i < MAX_PILOTS; i++) {
        memset(pilots[i].uid, 0, 6);
        pilots[i].hasUid         = false;
        pilots[i].emaRssi        = -120.0f;
        pilots[i].rawRssi        = -120;
        pilots[i].crossing       = false;
        pilots[i].peakRssi       = -120;
        pilots[i].peakTime       = 0;
        pilots[i].lastLapTime    = 0;
        pilots[i].entryThreshold = DEFAULT_ENTRY_THR;
        pilots[i].exitThreshold  = DEFAULT_EXIT_THR;
    }
}

void resetPilots() {
    for (int i = 0; i < MAX_PILOTS; i++) {
        pilots[i].crossing    = false;
        pilots[i].peakRssi    = -120;
        pilots[i].peakTime    = 0;
        pilots[i].lastLapTime = 0;
        pilots[i].emaRssi     = -120.0f;
        pilots[i].rawRssi     = -120;
    }
    Serial.println("[Gate] Pilot state reset");
}

int findPilot(const uint8_t* mac) {
    for (int i = 0; i < MAX_PILOTS; i++) {
        if (pilots[i].hasUid && memcmp(pilots[i].uid, mac, 6) == 0) return i;
    }
    return -1;
}

bool anyPilotRegistered() {
    for (int i = 0; i < MAX_PILOTS; i++) if (pilots[i].hasUid) return true;
    return false;
}

void macToStr(const uint8_t* mac, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Scan: rate-limited forwarding of unknown ESP-NOW MACs ─────────────────────
static struct ScanEntry { uint8_t mac[6]; uint32_t lastSent; } scanTable[MAX_SCAN_MACS];
static int scanCount = 0;

void reportScanMac(const uint8_t* mac, int8_t rssi) {
    uint32_t now = millis();
    int slot = -1;
    for (int k = 0; k < scanCount; k++) {
        if (memcmp(scanTable[k].mac, mac, 6) == 0) { slot = k; break; }
    }
    if (slot < 0) {
        if (scanCount >= MAX_SCAN_MACS) return;
        slot = scanCount++;
        memcpy(scanTable[slot].mac, mac, 6);
        scanTable[slot].lastSent = 0;
    }
    if (now - scanTable[slot].lastSent < SCAN_INTERVAL_MS) return;
    scanTable[slot].lastSent = now;

    char macStr[18];
    macToStr(mac, macStr);
    char buf[96];
    snprintf(buf, sizeof(buf),
             R"({"type":"scan","mac":"%s","rssi":%d,"ts":%lu})",
             macStr, (int)rssi, (unsigned long)now);
    Serial1.println(buf);
    Serial.printf("[Gate] SCAN %s rssi=%d\n", macStr, (int)rssi);
}

void resetScanTimers() {
    for (int k = 0; k < scanCount; k++) scanTable[k].lastSent = 0;
}
