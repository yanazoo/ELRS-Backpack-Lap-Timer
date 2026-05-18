#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include "sd_gate.h"
#include "pilots.h"
#include "config.h"

bool    sdPresent     = false;
bool    sdPollEnabled = false;
uint8_t sdLogMode     = 0;   // 0=always, 1=rotate, 2=off

static File raceFile;
static int  raceFileNum = 0;
static File backupFile;

static int findNextRaceNum() {
    int n = 1;
    char path[32];
    while (n < 1000) {
        snprintf(path, sizeof(path), "/race_%03d.csv", n);
        if (!SD.exists(path)) break;
        n++;
    }
    return n;
}

void sdInit() {
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    delay(10);
    if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
        sdPresent = true;
        Serial.printf("[Gate] SD card OK  size=%lluMB\n",
                      SD.cardSize() / (1024ULL * 1024ULL));
    } else {
        sdPresent = false;
        Serial.println("[Gate] SD card not found");
    }
    sdSendStatus();
}

void sdCheckHotplug(uint32_t now) {
    static uint32_t lastInsertCheck = 0;
    static uint32_t lastRemoveCheck = 0;

    if (!sdPresent) {
        if (now - lastInsertCheck < 500) return;
        lastInsertCheck = now;
        SD.end();
        if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
            sdPresent = true;
            Serial.printf("[Gate] SD inserted  size=%lluMB\n",
                          SD.cardSize() / (1024ULL * 1024ULL));
            sdSendStatus();
        }
    } else if (!raceFile && !backupFile) {
        if (now - lastRemoveCheck < 3000) return;
        lastRemoveCheck = now;
        SD.end();
        if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
            sdPresent = false;
            Serial.println("[Gate] SD removed");
            sdSendStatus();
        }
    }
}

void sdSendStatus() {
    char buf[48];
    snprintf(buf, sizeof(buf), R"({"type":"sd_status","present":%s})",
             sdPresent ? "true" : "false");
    Serial1.println(buf);
}

// rotate mode: delete oldest race_*.csv until fewer than keep remain,
// leaving room for the one race file we are about to create.
static void sdPruneRaceFiles(int keep) {
    char path[32];
    for (;;) {
        int count = 0, minN = 1000;
        for (int n = 1; n < 1000; n++) {
            snprintf(path, sizeof(path), "/race_%03d.csv", n);
            if (SD.exists(path)) { count++; if (n < minN) minN = n; }
        }
        if (count < keep || minN == 1000) break;
        snprintf(path, sizeof(path), "/race_%03d.csv", minN);
        SD.remove(path);
        Serial.printf("[Gate] SD rotate: removed %s\n", path);
    }
}

void sdBeginRace() {
    if (!sdPresent || sdLogMode == 2) return;   // 2 = off (no race logging)
    if (sdLogMode == 1) sdPruneRaceFiles(SD_MAX_RACE_FILES);
    raceFileNum = findNextRaceNum();
    char path[32];
    snprintf(path, sizeof(path), "/race_%03d.csv", raceFileNum);
    raceFile = SD.open(path, FILE_WRITE);
    if (raceFile) {
        raceFile.print("\xEF\xBB\xBF");  // UTF-8 BOM for Excel compatibility
        raceFile.println("Slot,Name,UID,Lap,LapTime_ms,RSSI_dBm,Timestamp_ms");
        raceFile.flush();
        Serial.printf("[Gate] SD race file opened: %s\n", path);
    } else {
        Serial.printf("[Gate] SD race file open FAILED: %s\n", path);
    }
}

// One race result row, written from web-provided values at "clear" time.
// No per-row flush; sdEndRace() close() finalizes and persists the file.
void sdWriteRaceRow(int slot, const char* name, const char* uid,
                    int lap, uint32_t lapMs, int rssi, uint32_t ts) {
    if (!sdPresent || !raceFile) return;
    raceFile.printf("%d,%s,%s,%d,%lu,%d,%lu\n",
                    slot,
                    (name && name[0]) ? name : (uid ? uid : ""),
                    uid ? uid : "",
                    lap,
                    (unsigned long)lapMs,
                    rssi,
                    (unsigned long)ts);
}

void sdEndRace() {
    if (raceFile) { raceFile.close(); Serial.println("[Gate] SD race file closed"); }
}

