#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define AP_SSID        "ELRS-Logger"
#define AP_CHANNEL     1
#define MAX_PILOTS     4
#define HISTORY_SIZE   500
#define WS_INTERVAL_MS 100

struct Pilot {
    uint8_t  mac[6];
    char     name[32];
    int8_t   rssi;
    uint32_t lastSeen;
    bool     active;
};

struct Record {
    uint32_t ts;
    uint8_t  pilot;
    int8_t   rssi;
};

static Pilot       g_pilots[MAX_PILOTS];
static uint8_t     g_nPilots  = 0;
static Record      g_hist[HISTORY_SIZE];
static uint16_t    g_histHead  = 0;
static uint16_t    g_histCnt   = 0;
static portMUX_TYPE g_mux      = portMUX_INITIALIZER_UNLOCKED;

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static Preferences    prefs;
static uint32_t       g_lastWs = 0;
static Record         g_csvSnap[HISTORY_SIZE];

static String macStr(const uint8_t* m) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

// Must be called inside critical section
static int findPilot(const uint8_t* mac) {
    for (int i = 0; i < g_nPilots; i++)
        if (memcmp(g_pilots[i].mac, mac, 6) == 0) return i;
    return -1;
}

// 802.11 Action frame (ESP-NOW) promiscuous callback.
// Runs in WiFi task context; use portENTER_CRITICAL_ISR for safety.
static void IRAM_ATTR onPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const auto* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* p = pkt->payload;
    const uint16_t len = pkt->rx_ctrl.sig_len;

    // Management Action frame: FC[0]=0xD0
    if (len < 28 || p[0] != 0xD0) return;
    // Vendor-specific category (0x7F) + Espressif OUI (18:FE:34) = ESP-NOW
    if (p[24] != 0x7F || p[25] != 0x18 || p[26] != 0xFE || p[27] != 0x34) return;

    const uint8_t* src  = p + 10; // Source MAC at offset 10 in 802.11 header
    const int8_t   rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    const uint32_t now  = millis();

    portENTER_CRITICAL_ISR(&g_mux);
    int idx = findPilot(src);
    if (idx < 0 && g_nPilots < MAX_PILOTS) {
        idx = g_nPilots++;
        memcpy(g_pilots[idx].mac, src, 6);
        snprintf(g_pilots[idx].name, sizeof(g_pilots[idx].name), "Pilot %d", idx + 1);
        g_pilots[idx].active = true;
    }
    if (idx >= 0) {
        g_pilots[idx].rssi     = rssi;
        g_pilots[idx].lastSeen = now;
        g_hist[g_histHead]     = {now, static_cast<uint8_t>(idx), rssi};
        g_histHead             = (g_histHead + 1) % HISTORY_SIZE;
        if (g_histCnt < HISTORY_SIZE) g_histCnt++;
    }
    portEXIT_CRITICAL_ISR(&g_mux);
}

struct PilotSnap { Pilot data[MAX_PILOTS]; uint8_t n; };

static PilotSnap snapPilots() {
    PilotSnap s{};
    portENTER_CRITICAL(&g_mux);
    s.n = g_nPilots;
    memcpy(s.data, g_pilots, sizeof(Pilot) * s.n);
    portEXIT_CRITICAL(&g_mux);
    return s;
}

