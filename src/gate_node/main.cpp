/*
 * ELRS Backpack Lap Timer — Gate Node
 * Hardware : ESP32-WROVER-E-A
 *
 * Roles
 *   - Promiscuous mode: receive all 2.4 GHz ESP-NOW frames from ELRS TX Backpacks
 *   - EMA filter + RotorHazard-style state machine for gate-crossing detection
 *   - Per-pilot runtime-configurable Enter/Exit RSSI thresholds
 *
 * UART protocol — Gate→Web (1 JSON line per event):
 *   {"type":"lap",   "pilot":0,"uid":"AA:BB","rssi":-72,"ts":123456}
 *   {"type":"rssi",  "pilot":0,"rssi":-85,"raw":-87,"crossing":false,"ts":123460}
 *   {"type":"ready", "pilots":4}          — sent on boot
 *
 * UART protocol — Web→Gate (sync commands):
 *   {"type":"cmd","action":"race_start"}
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
    bool     active;
    float    emaRssi;
    int      rawRssi;
    bool     crossing;
    int      peakRssi;
    uint32_t peakTime;
    uint32_t lastLapTime;
    int      entryThreshold;   // runtime-configurable per pilot
    int      exitThreshold;
};

static PilotState pilots[MAX_PILOTS];
static int        pilotCount = 0;

// ── Pilot registry ─────────────────────────────────────────────────────────
static int findOrRegister(const uint8_t* mac) {
    for (int i = 0; i < pilotCount; i++) {
        if (memcmp(pilots[i].uid, mac, 6) == 0) return i;
    }
    if (pilotCount >= MAX_PILOTS) return -1;

    int idx = pilotCount++;
    PilotState& p = pilots[idx];
    memcpy(p.uid, mac, 6);
    p.active          = true;
    p.emaRssi         = -120.0f;
    p.rawRssi         = -120;
    p.crossing        = false;
    p.peakRssi        = -120;
    p.peakTime        = 0;
    p.lastLapTime     = 0;
    p.entryThreshold  = DEFAULT_ENTRY_THR;
    p.exitThreshold   = DEFAULT_EXIT_THR;
    Serial.printf("[Gate] New pilot #%d  %02X:%02X:%02X:%02X:%02X:%02X\n",
        idx, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return idx;
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

// ── Reset pilot detection state ────────────────────────────────────────────
static void resetPilots() {
    for (int i = 0; i < pilotCount; i++) {
        pilots[i].crossing    = false;
        pilots[i].peakRssi    = -120;
        pilots[i].peakTime    = 0;
        pilots[i].lastLapTime = 0;
        pilots[i].emaRssi     = -120.0f;
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
    } else if (strcmp(action, "set_threshold") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx >= 0 && idx < MAX_PILOTS) {
            int enter = doc["enter"] | DEFAULT_ENTRY_THR;
            int exit_ = doc["exit"]  | DEFAULT_EXIT_THR;
            // Apply to existing pilot or store as default for when they register
            if (idx < pilotCount) {
                pilots[idx].entryThreshold = enter;
                pilots[idx].exitThreshold  = exit_;
                Serial.printf("[Gate] Threshold p%d: enter=%d exit=%d\n", idx, enter, exit_);
            }
            // Also store as "pending" in a separate array for not-yet-seen pilots
            // (simple approach: store for idx slot regardless of pilotCount)
            static int pendingEnter[MAX_PILOTS] = {DEFAULT_ENTRY_THR,DEFAULT_ENTRY_THR,DEFAULT_ENTRY_THR,DEFAULT_ENTRY_THR};
            static int pendingExit [MAX_PILOTS] = {DEFAULT_EXIT_THR, DEFAULT_EXIT_THR, DEFAULT_EXIT_THR, DEFAULT_EXIT_THR};
            pendingEnter[idx] = enter;
            pendingExit [idx] = exit_;
        }
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(DEBUG_BAUD);
    Serial.println("\n[Gate] ELRS Backpack Lap Timer — Gate Node");

    Serial1.begin(UART_BAUD, SERIAL_8N1, WEB_NODE_RX_PIN, WEB_NODE_TX_PIN);

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

    // Notify web node
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
    Serial1.println(buf);
}

// ── Loop ───────────────────────────────────────────────────────────────────
static String  webCmdBuf;
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

    // 1. Drain ISR queue
    PacketInfo info;
    while (xQueueReceive(packetQueue, &info, 0) == pdTRUE) {
        int idx = findOrRegister(info.mac);
        if (idx >= 0) pilots[idx].rawRssi = info.rssi;
    }

    // 2. EMA filter + state machine per pilot
    for (int i = 0; i < pilotCount; i++) {
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

    // 3. Periodic RSSI telemetry
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        for (int i = 0; i < pilotCount; i++) sendRssi(i, now);
    }

    delay(10);
}
