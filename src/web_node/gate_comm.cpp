#include <Arduino.h>
#include <ArduinoJson.h>
#include "gate_comm.h"
#include "data_model.h"
#include "json_api.h"
#include "nvs_store.h"
#include "ws_handler.h"

SlotRuntime rt[MAX_ACTIVE];
LapRecord   laps[MAX_LAPS];
int         lapCount    = 0;
bool        raceRunning      = false;
uint32_t    raceStartMs      = 0;
bool        sdPresent        = false;
uint8_t     lapMode          = 0;
uint32_t    gateRaceStartTs  = 0;
uint32_t    cooldownMs       = 3000;
String      restoreBuffer[MAX_REGISTERED];
int         restoreCount = 0;
ScanMac     scanMacs[MAX_SCAN_MACS];
int         scanMacCount = 0;

void updateScanMac(const char* mac, int rssi) {
    for (int i = 0; i < scanMacCount; i++) {
        if (strcmp(scanMacs[i].mac, mac) == 0) {
            scanMacs[i].rssi = rssi; scanMacs[i].ts = millis(); return;
        }
    }
    if (scanMacCount < MAX_SCAN_MACS) {
        strncpy(scanMacs[scanMacCount].mac, mac, 17);
        scanMacs[scanMacCount].mac[17] = '\0';
        scanMacs[scanMacCount].rssi    = rssi;
        scanMacs[scanMacCount].ts      = millis();
        scanMacCount++;
    }
}

void sendGateCooldown() {
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"set_cooldown","ms":%lu})", (unsigned long)cooldownMs);
    Serial1.println(buf);
}

void sendGateCmd(const char* action) {
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"%s"})", action);
    Serial1.println(buf);
}

void sendGatePilot(int slot) {
    int ri = activePilots[slot];
    char buf[96];
    if (ri < 0 || ri >= rosterCount || !roster[ri].hasUid) {
        snprintf(buf, sizeof(buf),
                 R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":""})", slot);
        Serial1.println(buf);
    } else {
        char uid[18]; uidToStr(roster[ri].uid, uid);
        snprintf(buf, sizeof(buf),
                 R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":"%s"})", slot, uid);
        Serial1.println(buf);
    }
    delay(30);
}

void sendGateThreshold(int slot) {
    int ri    = activePilots[slot];
    int enter = (ri >= 0 && ri < rosterCount) ? rosterCal[ri].enterRssi : DEFAULT_ENTER;
    int exit_ = (ri >= 0 && ri < rosterCount) ? rosterCal[ri].exitRssi  : DEFAULT_EXIT;
    char buf[96];
    snprintf(buf, sizeof(buf),
             R"({"type":"cmd","action":"set_threshold","pilot":%d,"enter":%d,"exit":%d})",
             slot, enter, exit_);
    Serial1.println(buf);
}

void sendAllPilots() {
    for (int s = 0; s < MAX_ACTIVE; s++) { sendGatePilot(s); }
}

void sendAllThresholds() {
    for (int s = 0; s < MAX_ACTIVE; s++) { sendGateThreshold(s); delay(30); }
}

void processGateLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";

    if (strcmp(type, "ready") == 0) {
        sendAllPilots(); sendAllThresholds(); sendGateCooldown();
        return;
    }
    if (strcmp(type, "sd_status") == 0) {
        sdPresent = doc["present"] | false;
        return;
    }
    if (strcmp(type, "scan") == 0) {
        const char* mac = doc["mac"] | "";
        if (strlen(mac) == 17) updateScanMac(mac, doc["rssi"] | -120);
        wsText(line); return;
    }
    if (strcmp(type, "rssi") == 0) {
        int s = doc["pilot"] | -1;
        if (s < 0 || s >= MAX_ACTIVE) return;
        rt[s].rssi     = doc["rssi"]     | -120;
        rt[s].rawRssi  = doc["raw"]      | -120;
        rt[s].crossing = doc["crossing"] | false;
        rt[s].lastTs   = doc["ts"]       | 0u;
        JsonDocument wd;
        wd["type"]     = "rssi";
        wd["pilot"]    = s;
        wd["name"]     = activeName(s);
        wd["rssi"]     = rt[s].rssi;
        wd["raw"]      = rt[s].rawRssi;
        wd["crossing"] = rt[s].crossing;
        wd["ts"]       = rt[s].lastTs;
        String wm; serializeJson(wd, wm); wsText(wm);
        return;
    }
    if (strcmp(type, "race_start_ack") == 0) {
        gateRaceStartTs = doc["ts"] | 0u;
        if (lapMode == 1) {
            for (int s = 0; s < MAX_ACTIVE; s++) rt[s].lastLapTs = gateRaceStartTs;
        }
        return;
    }
    if (strcmp(type, "lap") == 0) {
        if (!raceRunning) return;
        int s = doc["pilot"] | -1;
        if (s < 0 || s >= MAX_ACTIVE) return;
        int ri = activePilots[s];
        uint32_t ts = doc["ts"] | 0u;

        bool isFirst = (rt[s].lastLapTs == 0);
        uint32_t lapMs = 0;
        if (!isFirst) {
            lapMs = ts - rt[s].lastLapTs;
        } else if (lapMode == 1 && gateRaceStartTs > 0) {
            lapMs = ts - gateRaceStartTs;
        }
        rt[s].lastLapTs = ts;

        if (isFirst && lapMs == 0) {
            JsonDocument wd;
            wd["type"]  = "gate_start";
            wd["pilot"] = s;
            wd["name"]  = activeName(s);
            wd["ts"]    = ts;
            String wm; serializeJson(wd, wm); wsText(wm);
            return;
        }

        rt[s].lapCount++;
        bool newBest = false;
        if (lapMs > 0 && (rt[s].bestLapMs == 0 || lapMs < rt[s].bestLapMs)) {
            rt[s].bestLapMs = lapMs; newBest = true;
        }
        if (lapCount < MAX_LAPS) laps[lapCount++] = { s, ri, lapMs, ts };

        JsonDocument wd;
        wd["type"]     = "lap";
        wd["pilot"]    = s;
        wd["name"]     = activeName(s);
        wd["yomi"]     = (ri >= 0 && ri < rosterCount) ? roster[ri].yomi : "";
        wd["lapTime"]  = lapMs;
        wd["bestLap"]  = rt[s].bestLapMs;
        wd["lapCount"] = rt[s].lapCount;
        wd["newBest"]  = newBest;
        wd["rssi"]     = doc["rssi"] | -120;
        wd["ts"]       = ts;
        String wm; serializeJson(wd, wm); wsText(wm);
        return;
    }
    if (strcmp(type, "sd_pilot_row") == 0) {
        if (restoreCount < MAX_REGISTERED) restoreBuffer[restoreCount++] = line;
        return;
    }
    if (strcmp(type, "sd_file_list") == 0 ||
        strcmp(type, "sd_file_line") == 0 ||
        strcmp(type, "sd_file_done") == 0 ||
        strcmp(type, "sd_delete_result") == 0) {
        wsText(line); return;
    }
    if (strcmp(type, "sd_restore_done") == 0) {
        rosterCount = 0;
        for (int i = 0; i < MAX_ACTIVE; i++) activePilots[i] = -1;
        for (int i = 0; i < restoreCount && i < MAX_REGISTERED; i++) {
            JsonDocument row;
            if (deserializeJson(row, restoreBuffer[i]) != DeserializationError::Ok) continue;
            int id = rosterCount++;
            strncpy(roster[id].name, row["name"] | "", sizeof(roster[id].name)-1);
            strncpy(roster[id].yomi, row["yomi"] | "", sizeof(roster[id].yomi)-1);
            const char* mac = row["mac"] | "";
            roster[id].hasUid = (strlen(mac) == 17);
            if (roster[id].hasUid)
                sscanf(mac, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                       &roster[id].uid[0], &roster[id].uid[1], &roster[id].uid[2],
                       &roster[id].uid[3], &roster[id].uid[4], &roster[id].uid[5]);
            rosterCal[id].enterRssi = row["enter"] | DEFAULT_ENTER;
            rosterCal[id].exitRssi  = row["exit"]  | DEFAULT_EXIT;
            saveRosterPilot(id);
        }
        saveRosterCount();
        saveActive();
        restoreCount = 0;

        JsonDocument wd;
        wd["type"]   = "sd_restore_done";
        wd["pilots"] = serialized(rosterJson());
        String wm; serializeJson(wd, wm); wsText(wm);
        return;
    }
}
