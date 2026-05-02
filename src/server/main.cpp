#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define AP_SSID        "ELRS-Logger"
#define AP_CHANNEL     1
#define MAX_PILOTS     4
#define HISTORY_SIZE   500
#define WS_INTERVAL_MS 100
#define DETECT_SIZE    16
#define DETECT_TTL_MS  15000

// ── UART to/from Reader ───────────────────────────────────────
// Reader GPIO26(TX) → Server D3/GPIO4(RX)
// Server D2/GPIO3(TX) → Reader GPIO25(RX)
#define UART_RX_PIN  4       // D3 on XIAO ESP32-S3
#define UART_TX_PIN  3       // D2 on XIAO ESP32-S3
#define UART_BAUD    115200

struct Pilot {
    uint8_t  mac[6];
    uint8_t  uid[6];
    bool     uidSet;
    char     name[32];
    int8_t   rssi;
    uint32_t lastSeen;
    bool     active;
    bool     crossing;
    int8_t   enterAt;
    int8_t   exitAt;
};

struct Record {
    uint32_t ts;
    uint8_t  pilot;
    int8_t   rssi;
};

struct Detected {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t lastSeen;   // millis()
    bool     valid;
};

static Pilot        g_pilots[MAX_PILOTS];
static uint8_t      g_nPilots  = 0;
static Detected     g_detected[DETECT_SIZE];
static Record       g_hist[HISTORY_SIZE];
static uint16_t     g_histHead  = 0;
static uint16_t     g_histCnt   = 0;
static uint16_t     g_minLapMs  = 3000;
static portMUX_TYPE g_mux       = portMUX_INITIALIZER_UNLOCKED;

static DNSServer      dns;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static Preferences    prefs;
static uint32_t       g_lastWs = 0;
static Record         g_csvSnap[HISTORY_SIZE];

// ── UART RX line buffer ───────────────────────────────────────
static char     g_rxLine[512];
static uint16_t g_rxLen = 0;

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

// ── Push commands to Reader via UART ─────────────────────────
static void pushToReader(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    Serial2.println(out);
}

static void pushUidToReader(int pilot) {
    JsonDocument doc;
    doc["cmd"]   = "setuid";
    doc["pilot"] = pilot;
    doc["uid"]   = g_pilots[pilot].uidSet ? macStr(g_pilots[pilot].uid) : String("");
    pushToReader(doc);
}

static void pushThreshToReader(int pilot) {
    JsonDocument doc;
    doc["cmd"]     = "setthresh";
    doc["pilot"]   = pilot;
    doc["enterAt"] = g_pilots[pilot].enterAt;
    doc["exitAt"]  = g_pilots[pilot].exitAt;
    pushToReader(doc);
}

static void pushAllToReader() {
    for (int i = 0; i < MAX_PILOTS; i++) {
        pushUidToReader(i);
        pushThreshToReader(i);
    }
    Serial.println("[Server] pushed config to reader");
}

