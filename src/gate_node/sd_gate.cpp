#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include "sd_gate.h"
#include "pilots.h"
#include "config.h"

bool sdPresent = false;

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
    char buf[48];
    snprintf(buf, sizeof(buf), R"({"type":"sd_status","present":%s})",
             sdPresent ? "true" : "false");
    Serial1.println(buf);
}

void sdBeginRace() {
    if (!sdPresent) return;
    raceFileNum = findNextRaceNum();
    char path[32];
    snprintf(path, sizeof(path), "/race_%03d.csv", raceFileNum);
    raceFile = SD.open(path, FILE_WRITE);
    if (raceFile) {
        raceFile.println("Slot,UID,RSSI_dBm,Timestamp_ms");
        raceFile.flush();
        Serial.printf("[Gate] SD race file opened: %s\n", path);
    } else {
        Serial.printf("[Gate] SD race file open FAILED: %s\n", path);
    }
}

void sdWriteLap(int slotIdx) {
    if (!sdPresent || !raceFile) return;
    char macStr[18];
    macToStr(pilots[slotIdx].uid, macStr);
    raceFile.printf("%d,%s,%d,%lu\n",
                    slotIdx, macStr,
                    pilots[slotIdx].peakRssi,
                    (unsigned long)pilots[slotIdx].peakTime);
    raceFile.flush();
}

void sdEndRace() {
    if (raceFile) { raceFile.close(); Serial.println("[Gate] SD race file closed"); }
}

void sdBeginBackup() {
    if (!sdPresent) { Serial.println("[Gate] SD backup: no SD"); return; }
    backupFile = SD.open("/pilots.json", FILE_WRITE);
    if (backupFile) Serial.println("[Gate] SD backup: /pilots.json opened");
    else            Serial.println("[Gate] SD backup: open failed");
}

void sdWriteBackupRow(const char* name, const char* yomi,
                      const char* mac, int enter, int exit_) {
    if (!backupFile) return;
    JsonDocument row;
    row["name"]  = name;
    row["yomi"]  = yomi;
    row["mac"]   = mac;
    row["enter"] = enter;
    row["exit"]  = exit_;
    serializeJson(row, backupFile);
    backupFile.println();
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
    File f = SD.open("/pilots.json", FILE_READ);
    if (!f) {
        Serial.println("[Gate] SD restore: /pilots.json not found");
        Serial1.println(R"({"type":"sd_restore_done"})");
        return;
    }
    Serial.println("[Gate] SD restore: reading /pilots.json");
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        JsonDocument row;
        if (deserializeJson(row, line) == DeserializationError::Ok) {
            row["type"] = "sd_pilot_row";
            serializeJson(row, Serial1);
            Serial1.print('\n');
        }
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
