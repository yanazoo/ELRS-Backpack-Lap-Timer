/*
 * ELRS Backpack Lap Timer — Gate Node
 * Hardware : ESP32-WROVER-E-A
 *
 * Roles
 *   - Promiscuous mode: receive all 2.4 GHz ESP-NOW frames from ELRS TX Backpacks
 *   - EMA filter + RotorHazard-style state machine for gate-crossing detection
 *   - Per-pilot runtime-configurable Enter/Exit RSSI thresholds
 *   - Only processes packets from pre-registered UIDs (set via set_pilot command)
 *   - SD card logging: race CSV files + pilot backup/restore
 *
 * UART protocol — Gate→Web (1 JSON line per event):
 *   {"type":"lap",    "pilot":0,"uid":"AA:BB","rssi":-72,"ts":123456}
 *   {"type":"rssi",   "pilot":0,"rssi":-85,"raw":-87,"crossing":false,"ts":123460}
 *   {"type":"ready",  "pilots":4}             — sent on boot
 *   {"type":"sd_status","present":true/false} — sent on boot after SD init
 *   {"type":"sd_pilot_row",<pilot fields>}    — sent per line during restore
 *   {"type":"sd_restore_done"}               — sent after all restore rows sent
 *
 * UART protocol — Web→Gate (sync commands):
 *   {"type":"cmd","action":"race_start"}
 *   {"type":"cmd","action":"set_pilot","pilot":0,"uid":"AA:BB:CC:DD:EE:FF"}
 *   {"type":"cmd","action":"set_threshold","pilot":0,"enter":-80,"exit":-90}
 *   {"type":"cmd","action":"sd_begin_backup"}
 *   {"type":"cmd","action":"sd_backup_row","name":"...","yomi":"...","mac":"...","enter":-80,"exit":-90}
 *   {"type":"cmd","action":"sd_end_backup"}
 *   {"type":"cmd","action":"sd_restore_request"}
 *
 * Wiring
 *   ESP32-WROVER-E GPIO26 (TX1) → XIAO ESP32-S3 GPIO3  (D2/RX1)
 *   ESP32-WROVER-E GPIO25 (RX1) ← XIAO ESP32-S3 GPIO2  (D1/TX1)
 *   SD card (SPI mode): CS=5, MOSI=23, MISO=19, SCK=18
 *   Common GND
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

// ── Pins & UART ────────────────────────────────────────────────────────────
#define WEB_NODE_TX_PIN   26
#define WEB_NODE_RX_PIN   25
#define DEBUG_BAUD        115200
#define UART_BAUD         115200

// ── SD SPI Pins — LilyGo TTGO T8 V1.8 ────────────────────────────────────
#define SD_CS_PIN    13
#define SD_MOSI_PIN  15
#define SD_MISO_PIN   2
#define SD_SCK_PIN   14

// ── WiFi channel ───────────────────────────────────────────────────────────
// Aircraft node (XIAO ESP32-C3) broadcasts ESP-NOW on ch1.
// Gate node listens on ch1 via promiscuous mode.
#define ESPNOW_CHANNEL    1

// ── Default detection parameters ──────────────────────────────────────────
#define MAX_PILOTS         4
// EMA alpha: applied every loop iteration using most-recent rawRssi.
// 0.3 gives smooth convergence; holds last-received value between packets.
#define EMA_ALPHA          0.3f
#define DEFAULT_ENTRY_THR  (-80)    // dBm
#define DEFAULT_EXIT_THR   (-90)    // dBm
#define COOLDOWN_MS        3000UL
// 50ms interval → 20Hz telemetry stream to web node
#define RSSI_INTERVAL_MS   50UL

// ── ISR→main queue ─────────────────────────────────────────────────────────
struct PacketInfo { uint8_t mac[6]; int8_t rssi; bool isEspNow; };
static QueueHandle_t packetQueue;

// ── Pilot state ────────────────────────────────────────────────────────────
struct PilotState {
    uint8_t  uid[6];
    bool     hasUid;          // true only after set_pilot command from Web Node
    float    emaRssi;
    int      rawRssi;
    bool     crossing;
    int      peakRssi;
    uint32_t peakTime;
    uint32_t lastLapTime;
    int      entryThreshold;
    int      exitThreshold;
};

static PilotState pilots[MAX_PILOTS];

static void initPilots() {
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

// ── Pilot lookup — only matches pre-registered UIDs ────────────────────────
static int findPilot(const uint8_t* mac) {
    for (int i = 0; i < MAX_PILOTS; i++) {
        if (pilots[i].hasUid && memcmp(pilots[i].uid, mac, 6) == 0) return i;
    }
    return -1;   // unknown MAC → ignored
}

// ── Promiscuous callback (ISR) ─────────────────────────────────────────────
static void IRAM_ATTR onPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const auto* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->payload[0] != 0xD0) return;   // Action frame only

    PacketInfo info;
    memcpy(info.mac, &pkt->payload[10], 6);
    info.rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    // Tag ESP-NOW frames (Vendor Specific 0x7F + Espressif OUI 18:FE:34 + type 0x04)
    // without hard-blocking non-tagged frames — registered pilots are always processed.
    info.isEspNow = (pkt->rx_ctrl.sig_len >= 29 &&
                     pkt->payload[24] == 0x7F &&
                     pkt->payload[25] == 0x18 && pkt->payload[26] == 0xFE &&
                     pkt->payload[27] == 0x34 && pkt->payload[28] == 0x04);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(packetQueue, &info, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// ── Helpers ────────────────────────────────────────────────────────────────
static void macToStr(const uint8_t* mac, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Scan report — unknown MACs forwarded to Web Node (rate-limited) ─────────
#define MAX_SCAN_MACS   8
#define SCAN_INTERVAL_MS 5000UL

struct ScanEntry { uint8_t mac[6]; uint32_t lastSent; };
static ScanEntry scanTable[MAX_SCAN_MACS];
static int       scanCount = 0;

static void reportScanMac(const uint8_t* mac, int8_t rssi) {
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

// ── SD card state ──────────────────────────────────────────────────────────
static bool     sdPresent    = false;
static File     raceFile;            // open during a race for CSV append
static int      raceFileNum  = 0;    // auto-incrementing race file number
static File     backupFile;          // open during pilot backup write

// ── SD: find next race file number (race_001.csv, race_002.csv, …) ─────────
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

// ── SD: open a new race CSV file and write the header row ──────────────────
static void sdBeginRace() {
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

// ── SD: append a lap row to the open race CSV ─────────────────────────────
static void sdWriteLap(int slotIdx) {
    if (!sdPresent || !raceFile) return;
    char macStr[18];
    macToStr(pilots[slotIdx].uid, macStr);
    raceFile.printf("%d,%s,%d,%lu\n",
                    slotIdx, macStr,
                    pilots[slotIdx].peakRssi,
                    (unsigned long)pilots[slotIdx].peakTime);
    raceFile.flush();
}

// ── SD: close race CSV on stop / new race ─────────────────────────────────
static void sdEndRace() {
    if (raceFile) { raceFile.close(); Serial.println("[Gate] SD race file closed"); }
}

// ── UART helpers ───────────────────────────────────────────────────────────
static void sendLap(int idx) {
    char macStr[18];
    macToStr(pilots[idx].uid, macStr);
    JsonDocument doc;
    doc["type"]  = "lap";
    doc["pilot"] = idx;
    doc["uid"]   = macStr;
    doc["rssi"]  = pilots[idx].peakRssi;
    doc["ts"]    = pilots[idx].peakTime;
    serializeJson(doc, Serial1);
    Serial1.print('\n');
    Serial.printf("[Gate] LAP  pilot=%d  rssi=%d\n", idx, pilots[idx].peakRssi);

    // Also append to the open race CSV on SD
    sdWriteLap(idx);
}

static void sendRssi(int idx, uint32_t now) {
    char macStr[18];
    macToStr(pilots[idx].uid, macStr);
    JsonDocument doc;
    doc["type"]     = "rssi";
    doc["pilot"]    = idx;
    doc["uid"]      = macStr;
    doc["rssi"]     = (int)pilots[idx].emaRssi;
    doc["raw"]      = pilots[idx].rawRssi;
    doc["crossing"] = pilots[idx].crossing;
    doc["ts"]       = now;
    serializeJson(doc, Serial1);
    Serial1.print('\n');
}

// ── Reset pilot detection state (UIDs are kept) ────────────────────────────
static void resetPilots() {
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

// ── SD: restore — read /pilots.json and send each line as sd_pilot_row ─────
static void sdHandleRestore() {
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
        // Wrap in a sd_pilot_row envelope and forward to web node
        // Re-parse and re-emit so we can set the type field correctly
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

// ── Handle commands from Web Node ──────────────────────────────────────────
static void processWebCmd(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";
    if (strcmp(type, "cmd") != 0) return;

    const char* action = doc["action"] | "";

    if (strcmp(action, "race_start") == 0) {
        sdEndRace();   // close any previously open race file
        resetPilots();
        sdBeginRace(); // open new race file on SD

    } else if (strcmp(action, "set_pilot") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        const char* uidStr = doc["uid"] | "";
        if (strlen(uidStr) == 17) {
            sscanf(uidStr, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                   &pilots[idx].uid[0], &pilots[idx].uid[1], &pilots[idx].uid[2],
                   &pilots[idx].uid[3], &pilots[idx].uid[4], &pilots[idx].uid[5]);
            pilots[idx].hasUid = true;
            Serial.printf("[Gate] Pilot #%d registered  %s\n", idx, uidStr);
        } else {
            pilots[idx].hasUid = false;
            Serial.printf("[Gate] Pilot #%d cleared\n", idx);
        }

    } else if (strcmp(action, "set_threshold") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        pilots[idx].entryThreshold = doc["enter"] | DEFAULT_ENTRY_THR;
        pilots[idx].exitThreshold  = doc["exit"]  | DEFAULT_EXIT_THR;
        Serial.printf("[Gate] Threshold p%d: enter=%d exit=%d\n",
                      idx, pilots[idx].entryThreshold, pilots[idx].exitThreshold);

    } else if (strcmp(action, "sd_begin_backup") == 0) {
        // Open (overwrite) /pilots.json for writing
        if (!sdPresent) { Serial.println("[Gate] SD backup: no SD"); return; }
        backupFile = SD.open("/pilots.json", FILE_WRITE);
        if (backupFile) Serial.println("[Gate] SD backup: /pilots.json opened");
        else            Serial.println("[Gate] SD backup: open failed");

    } else if (strcmp(action, "sd_backup_row") == 0) {
        // Append one pilot JSON line to /pilots.json
        if (!backupFile) return;
        // Build a clean object with only the pilot fields
        JsonDocument row;
        row["name"]  = doc["name"]  | "";
        row["yomi"]  = doc["yomi"]  | "";
        row["mac"]   = doc["mac"]   | "";
        row["enter"] = doc["enter"] | DEFAULT_ENTRY_THR;
        row["exit"]  = doc["exit"]  | DEFAULT_EXIT_THR;
        serializeJson(row, backupFile);
        backupFile.println();
        backupFile.flush();

    } else if (strcmp(action, "sd_end_backup") == 0) {
        if (backupFile) { backupFile.close(); Serial.println("[Gate] SD backup: done"); }

    } else if (strcmp(action, "sd_restore_request") == 0) {
        sdHandleRestore();

    } else if (strcmp(action, "sd_list_files") == 0) {
        if (!sdPresent) {
            Serial1.println(R"({"type":"sd_file_list","files":[]})");
            return;
        }
        File root = SD.open("/");
        if (!root) { Serial1.println(R"({"type":"sd_file_list","files":[]})"); return; }
        JsonDocument listDoc;
        listDoc["type"] = "sd_file_list";
        JsonArray arr = listDoc["files"].to<JsonArray>();
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
        serializeJson(listDoc, Serial1);
        Serial1.print('\n');
        Serial.println("[Gate] SD file list sent");

    } else if (strcmp(action, "sd_read_file") == 0) {
        const char* path = doc["path"] | "";
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

    } else if (strcmp(action, "sd_delete_file") == 0) {
        const char* path = doc["path"] | "";
        bool ok = sdPresent && strlen(path) && SD.remove(path);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 R"({"type":"sd_delete_result","path":"%s","ok":%s})",
                 path, ok ? "true" : "false");
        Serial1.println(buf);
        Serial.printf("[Gate] SD delete %s: %s\n", path, ok ? "ok" : "fail");
    }
}

// ── Loop state — declared before setup() so setup() can write lastReadySend
static String   webCmdBuf;
static uint32_t lastRssiSend  = 0;
static uint32_t lastReadySend = 0;

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(DEBUG_BAUD);
    Serial.println("\n[Gate] ELRS Backpack Lap Timer — Gate Node");

    Serial1.begin(UART_BAUD, SERIAL_8N1, WEB_NODE_RX_PIN, WEB_NODE_TX_PIN);

    initPilots();
    packetQueue = xQueueCreate(64, sizeof(PacketInfo));

    // ── SD card initialisation (SPI mode) ─────────────────────────────────
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN)) {
        sdPresent = true;
        Serial.println("[Gate] SD card OK");
    } else {
        sdPresent = false;
        Serial.println("[Gate] SD card not found");
    }
    // Inform web node of SD status immediately after Serial1 is ready
    char sdBuf[48];
    snprintf(sdBuf, sizeof(sdBuf), R"({"type":"sd_status","present":%s})",
             sdPresent ? "true" : "false");
    Serial1.println(sdBuf);

    // ── WiFi promiscuous setup ─────────────────────────────────────────────
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);

    Serial.printf("[Gate] Listening on WiFi channel %d\n", ESPNOW_CHANNEL);

    // Notify web node — it will respond with set_pilot + set_threshold for all pilots
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
    Serial1.println(buf);
    lastReadySend = millis();   // prevent 10s re-send from firing immediately
}

static bool anyPilotRegistered() {
    for (int i = 0; i < MAX_PILOTS; i++) if (pilots[i].hasUid) return true;
    return false;
}

void loop() {
    uint32_t now = millis();

    // 0. Re-send "ready" every 10 s until Web Node registers at least one pilot
    if (!anyPilotRegistered() && now - lastReadySend >= 10000UL) {
        lastReadySend = now;
        char buf[64];
        snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
        Serial1.println(buf);
        Serial.println("[Gate] Re-sent ready (no pilots registered yet)");
    }

    // 1. Read commands from Web Node
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            webCmdBuf.trim();
            if (webCmdBuf.length()) { processWebCmd(webCmdBuf); webCmdBuf = ""; }
        } else if (c != '\r') {
            webCmdBuf += c;
            if (webCmdBuf.length() > 512) webCmdBuf = "";
        }
    }

    // 2. Drain ISR queue — registered pilots update rawRssi; unknown ESP-NOW MACs go to scan
    PacketInfo info;
    while (xQueueReceive(packetQueue, &info, 0) == pdTRUE) {
        int idx = findPilot(info.mac);
        if (idx >= 0) {
            pilots[idx].rawRssi = info.rssi;
        } else if (info.isEspNow) {
            // Only forward genuine ESP-NOW frames to scan list
            reportScanMac(info.mac, info.rssi);
        }
    }

    // 3. EMA filter + state machine — only for registered pilots
    for (int i = 0; i < MAX_PILOTS; i++) {
        if (!pilots[i].hasUid) continue;
        PilotState& p = pilots[i];
        // Simple EMA every loop iteration. rawRssi holds the last received packet value,
        // so EMA naturally converges to and holds that value between packets.
        p.emaRssi = EMA_ALPHA * p.rawRssi + (1.0f - EMA_ALPHA) * p.emaRssi;
        float ema = p.emaRssi;

        if (!p.crossing) {
            if (ema > p.entryThreshold) {
                p.crossing = true;
                p.peakRssi = (int)ema;
                p.peakTime = now;
            }
        } else {
            if ((int)ema > p.peakRssi) { p.peakRssi = (int)ema; p.peakTime = now; }
            if (ema < p.exitThreshold) {
                if (now - p.lastLapTime >= COOLDOWN_MS) {
                    p.lastLapTime = now;
                    sendLap(i);
                }
                p.crossing = false;
            }
        }
    }

    // 4. Periodic RSSI telemetry — only for registered pilots
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        for (int i = 0; i < MAX_PILOTS; i++) {
            if (pilots[i].hasUid) sendRssi(i, now);
        }
    }

    delay(10);
}