// ── JSON line processing (from Reader) ────────────────────────
static void processLine(const char* line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;
    const char* type  = doc["type"] | "";
    int         pilot = doc["pilot"] | -1;

    if (strcmp(type, "ready") == 0) {
        // Reader just booted — push all stored config
        pushAllToReader();
        Serial.println("[Reader] ready, config pushed");
        return;
    }

    if (strcmp(type, "detect") == 0) {
        const char* uidStr = doc["uid"] | "";
        const int8_t rssi  = (int8_t)(doc["rssi"] | -100);
        uint8_t mac[6];
        if (!parseMAC(uidStr, mac)) return;

        portENTER_CRITICAL(&g_mux);
        int slot = -1;
        for (int i = 0; i < DETECT_SIZE; i++) {
            if (g_detected[i].valid && memcmp(g_detected[i].mac, mac, 6) == 0) {
                slot = i; break;
            }
        }
        if (slot < 0) {
            int oldest = 0;
            uint32_t oldestTs = UINT32_MAX;
            for (int i = 0; i < DETECT_SIZE; i++) {
                if (!g_detected[i].valid) { oldest = i; break; }
                if (g_detected[i].lastSeen < oldestTs) {
                    oldestTs = g_detected[i].lastSeen;
                    oldest = i;
                }
            }
            slot = oldest;
            memcpy(g_detected[slot].mac, mac, 6);
            g_detected[slot].valid = true;
        }
        g_detected[slot].rssi     = rssi;
        g_detected[slot].lastSeen = millis();
        portEXIT_CRITICAL(&g_mux);
        return;
    }

    if (pilot < 0 || pilot >= MAX_PILOTS) return;

    if (strcmp(type, "rssi") == 0) {
        const int8_t rssi     = (int8_t)(doc["rssi"] | -100);
        const bool   crossing = doc["crossing"] | false;
        portENTER_CRITICAL(&g_mux);
        g_pilots[pilot].rssi     = rssi;
        g_pilots[pilot].crossing = crossing;
        g_pilots[pilot].lastSeen = millis();
        g_pilots[pilot].active   = true;
        g_hist[g_histHead] = {millis(), (uint8_t)pilot, rssi};
        g_histHead         = (g_histHead + 1) % HISTORY_SIZE;
        if (g_histCnt < HISTORY_SIZE) g_histCnt++;
        portEXIT_CRITICAL(&g_mux);

    } else if (strcmp(type, "lap") == 0) {
        const int8_t  rssi = (int8_t)(doc["rssi"] | -100);
        const uint32_t ts  = (uint32_t)(doc["ts"]  | 0u);
        portENTER_CRITICAL(&g_mux);
        g_pilots[pilot].crossing = false;
        portEXIT_CRITICAL(&g_mux);

        // Immediate WS broadcast for lap (before next periodic update)
        JsonDocument lapDoc;
        lapDoc["type"]  = "lap";
        lapDoc["pilot"] = pilot;
        lapDoc["uid"]   = g_pilots[pilot].uidSet
                            ? macStr(g_pilots[pilot].uid)
                            : macStr(g_pilots[pilot].mac);
        lapDoc["rssi"]  = rssi;
        lapDoc["ts"]    = ts;
        String lapJson;
        serializeJson(lapDoc, lapJson);
        ws.textAll(lapJson);
        Serial.printf("[LAP] P%d rssi=%d ts=%lu\n", pilot, rssi, (unsigned long)ts);

    } else if (strcmp(type, "new") == 0) {
        const char* uid = doc["uid"] | "";
        portENTER_CRITICAL(&g_mux);
        if (!g_pilots[pilot].active) {
            uint8_t mac[6];
            if (parseMAC(uid, mac)) {
                memcpy(g_pilots[pilot].mac, mac, 6);
                g_pilots[pilot].active = true;
                if (pilot >= (int)g_nPilots) g_nPilots = pilot + 1;
            }
        }
        portEXIT_CRITICAL(&g_mux);
        Serial.printf("[New] P%d=%s\n", pilot, uid);
    }
}

static void pollUart() {
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        if (c == '\n' || c == '\r') {
            if (g_rxLen > 0) {
                g_rxLine[g_rxLen] = '\0';
                processLine(g_rxLine);
                g_rxLen = 0;
            }
        } else if (g_rxLen < sizeof(g_rxLine) - 1) {
            g_rxLine[g_rxLen++] = c;
        }
    }
}

struct PilotSnap { Pilot data[MAX_PILOTS]; uint8_t n; };

static PilotSnap snapPilots() {
    PilotSnap s{};
    portENTER_CRITICAL(&g_mux);
    s.n = MAX_PILOTS;
    memcpy(s.data, g_pilots, sizeof(Pilot) * MAX_PILOTS);
    portEXIT_CRITICAL(&g_mux);
    return s;
}

static String buildJson(const PilotSnap& s) {
    JsonDocument doc;
    doc["minLapMs"] = g_minLapMs;
    JsonArray arr = doc["pilots"].to<JsonArray>();
    for (int i = 0; i < s.n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]       = i;
        o["name"]     = s.data[i].name;
        o["mac"]      = s.data[i].active ? macStr(s.data[i].mac) : "";
        o["uid"]      = s.data[i].uidSet  ? macStr(s.data[i].uid) : "";
        o["rssi"]     = s.data[i].rssi;
        o["ts"]       = s.data[i].lastSeen;
        o["crossing"] = s.data[i].crossing;
        o["enterAt"]  = s.data[i].enterAt;
        o["exitAt"]   = s.data[i].exitAt;
        o["active"]   = s.data[i].active;
    }

    // Detected MACs (for diagnostic UI)
    JsonArray det = doc["detected"].to<JsonArray>();
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < DETECT_SIZE; i++) {
        if (!g_detected[i].valid) continue;
        if (now - g_detected[i].lastSeen > DETECT_TTL_MS) continue;
        JsonObject o = det.add<JsonObject>();
        o["mac"]  = macStr(g_detected[i].mac);
        o["rssi"] = g_detected[i].rssi;
        o["age"]  = now - g_detected[i].lastSeen;
    }
    portEXIT_CRITICAL(&g_mux);

    String out;
    serializeJson(doc, out);
    return out;
}