void sdBeginBackup() {
    if (!sdPresent) { Serial.println("[Gate] SD backup: no SD"); return; }
    backupFile = SD.open("/pilots.csv", FILE_WRITE);
    if (backupFile) {
        backupFile.print("\xEF\xBB\xBF");  // UTF-8 BOM for Excel compatibility
        backupFile.println("name,yomi,mac,enter,exit,slot");
        backupFile.flush();
        Serial.println("[Gate] SD backup: /pilots.csv opened");
    } else {
        Serial.println("[Gate] SD backup: open failed");
    }
}

void sdWriteBackupRow(const char* name, const char* yomi,
                      const char* mac, int enter, int exit_, int slot) {
    if (!backupFile) return;
    backupFile.printf("%s,%s,%s,%d,%d,%d\n", name, yomi, mac, enter, exit_, slot);
    backupFile.flush();
}

void sdEndBackup() {
    if (backupFile) { backupFile.close(); Serial.println("[Gate] SD backup: done"); }
}

void sdHandleRestore() {
    if (!sdPresent) {
        Serial1.println(R"({"type":"sd_restore_done"})");
        return;
    }
    File f = SD.open("/pilots.csv", FILE_READ);
    if (!f) {
        Serial.println("[Gate] SD restore: /pilots.csv not found");
        Serial1.println(R"({"type":"sd_restore_done"})");
        return;
    }
    Serial.println("[Gate] SD restore: reading /pilots.csv");
    bool firstLine = true;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        if (firstLine) { firstLine = false; continue; }  // skip header
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) continue;
        int c5 = line.indexOf(',', c4 + 1);  // slot column (optional, new format)
        JsonDocument row;
        row["type"]  = "sd_pilot_row";
        row["name"]  = line.substring(0, c1);
        row["yomi"]  = line.substring(c1 + 1, c2);
        row["mac"]   = line.substring(c2 + 1, c3);
        row["enter"] = line.substring(c3 + 1, c4).toInt();
        row["exit"]  = (c5 >= 0) ? line.substring(c4 + 1, c5).toInt()
                                  : line.substring(c4 + 1).toInt();
        row["slot"]  = (c5 >= 0) ? line.substring(c5 + 1).toInt() : -1;
        serializeJson(row, Serial1);
        Serial1.print('\n');
    }
    f.close();
    Serial1.println(R"({"type":"sd_restore_done"})");
    Serial.println("[Gate] SD restore done");
}

void sdListFiles() {
    if (!sdPresent) {
        Serial1.println(R"({"type":"sd_file_list","files":[]})");
        return;
    }
    File root = SD.open("/");
    if (!root) { Serial1.println(R"({"type":"sd_file_list","files":[]})"); return; }
    JsonDocument doc;
    doc["type"] = "sd_file_list";
    JsonArray arr = doc["files"].to<JsonArray>();
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            JsonObject f = arr.add<JsonObject>();
            f["name"] = String(entry.name());
            f["size"] = (uint32_t)entry.size();
        }
        entry.close();
    }
    root.close();
    serializeJson(doc, Serial1);
    Serial1.print('\n');
    Serial.println("[Gate] SD file list sent");
}

void sdReadFile(const char* path) {
    char doneBuf[96];
    snprintf(doneBuf, sizeof(doneBuf), R"({"type":"sd_file_done","path":"%s"})", path);
    if (!sdPresent || !strlen(path)) { Serial1.println(doneBuf); return; }
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[Gate] SD read: %s not found\n", path);
        Serial1.println(doneBuf);
        return;
    }
    while (f.available()) {
        String csvLine = f.readStringUntil('\n');
        csvLine.trim();
        if (!csvLine.length()) continue;
        JsonDocument lineDoc;
        lineDoc["type"] = "sd_file_line";
        lineDoc["path"] = path;
        lineDoc["line"] = csvLine;
        serializeJson(lineDoc, Serial1);
        Serial1.print('\n');
    }
    f.close();
    Serial1.println(doneBuf);
    Serial.printf("[Gate] SD read done: %s\n", path);
}

void sdDeleteFile(const char* path) {
    bool ok = sdPresent && strlen(path) && SD.remove(path);
    char buf[96];
    snprintf(buf, sizeof(buf),
             R"({"type":"sd_delete_result","path":"%s","ok":%s})",
             path, ok ? "true" : "false");
    Serial1.println(buf);
    Serial.printf("[Gate] SD delete %s: %s\n", path, ok ? "ok" : "fail");
}
