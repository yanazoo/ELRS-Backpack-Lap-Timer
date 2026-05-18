#include <Arduino.h>
#include <ArduinoJson.h>
#include "uart_gate.h"
#include "pilots.h"
#include "sd_gate.h"
#include "config.h"

uint32_t gCooldownMs = COOLDOWN_MS;

void sendLap(int idx, uint32_t lapMs) {
    char macStr[18];
    macToStr(pilots[idx].uid, macStr);
    JsonDocument doc;
    doc["type"]   = "lap";
    doc["pilot"]  = idx;
    doc["uid"]    = macStr;
    doc["rssi"]   = pilots[idx].peakRssi;
    doc["ts"]     = pilots[idx].peakTime;
    doc["lapMs"]  = lapMs;
    serializeJson(doc, Serial1);
    Serial1.print('\n');
    pilots[idx].lapCount++;
    Serial.printf("[Gate] LAP  pilot=%d  lap=%d  rssi=%d  lapMs=%lu\n",
                  idx, pilots[idx].lapCount, pilots[idx].peakRssi, (unsigned long)lapMs);
}

void sendRssi(int idx, uint32_t now) {
    char macStr[18];
    macToStr(pilots[idx].uid, macStr);
    bool hasSignal = pilots[idx].lastPacketTime > 0 &&
                     (now - pilots[idx].lastPacketTime) < SIGNAL_LOST_MS;
    JsonDocument doc;
    doc["type"]     = "rssi";
    doc["pilot"]    = idx;
    doc["uid"]      = macStr;
    doc["rssi"]     = (int)pilots[idx].emaRssi;
    doc["raw"]      = pilots[idx].rawRssi;
    doc["crossing"] = pilots[idx].crossing;
    doc["signal"]   = hasSignal;
    doc["ts"]       = now;
    serializeJson(doc, Serial1);
    Serial1.print('\n');
}

void processWebCmd(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";
    if (strcmp(type, "cmd") != 0) return;

    const char* action = doc["action"] | "";

    if (strcmp(action, "race_start") == 0) {
        resetPilots();
        char ackBuf[48];
        snprintf(ackBuf, sizeof(ackBuf), R"({"type":"race_start_ack","ts":%lu})", (unsigned long)millis());
        Serial1.println(ackBuf);

    } else if (strcmp(action, "sd_race_save_begin") == 0) {
        sdBeginRace();
    } else if (strcmp(action, "sd_race_save_row") == 0) {
        sdWriteRaceRow(doc["slot"]  | -1,
                       doc["name"]  | "",
                       doc["uid"]   | "",
                       doc["lap"]   | 0,
                       (uint32_t)(doc["lapMs"] | 0u),
                       doc["rssi"]  | -120,
                       (uint32_t)(doc["ts"] | 0u));
    } else if (strcmp(action, "sd_race_save_end") == 0) {
        sdEndRace();

    } else if (strcmp(action, "set_pilot") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        const char* uidStr  = doc["uid"]  | "";
        const char* nameStr = doc["name"] | "";
        if (strlen(uidStr) == 17) {
            sscanf(uidStr, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                   &pilots[idx].uid[0], &pilots[idx].uid[1], &pilots[idx].uid[2],
                   &pilots[idx].uid[3], &pilots[idx].uid[4], &pilots[idx].uid[5]);
            pilots[idx].hasUid = true;
            strncpy(pilots[idx].name, nameStr, sizeof(pilots[idx].name) - 1);
            pilots[idx].name[sizeof(pilots[idx].name) - 1] = '\0';
            Serial.printf("[Gate] Pilot #%d registered  %s  \"%s\"\n", idx, uidStr, pilots[idx].name);
        } else {
            pilots[idx].hasUid  = false;
            pilots[idx].name[0] = '\0';
            Serial.printf("[Gate] Pilot #%d cleared\n", idx);
        }
        sdSendStatus();

    } else if (strcmp(action, "set_cooldown") == 0) {
        gCooldownMs = (uint32_t)(doc["ms"] | (int)COOLDOWN_MS);
        Serial.printf("[Gate] Cooldown set to %lu ms\n", (unsigned long)gCooldownMs);

    } else if (strcmp(action, "set_sd_log_mode") == 0) {
        int m = doc["mode"] | 0;
        if (m < 0 || m > 2) m = 0;
        sdLogMode = (uint8_t)m;
        Serial.printf("[Gate] SD log mode = %d\n", sdLogMode);

    } else if (strcmp(action, "scan_refresh") == 0) {
        resetScanTimers();
        Serial.println("[Gate] Scan timers reset");

    } else if (strcmp(action, "set_threshold") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        pilots[idx].entryThreshold = doc["enter"] | DEFAULT_ENTRY_THR;
        pilots[idx].exitThreshold  = doc["exit"]  | DEFAULT_EXIT_THR;
        Serial.printf("[Gate] Threshold p%d: enter=%d exit=%d\n",
                      idx, pilots[idx].entryThreshold, pilots[idx].exitThreshold);

    } else if (strcmp(action, "sd_begin_backup") == 0) {
        sdBeginBackup();
    } else if (strcmp(action, "sd_backup_row") == 0) {
        sdWriteBackupRow(
            doc["name"]  | "",
            doc["yomi"]  | "",
            doc["mac"]   | "",
            doc["enter"] | DEFAULT_ENTRY_THR,
            doc["exit"]  | DEFAULT_EXIT_THR,
            doc["slot"]  | -1
        );
    } else if (strcmp(action, "sd_end_backup") == 0) {
        sdEndBackup();
    } else if (strcmp(action, "sd_restore_request") == 0) {
        sdHandleRestore();
    } else if (strcmp(action, "sd_list_files") == 0) {
        sdListFiles();
    } else if (strcmp(action, "sd_read_file") == 0) {
        sdReadFile(doc["path"] | "");
    } else if (strcmp(action, "sd_delete_file") == 0) {
        sdDeleteFile(doc["path"] | "");
    } else if (strcmp(action, "sd_poll") == 0) {
        sdPollEnabled = doc["enable"] | false;
        if (sdPollEnabled) sdSendStatus();
        Serial.printf("[Gate] SD poll %s\n", sdPollEnabled ? "on" : "off");
    }
}