static void loadPilotPrefs() {
    prefs.begin("pilots", true);

    for (int i = 0; i < MAX_PILOTS; i++) {
        memset(g_pilots[i].mac, 0, 6);
        memset(g_pilots[i].uid, 0, 6);
        g_pilots[i].uidSet   = false;
        snprintf(g_pilots[i].name, sizeof(g_pilots[i].name), "Pilot %d", i + 1);
        g_pilots[i].rssi     = -100;
        g_pilots[i].active   = false;
        g_pilots[i].crossing = false;
        g_pilots[i].enterAt  = -80;
        g_pilots[i].exitAt   = -90;
    }

    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.getBytes(k, g_pilots[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        g_pilots[i].uidSet  = prefs.getBool(k, false);
        snprintf(k, sizeof(k), "en%d", i);
        g_pilots[i].enterAt = (int8_t)prefs.getChar(k, -80);
        snprintf(k, sizeof(k), "ex%d", i);
        g_pilots[i].exitAt  = (int8_t)prefs.getChar(k, -90);
    }

    uint8_t n = min((unsigned)prefs.getUChar("n", 0), (unsigned)MAX_PILOTS);
    for (int i = 0; i < n; i++) {
        char k[12];
        snprintf(k, sizeof(k), "mac%d", i);
        prefs.getBytes(k, g_pilots[i].mac, 6);
        snprintf(k, sizeof(k), "name%d", i);
        prefs.getString(k, g_pilots[i].name, sizeof(g_pilots[i].name));
        g_pilots[i].active = true;
    }
    g_nPilots = n;

    prefs.end();
    Serial.printf("[Pilots] loaded %d active\n", g_nPilots);
}

static void savePilotPrefs(const PilotSnap& s) {
    prefs.begin("pilots", false);

    uint8_t n = 0;
    for (int i = 0; i < MAX_PILOTS; i++)
        if (s.data[i].active) n = i + 1;
    prefs.putUChar("n", n);

    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.putBytes(k, s.data[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        prefs.putBool(k, s.data[i].uidSet);
        snprintf(k, sizeof(k), "en%d", i);
        prefs.putChar(k, s.data[i].enterAt);
        snprintf(k, sizeof(k), "ex%d", i);
        prefs.putChar(k, s.data[i].exitAt);
        if (s.data[i].active) {
            snprintf(k, sizeof(k), "mac%d", i);
            prefs.putBytes(k, s.data[i].mac, 6);
            snprintf(k, sizeof(k), "name%d", i);
            prefs.putString(k, s.data[i].name);
        }
    }
    prefs.end();
}

static void loadSettings() {
    prefs.begin("settings", true);
    g_minLapMs = prefs.getUShort("minLapMs", 3000);
    prefs.end();
    Serial.printf("[Settings] minLapMs=%d\n", g_minLapMs);
}

static void saveSettings() {
    prefs.begin("settings", false);
    prefs.putUShort("minLapMs", g_minLapMs);
    prefs.end();
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ELRS Server ===");

    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.printf("UART RX=%d TX=%d @ %d baud\n", UART_RX_PIN, UART_TX_PIN, UART_BAUD);

    if (!LittleFS.begin(true))
        Serial.println("[WARN] LittleFS mount failed");

    loadPilotPrefs();
    loadSettings();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, "", AP_CHANNEL);
    const IPAddress apIP = WiFi.softAPIP();
    Serial.printf("AP: %s  CH: %d  IP: %s\n", AP_SSID, AP_CHANNEL, apIP.toString().c_str());

    // キャプティブポータル: 全ドメインをAP IPへ向ける
    dns.start(53, "*", apIP);

    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* c,
                  AwsEventType t, void*, uint8_t*, size_t) {
        if (t == WS_EVT_CONNECT) {
            Serial.printf("[WS] client %u connected\n", c->id());
            c->text(buildJson(snapPilots()));
        }
    });
    server.addHandler(&ws);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ── Captive portal ────────────────────────────────────────
    // iOS: /hotspot-detect.html → redirect
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    // iOS (older): /library/test/success.html
    server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    // Android: /generate_204
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    // Windows: /ncsi.txt
    server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    // 未マッチは全てトップへリダイレクト
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });

    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson(snapPilots()));
    });

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
            const char* name   = doc["name"] | "";
            const char* uidStr = doc["uid"]  | "";
            portENTER_CRITICAL(&g_mux);
            if (strlen(name) > 0)
                strncpy(g_pilots[id].name, name, sizeof(g_pilots[id].name) - 1);
            if (strlen(uidStr) == 17) {
                uint8_t uid[6];
                if (parseMAC(uidStr, uid)) {
                    memcpy(g_pilots[id].uid, uid, 6);
                    g_pilots[id].uidSet = true;
                    Serial.printf("[UID] P%d set %s\n", id, uidStr);
                }
            } else if (strlen(uidStr) == 0 && doc["uid"].is<const char*>()) {
                memset(g_pilots[id].uid, 0, 6);
                g_pilots[id].uidSet = false;
                Serial.printf("[UID] P%d cleared\n", id);
            }
            portEXIT_CRITICAL(&g_mux);
            pushUidToReader(id);
            savePilotPrefs(snapPilots());
            req->send(200, "application/json", R"({"ok":true})");
        });

    server.on("/api/thresholds", HTTP_POST,
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
            int8_t en, ex;
            portENTER_CRITICAL(&g_mux);
            en = (int8_t)(doc["enterAt"] | (int)g_pilots[id].enterAt);
            ex = (int8_t)(doc["exitAt"]  | (int)g_pilots[id].exitAt);
            portEXIT_CRITICAL(&g_mux);
            if (ex >= en) {
                req->send(400, "application/json", R"({"error":"exitAt must be < enterAt"})");
                return;
            }
            portENTER_CRITICAL(&g_mux);
            g_pilots[id].enterAt = en;
            g_pilots[id].exitAt  = ex;
            portEXIT_CRITICAL(&g_mux);
            pushThreshToReader(id);
            savePilotPrefs(snapPilots());
            Serial.printf("[Thresh] P%d EnterAt=%d ExitAt=%d\n", id, en, ex);
            req->send(200, "application/json", R"({"ok":true})");
        });

    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["minLapMs"] = g_minLapMs;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/api/channel", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", R"({"error":"invalid JSON"})");
                return;
            }
            int ch = doc["channel"] | -1;
            if (ch < 1 || ch > 13) {
                req->send(400, "application/json", R"({"error":"channel must be 1..13"})");
                return;
            }
            JsonDocument cmd;
            cmd["cmd"]     = "setchannel";
            cmd["channel"] = ch;
            pushToReader(cmd);
            Serial.printf("[Channel] set to %d\n", ch);
            req->send(200, "application/json", R"({"ok":true})");
        });

    server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", R"({"error":"invalid JSON"})");
                return;
            }
            if (doc["minLapMs"].is<int>())
                g_minLapMs = (uint16_t)constrain((int)doc["minLapMs"], 500, 60000);
            saveSettings();
            req->send(200, "application/json", R"({"ok":true})");
        });

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

    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        portENTER_CRITICAL(&g_mux);
        g_nPilots  = 0;
        g_histHead = 0;
        g_histCnt  = 0;
        memset(g_pilots, 0, sizeof(g_pilots));
        for (int i = 0; i < MAX_PILOTS; i++) {
            snprintf(g_pilots[i].name, sizeof(g_pilots[i].name), "Pilot %d", i + 1);
            g_pilots[i].rssi    = -100;
            g_pilots[i].enterAt = -80;
            g_pilots[i].exitAt  = -90;
        }
        portEXIT_CRITICAL(&g_mux);
        prefs.begin("pilots", false);
        prefs.clear();
        prefs.end();
        JsonDocument doc;
        doc["cmd"] = "reset";
        String out;
        serializeJson(doc, out);
        Serial2.println(out);
        req->send(200, "application/json", R"({"ok":true})");
    });

    server.begin();
    Serial.println("HTTP server ready. Connect to WiFi: " AP_SSID);
}

void loop() {
    dns.processNextRequest();
    pollUart();
    ws.cleanupClients();
    const uint32_t now = millis();
    if (now - g_lastWs >= WS_INTERVAL_MS && ws.count() > 0) {
        g_lastWs = now;
        ws.textAll(buildJson(snapPilots()));
    }
}
