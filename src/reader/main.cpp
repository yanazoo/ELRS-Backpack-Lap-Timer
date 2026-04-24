#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

// ── Config ────────────────────────────────────────────────────
#define LISTEN_CHANNEL  1
#define UART_RX_PIN     16      // ← Server GPIO17
#define UART_TX_PIN     17      // → Server GPIO16
#define UART_BAUD       115200
#define MAX_PILOTS      4
#define EMA_ALPHA       0.3f
#define RSSI_PERIOD_MS  100

// ── Pilot State ───────────────────────────────────────────────
struct PilotState {
    uint8_t  uid[6];
    bool     uidSet;
    uint8_t  mac[6];
    bool     macSet;
    int8_t   enterAt;
    int8_t   exitAt;
    float    emaRssi;
    bool     crossing;
    int8_t   peakRssi;
    uint32_t peakTs;
    uint32_t lastReportMs;
};

static PilotState g_pilots[MAX_PILOTS];
static uint8_t    g_nAuto = 0;

// ── ISR FIFO ──────────────────────────────────────────────────
struct Pkt { uint8_t mac[6]; int8_t rssi; };
static Pkt          g_q[32];
static volatile int g_qH   = 0;
static volatile int g_qT   = 0;
static portMUX_TYPE g_qMux = portMUX_INITIALIZER_UNLOCKED;

// ── UART RX line buffer ───────────────────────────────────────
static char    g_rxLine[256];
static uint8_t g_rxLen = 0;

static Preferences prefs;

// ── Promiscuous callback (IRAM) ───────────────────────────────
static void IRAM_ATTR onPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const auto*    pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* p   = pkt->payload;
    const uint16_t len = pkt->rx_ctrl.sig_len;
    // 802.11 Action frame + Espressif ESP-NOW OUI
    if (len < 28 || p[0] != 0xD0) return;
    if (p[24] != 0x7F || p[25] != 0x18 || p[26] != 0xFE || p[27] != 0x34) return;
    const uint8_t* mac  = p + 10;
    const int8_t   rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    portENTER_CRITICAL_ISR(&g_qMux);
    const int next = (g_qH + 1) & 31;
    if (next != g_qT) {
        memcpy(g_q[g_qH].mac, mac, 6);
        g_q[g_qH].rssi = rssi;
        g_qH = next;
    }
    portEXIT_CRITICAL_ISR(&g_qMux);
}

// ── Helpers ───────────────────────────────────────────────────
static String macStr(const uint8_t* m) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

static bool parseMAC(const char* str, uint8_t* out) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &out[0], &out[1], &out[2], &out[3], &out[4], &out[5]) == 6;
}

static void sendJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    Serial2.println(out);
}

