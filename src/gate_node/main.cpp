/*
 * ELRS Backpack Lap Timer — Gate Node
 * Hardware : ESP32-WROVER-E-A
 *
 * Roles
 *   - Promiscuous mode: receive all 2.4 GHz ESP-NOW frames from ELRS TX Backpacks
 *   - EMA filter + RotorHazard-style state machine for gate-crossing detection
 *   - Per-pilot runtime-configurable Enter/Exit RSSI thresholds
 *   - Only processes packets from pre-registered UIDs (set via set_pilot command)
 *
 * UART protocol — Gate→Web (1 JSON line per event):
 *   {"type":"lap",   "pilot":0,"uid":"AA:BB","rssi":-72,"ts":123456}
 *   {"type":"rssi",  "pilot":0,"rssi":-85,"raw":-87,"crossing":false,"ts":123460}
 *   {"type":"ready", "pilots":4}          — sent on boot
 *
 * UART protocol — Web→Gate (sync commands):
 *   {"type":"cmd","action":"race_start"}
 *   {"type":"cmd","action":"set_pilot","pilot":0,"uid":"AA:BB:CC:DD:EE:FF"}
 *   {"type":"cmd","action":"set_threshold","pilot":0,"enter":-80,"exit":-90}
 *
 * Wiring
 *   ESP32-WROVER-E GPIO26 (TX1) → XIAO ESP32-S3 GPIO3  (D2/RX1)
 *   ESP32-WROVER-E GPIO25 (RX1) ← XIAO ESP32-S3 GPIO2  (D1/TX1)
 *   Common GND
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

// ── Pins & UART ────────────────────────────────────────────────────────────
#define WEB_NODE_TX_PIN   26
#define WEB_NODE_RX_PIN   25
#define DEBUG_BAUD        115200
#define UART_BAUD         115200

// ── WiFi channel ───────────────────────────────────────────────────────────
// ELRS TX Backpack initializes WiFi on channel 1 (WiFi.begin(..., 1) in Tx_main.cpp)
// and ESP-NOW peers use channel=0 (follow current channel), so ESP-NOW runs on ch1.
#define ESPNOW_CHANNEL    1

// ── Default detection parameters ──────────────────────────────────────────
#define MAX_PILOTS         4
#define EMA_ALPHA          0.3f
#define DEFAULT_ENTRY_THR  (-80)    // dBm
#define DEFAULT_EXIT_THR   (-90)    // dBm
#define COOLDOWN_MS        3000UL
#define RSSI_INTERVAL_MS   100UL

// ── ISR→main queue ─────────────────────────────────────────────────────────
struct PacketInfo { uint8_t mac[6]; int8_t rssi; };
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

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(packetQueue, &info, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// ── Helpers ────────────────────────────────────────────────────────────────
static void macToStr(const uint8_t* mac, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

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

// ── Handle commands from Web Node ──────────────────────────────────────────
static void processWebCmd(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";
    if (strcmp(type, "cmd") != 0) return;

    const char* action = doc["action"] | "";

    if (strcmp(action, "race_start") == 0) {
        resetPilots();

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
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(DEBUG_BAUD);
    Serial.println("\n[Gate] ELRS Backpack Lap Timer — Gate Node");

    Serial1.begin(UART_BAUD, SERIAL_8N1, WEB_NODE_RX_PIN, WEB_NODE_TX_PIN);

    initPilots();
    packetQueue = xQueueCreate(64, sizeof(PacketInfo));

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
}

// ── Loop ───────────────────────────────────────────────────────────────────
static String   webCmdBuf;
static uint32_t lastRssiSend = 0;

void loop() {
    uint32_t now = millis();

    // 0. Read commands from Web Node
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            webCmdBuf.trim();
            if (webCmdBuf.length()) { processWebCmd(webCmdBuf); webCmdBuf = ""; }
        } else if (c != '\r') {
            webCmdBuf += c;
            if (webCmdBuf.length() > 256) webCmdBuf = "";
        }
    }

    // 1. Drain ISR queue — only update pilots with registered UIDs
    PacketInfo info;
    while (xQueueReceive(packetQueue, &info, 0) == pdTRUE) {
        int idx = findPilot(info.mac);
        if (idx >= 0) pilots[idx].rawRssi = info.rssi;
    }

    // 2. EMA filter + state machine — only for registered pilots
    for (int i = 0; i < MAX_PILOTS; i++) {
        if (!pilots[i].hasUid) continue;
        PilotState& p = pilots[i];
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

    // 3. Periodic RSSI telemetry — only for registered pilots
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        for (int i = 0; i < MAX_PILOTS; i++) {
            if (pilots[i].hasUid) sendRssi(i, now);
        }
    }

    delay(10);
}