static String buildJson(const PilotSnap& s) {
    JsonDocument doc;
    JsonArray arr = doc["pilots"].to<JsonArray>();
    for (int i = 0; i < s.n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]   = i;
        o["name"] = s.data[i].name;
        o["mac"]  = macStr(s.data[i].mac);
        o["rssi"] = s.data[i].rssi;
        o["ts"]   = s.data[i].lastSeen;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static void loadPrefs() {
    prefs.begin("pilots", true);
    g_nPilots = min((unsigned)prefs.getUChar("n", 0), (unsigned)MAX_PILOTS);
    for (int i = 0; i < g_nPilots; i++) {
        char k[12];
        snprintf(k, sizeof(k), "mac%d", i);
        prefs.getBytes(k, g_pilots[i].mac, 6);
        snprintf(k, sizeof(k), "name%d", i);
        prefs.getString(k, g_pilots[i].name, sizeof(g_pilots[i].name));
        g_pilots[i].rssi   = -100;
        g_pilots[i].active = true;
    }
    prefs.end();
}

static void savePrefs(const PilotSnap& s) {
    prefs.begin("pilots", false);
    prefs.putUChar("n", s.n);
    for (int i = 0; i < s.n; i++) {
        char k[12];
        snprintf(k, sizeof(k), "mac%d", i);
        prefs.putBytes(k, s.data[i].mac, 6);
        snprintf(k, sizeof(k), "name%d", i);
        prefs.putString(k, s.data[i].name);
    }
    prefs.end();
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ELRS Backpack RSSI Logger ===");

    if (!LittleFS.begin(true))
        Serial.println("[WARN] LittleFS mount failed");

    loadPrefs();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, "", AP_CHANNEL);
    Serial.printf("AP: %s  CH: %d  IP: %s\n",
                  AP_SSID, AP_CHANNEL, WiFi.softAPIP().toString().c_str());

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);

    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* c,
                  AwsEventType t, void*, uint8_t*, size_t) {
        if (t == WS_EVT_CONNECT) {
            Serial.printf("[WS] client %u connected\n", c->id());
            c->text(buildJson(snapPilots()));
        }
    });
    server.addHandler(&ws);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // GET /api/pilots
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson(snapPilots()));
    });

    // POST /api/pilots — update pilot name by id
    server.on("/api/pilots", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", R"({"error":"invalid JSON"})");
                return;
            }
            int id = doc["id"] | -1;
            if (id < 0 || id >= MAX_PILOTS) {
                req->send(400, "application/json", R"({"error":"invalid id"})");
                return;
            }
            const char* name = doc["name"] | "";
            portENTER_CRITICAL(&g_mux);
            if (id < g_nPilots)
                strncpy(g_pilots[id].name, name, sizeof(g_pilots[id].name) - 1);
            portEXIT_CRITICAL(&g_mux);
            savePrefs(snapPilots());
            req->send(200, "application/json", R"({"ok":true})");
        });

    // GET /api/csv — download RSSI history as CSV
    server.on("/api/csv", HTTP_GET, [](AsyncWebServerRequest* req) {
        uint16_t cnt, head;
        uint8_t  np;
        char     names[MAX_PILOTS][32];

        portENTER_CRITICAL(&g_mux);
        cnt  = g_histCnt;
        head = g_histHead;
        np   = g_nPilots;
        memcpy(g_csvSnap, g_hist, sizeof(Record) * HISTORY_SIZE);
        for (int i = 0; i < np; i++) memcpy(names[i], g_pilots[i].name, 32);
        portEXIT_CRITICAL(&g_mux);

        String csv = "timestamp_ms,pilot_id,pilot_name,rssi_dbm\n";
        const uint16_t start = (uint16_t)((head - cnt + HISTORY_SIZE) % HISTORY_SIZE);
        for (uint16_t i = 0; i < cnt; i++) {
            const Record& r = g_csvSnap[(start + i) % HISTORY_SIZE];
            csv += String(r.ts) + ',' + r.pilot + ',';
            csv += (r.pilot < np ? names[r.pilot] : "?");
            csv += ',' + String(r.rssi) + '\n';
        }
        auto* resp = req->beginResponse(200, "text/csv", csv);
        resp->addHeader("Content-Disposition", "attachment; filename=rssi_log.csv");
        req->send(resp);
    });

    // POST /api/reset — clear all detected pilots and history
    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        portENTER_CRITICAL(&g_mux);
        g_nPilots  = 0;
        g_histHead = 0;
        g_histCnt  = 0;
        memset(g_pilots, 0, sizeof(g_pilots));
        portEXIT_CRITICAL(&g_mux);
        prefs.begin("pilots", false);
        prefs.clear();
        prefs.end();
        req->send(200, "application/json", R"({"ok":true})");
    });

    server.begin();
    Serial.println("HTTP server ready. Connect to WiFi: " AP_SSID);
}

void loop() {
    ws.cleanupClients();
    const uint32_t now = millis();
    if (now - g_lastWs >= WS_INTERVAL_MS && ws.count() > 0) {
        g_lastWs = now;
        ws.textAll(buildJson(snapPilots()));
    }
}