// ── NVS ───────────────────────────────────────────────────────
static void savePrefs() {
    prefs.begin("reader", false);
    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.putBytes(k, g_pilots[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        prefs.putBool(k, g_pilots[i].uidSet);
        snprintf(k, sizeof(k), "en%d", i);
        prefs.putChar(k, g_pilots[i].enterAt);
        snprintf(k, sizeof(k), "ex%d", i);
        prefs.putChar(k, g_pilots[i].exitAt);
    }
    prefs.end();
}

static void loadPrefs() {
    prefs.begin("reader", true);
    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.getBytes(k, g_pilots[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        g_pilots[i].uidSet  = prefs.getBool(k, false);
        snprintf(k, sizeof(k), "en%d", i);
        g_pilots[i].enterAt = (int8_t)prefs.getChar(k, -80);
        snprintf(k, sizeof(k), "ex%d", i);
        g_pilots[i].exitAt  = (int8_t)prefs.getChar(k, -90);
    }
    prefs.end();
}

// ── Packet processing ─────────────────────────────────────────
static int findByMac(const uint8_t* mac) {
    for (int i = 0; i < MAX_PILOTS; i++)
        if (g_pilots[i].macSet && memcmp(g_pilots[i].mac, mac, 6) == 0) return i;
    return -1;
}

static void processPkt(const uint8_t* mac, int8_t rawRssi) {
    int idx = findByMac(mac);

    if (idx < 0) {
        bool anyUid = false;
        for (int i = 0; i < MAX_PILOTS; i++)
            if (g_pilots[i].uidSet) { anyUid = true; break; }

        if (anyUid) {
            // UID mode: only accept MACs matching a configured UID slot
            for (int i = 0; i < MAX_PILOTS; i++) {
                if (g_pilots[i].uidSet && memcmp(g_pilots[i].uid, mac, 6) == 0) {
                    memcpy(g_pilots[i].mac, mac, 6);
                    g_pilots[i].macSet  = true;
                    g_pilots[i].emaRssi = (float)rawRssi;
                    idx = i;
                    Serial.printf("[UID] P%d matched %s\n", i, macStr(mac).c_str());
                    break;
                }
            }
            if (idx < 0) return;
        } else {
            // Auto-discover: assign first 4 MACs to pilot slots
            if (g_nAuto >= MAX_PILOTS) return;
            idx = g_nAuto++;
            memcpy(g_pilots[idx].mac, mac, 6);
            g_pilots[idx].macSet   = true;
            g_pilots[idx].emaRssi  = (float)rawRssi;
            g_pilots[idx].crossing = false;
            g_pilots[idx].peakRssi = -127;
            Serial.printf("[Auto] P%d=%s\n", idx, macStr(mac).c_str());
            JsonDocument doc;
            doc["type"]  = "new";
            doc["pilot"] = idx;
            doc["uid"]   = macStr(mac);
            sendJson(doc);
        }
    }

    PilotState& p = g_pilots[idx];

    // EMA filter (α=0.3): smoothes noise before state machine
    p.emaRssi = EMA_ALPHA * rawRssi + (1.0f - EMA_ALPHA) * p.emaRssi;
    const int8_t rssi = (int8_t)roundf(p.emaRssi);

    const uint32_t now = millis();

    // RotorHazard state machine: CLEAR → CROSSING → CLEAR
    if (!p.crossing) {
        if (rssi > p.enterAt) {
            p.crossing = true;
            p.peakRssi = rssi;
            p.peakTs   = now;
        }
    } else {
        if (rssi > p.peakRssi) {
            p.peakRssi = rssi;
            p.peakTs   = now;
        }
        if (rssi < p.exitAt) {
            p.crossing = false;
            // Lap event uses the peak timestamp (moment of closest gate pass)
            JsonDocument doc;
            doc["type"]  = "lap";
            doc["pilot"] = idx;
            doc["uid"]   = macStr(p.uidSet ? p.uid : p.mac);
            doc["rssi"]  = p.peakRssi;
            doc["ts"]    = p.peakTs;
            sendJson(doc);
        }
    }

    // Periodic RSSI report (includes crossing state for UI)
    if (now - p.lastReportMs >= RSSI_PERIOD_MS) {
        p.lastReportMs = now;
        JsonDocument doc;
        doc["type"]     = "rssi";
        doc["pilot"]    = idx;
        doc["rssi"]     = rssi;
        doc["crossing"] = p.crossing;
        doc["ts"]       = now;
        sendJson(doc);
    }
}

// ── UART command processing (from Server) ─────────────────────
static void processCommand(const char* line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;
    const char* cmd   = doc["cmd"] | "";
    int         pilot = doc["pilot"] | -1;

    if (strcmp(cmd, "setuid") == 0 && pilot >= 0 && pilot < MAX_PILOTS) {
        const char* uid = doc["uid"] | "";
        if (strlen(uid) == 17) {
            parseMAC(uid, g_pilots[pilot].uid);
            g_pilots[pilot].uidSet = true;
            g_pilots[pilot].macSet = false;  // force re-match on next packet
            Serial.printf("[CMD] P%d UID=%s\n", pilot, uid);
        } else {
            memset(g_pilots[pilot].uid, 0, 6);
            g_pilots[pilot].uidSet = false;
            Serial.printf("[CMD] P%d UID cleared\n", pilot);
        }
        savePrefs();
    } else if (strcmp(cmd, "setthresh") == 0 && pilot >= 0 && pilot < MAX_PILOTS) {
        if (doc["enterAt"].is<int>())
            g_pilots[pilot].enterAt = (int8_t)(int)doc["enterAt"];
        if (doc["exitAt"].is<int>())
            g_pilots[pilot].exitAt  = (int8_t)(int)doc["exitAt"];
        savePrefs();
        Serial.printf("[CMD] P%d thresh en=%d ex=%d\n", pilot,
                      g_pilots[pilot].enterAt, g_pilots[pilot].exitAt);
    } else if (strcmp(cmd, "reset") == 0) {
        g_nAuto = 0;
        for (int i = 0; i < MAX_PILOTS; i++) {
            memset(g_pilots[i].mac, 0, 6);
            g_pilots[i].macSet   = false;
            g_pilots[i].crossing = false;
            g_pilots[i].emaRssi  = -100.0f;
            g_pilots[i].peakRssi = -127;
        }
        Serial.println("[CMD] reset");
    }
}

static void pollUartRx() {
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        if (c == '\n' || c == '\r') {
            if (g_rxLen > 0) {
                g_rxLine[g_rxLen] = '\0';
                processCommand(g_rxLine);
                g_rxLen = 0;
            }
        } else if (g_rxLen < sizeof(g_rxLine) - 1) {
            g_rxLine[g_rxLen++] = c;
        }
    }
}

// ── Setup / Loop ──────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    memset(g_pilots, 0, sizeof(g_pilots));
    for (int i = 0; i < MAX_PILOTS; i++) {
        g_pilots[i].emaRssi  = -100.0f;
        g_pilots[i].peakRssi = -127;
        g_pilots[i].enterAt  = -80;
        g_pilots[i].exitAt   = -90;
    }
    loadPrefs();

    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("\n=== ELRS Reader  ch.%d ===\n", LISTEN_CHANNEL);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);
    esp_wifi_set_channel(LISTEN_CHANNEL, WIFI_SECOND_CHAN_NONE);

    Serial.println("Listening for ESP-NOW frames...");

    // Signal server that reader is ready to receive config push
    JsonDocument doc;
    doc["type"] = "ready";
    sendJson(doc);
}

void loop() {
    pollUartRx();

    portENTER_CRITICAL(&g_qMux);
    int h = g_qH;
    portEXIT_CRITICAL(&g_qMux);

    int t = g_qT;
    while (t != h) {
        processPkt(g_q[t].mac, g_q[t].rssi);
        t = (t + 1) & 31;
        portENTER_CRITICAL(&g_qMux);
        g_qT = t;
        h    = g_qH;
        portEXIT_CRITICAL(&g_qMux);
    }
}
