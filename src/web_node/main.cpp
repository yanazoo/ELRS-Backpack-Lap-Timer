/*
 * ELRS Backpack Lap Timer — Web Node
 * Hardware : XIAO ESP32-S3-B
 *
 * Roles
 *   - WiFi AP  SSID="ELRS bp-LT"  PASS="elrsbp-lt"  IP=20.0.0.1
 *   - Serve index.html from LittleFS
 *   - WebSocket /ws — real-time push to browsers
 *   - REST API /api/pilots  /api/race/start|stop  /api/laps  /api/config
 *   - Receive JSON lines from Gate Node via Serial1 (UART RX = GPIO3/D2)
 *   - Send sync commands to Gate Node via Serial1  (UART TX = GPIO2/D1)
 *   - Store pilot config in NVS (Preferences)
 *   - GET /api/uid?phrase=...  compute ELRS UID (mbedTLS SHA-256)
 *
 * Wiring
 *   XIAO ESP32-S3 GPIO3 (D2/RX1) ← ESP32-WROVER-E GPIO26 (TX1)
 *   XIAO ESP32-S3 GPIO2 (D1/TX1) → ESP32-WROVER-E GPIO25 (RX1)
 *   Common GND
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

// ── Network ────────────────────────────────────────────────────────────────
static const char*  AP_SSID    = "ELRS bp-LT";
static const char*  AP_PASS    = "elrsbp-lt";
static const IPAddress AP_IP      (20, 0, 0, 1);
static const IPAddress AP_GATEWAY (20, 0, 0, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);

// ── UART ↔ Gate Node ──────────────────────────────────────────────────────
#define GATE_RX_PIN   3     // D2 on XIAO ESP32-S3 ← Gate Node GPIO26 (TX)
#define GATE_TX_PIN   2     // D1 on XIAO ESP32-S3 → Gate Node GPIO25 (RX)
#define GATE_BAUD     115200

// ── Pilot data ─────────────────────────────────────────────────────────────
#define MAX_PILOTS  4
#define MAX_LAPS    200

struct PilotConfig {
    char    name[32];
    char    bindPhrase[64];
    uint8_t uid[6];
    bool    hasUid;
};

struct LapRecord {
    int      pilotIdx;
    uint32_t lapTimeMs;
    uint32_t timestamp;
};

struct PilotRuntime {
    int      rssi;
    int      rawRssi;
    bool     crossing;
    uint32_t lastTs;
    uint32_t lapCount;
    uint32_t bestLapMs;
    uint32_t lastLapTs;   // gate-node timestamp of last lap trigger
};

static PilotConfig  cfg[MAX_PILOTS];
static PilotRuntime rt[MAX_PILOTS];
static LapRecord    laps[MAX_LAPS];
static int          lapCount = 0;

static bool      raceRunning   = false;
static uint32_t  raceStartMs   = 0;

// ── Server & WebSocket ─────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences    prefs;

// ── NVS helpers ────────────────────────────────────────────────────────────
static void loadConfig() {
    prefs.begin("pilots", true);
    for (int i = 0; i < MAX_PILOTS; i++) {
        char key[24];

        snprintf(key, sizeof(key), "p%d_name", i);
        String n = prefs.getString(key, "");
        if (n.length()) strncpy(cfg[i].name, n.c_str(), sizeof(cfg[i].name) - 1);
        else            snprintf(cfg[i].name, sizeof(cfg[i].name), "PILOT %d", i + 1);

        snprintf(key, sizeof(key), "p%d_phrase", i);
        String ph = prefs.getString(key, "");
        strncpy(cfg[i].bindPhrase, ph.c_str(), sizeof(cfg[i].bindPhrase) - 1);

        snprintf(key, sizeof(key), "p%d_uid", i);
        String u = prefs.getString(key, "");
        cfg[i].hasUid = (u.length() == 17);
        if (cfg[i].hasUid) {
            sscanf(u.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                   &cfg[i].uid[0], &cfg[i].uid[1], &cfg[i].uid[2],
                   &cfg[i].uid[3], &cfg[i].uid[4], &cfg[i].uid[5]);
        }
    }
    prefs.end();
}

static void saveOnePilot(int idx) {
    prefs.begin("pilots", false);
    char key[24];
    snprintf(key, sizeof(key), "p%d_name",   idx); prefs.putString(key, cfg[idx].name);
    snprintf(key, sizeof(key), "p%d_phrase", idx); prefs.putString(key, cfg[idx].bindPhrase);
    if (cfg[idx].hasUid) {
        char uidStr[18];
        snprintf(uidStr, sizeof(uidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg[idx].uid[0], cfg[idx].uid[1], cfg[idx].uid[2],
                 cfg[idx].uid[3], cfg[idx].uid[4], cfg[idx].uid[5]);
        snprintf(key, sizeof(key), "p%d_uid", idx);
        prefs.putString(key, uidStr);
    }
    prefs.end();
}

// ── JSON builders ──────────────────────────────────────────────────────────
static void appendPilotObj(JsonArray arr, int i) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]   = i;
    o["name"] = cfg[i].name;
    o["bindPhrase"] = cfg[i].bindPhrase;
    if (cfg[i].hasUid) {
        char u[18];
        snprintf(u, sizeof(u), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg[i].uid[0], cfg[i].uid[1], cfg[i].uid[2],
                 cfg[i].uid[3], cfg[i].uid[4], cfg[i].uid[5]);
        o["uid"] = u;
    } else {
        o["uid"] = "";
    }
}

static String pilotsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_PILOTS; i++) appendPilotObj(arr, i);
    String s; serializeJson(doc, s); return s;
}

static String lapsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < lapCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["pilot"]   = laps[i].pilotIdx;
        o["lapTime"] = laps[i].lapTimeMs;
        o["ts"]      = laps[i].timestamp;
        o["name"]    = cfg[laps[i].pilotIdx].name;
    }
    String s; serializeJson(doc, s); return s;
}

// ── Send command to Gate Node ─────────────────────────────────────────────
static void sendGateCmd(const char* action) {
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"%s"})", action);
    Serial1.println(buf);
    Serial.printf("[Web] → Gate: %s\n", buf);
}

// ── WebSocket broadcast ────────────────────────────────────────────────────
static void wsText(const String& msg) { ws.textAll(msg); }

// ── Process one JSON line from Gate Node ───────────────────────────────────
static void processGateLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;

    const char* type = doc["type"] | "";

    if (strcmp(type, "rssi") == 0) {
        int   idx      = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;

        rt[idx].rssi     = doc["rssi"]     | -120;
        rt[idx].rawRssi  = doc["raw"]      | -120;
        rt[idx].crossing = doc["crossing"] | false;
        rt[idx].lastTs   = doc["ts"]       | 0u;

        // Forward RSSI update to WebSocket clients as-is
        wsText(line);
        return;
    }

    if (strcmp(type, "lap") == 0) {
        int      idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        uint32_t ts  = doc["ts"]    | 0u;

        // Compute lap duration
        uint32_t lapMs = 0;
        if (rt[idx].lastLapTs > 0 && raceRunning) {
            lapMs = ts - rt[idx].lastLapTs;
        }
        rt[idx].lastLapTs = ts;
        rt[idx].lapCount++;

        bool newBest = false;
        if (lapMs > 0 && (rt[idx].bestLapMs == 0 || lapMs < rt[idx].bestLapMs)) {
            rt[idx].bestLapMs = lapMs;
            newBest = true;
        }

        if (lapCount < MAX_LAPS) {
            laps[lapCount++] = { idx, lapMs, ts };
        }

        // Build WebSocket event
        JsonDocument wd;
        wd["type"]     = "lap";
        wd["pilot"]    = idx;
        wd["name"]     = cfg[idx].name;
        wd["lapTime"]  = lapMs;
        wd["bestLap"]  = rt[idx].bestLapMs;
        wd["lapCount"] = rt[idx].lapCount;
        wd["newBest"]  = newBest;
        wd["rssi"]     = doc["rssi"] | -120;
        wd["ts"]       = ts;

        String ws_msg;
        serializeJson(wd, ws_msg);
        wsText(ws_msg);
        return;
    }
}

// ── WebSocket event handler ────────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType evType, void* arg, uint8_t* data, size_t len) {
    if (evType != WS_EVT_CONNECT) return;

    // Send current state snapshot
    JsonDocument doc;
    doc["type"]        = "init";
    doc["raceRunning"] = raceRunning;
    doc["raceStartMs"] = raceStartMs;
    JsonArray pa = doc["pilots"].to<JsonArray>();
    for (int i = 0; i < MAX_PILOTS; i++) {
        JsonObject o = pa.add<JsonObject>();
        o["id"]        = i;
        o["name"]      = cfg[i].name;
        o["lapCount"]  = rt[i].lapCount;
        o["bestLap"]   = rt[i].bestLapMs;
        o["rssi"]      = rt[i].rssi;
        o["crossing"]  = rt[i].crossing;
    }
    String msg; serializeJson(doc, msg);
    client->text(msg);
}

// ── REST: POST body accumulator ────────────────────────────────────────────
// ESPAsyncWebServer delivers POST bodies chunk-by-chunk.
// We collect chunks into a heap buffer keyed to the request pointer.
struct BodyBuf { char* buf; size_t total; };

static void handleBody(AsyncWebServerRequest* req,
                        uint8_t* data, size_t len,
                        size_t index, size_t total,
                        std::function<void(AsyncWebServerRequest*, const char*)> cb) {
    if (index == 0) {
        auto* bb = new BodyBuf{ new char[total + 1], total };
        req->_tempObject = bb;
    }
    auto* bb = reinterpret_cast<BodyBuf*>(req->_tempObject);
    if (bb) {
        memcpy(bb->buf + index, data, len);
        if (index + len == total) {
            bb->buf[total] = '\0';
            cb(req, bb->buf);
            delete[] bb->buf;
            delete bb;
            req->_tempObject = nullptr;
        }
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Web] ELRS Backpack Lap Timer — Web Node");

    // UART ↔ Gate Node (bidirectional)
    Serial1.begin(GATE_BAUD, SERIAL_8N1, GATE_RX_PIN, GATE_TX_PIN);

    // Pilot config from NVS
    loadConfig();
    memset(rt, 0, sizeof(rt));

    // WiFi AP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[Web] AP up  SSID=%s  IP=%s\n", AP_SSID, AP_IP.toString().c_str());

    // LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("[Web] LittleFS mount failed — upload data/ first");
    }

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── GET /api/pilots ────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", pilotsJson());
    });

    // ── POST /api/pilots ───────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            handleBody(req, data, len, index, total,
                [](AsyncWebServerRequest* req2, const char* body) {
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok) {
                        req2->send(400, "application/json", R"({"error":"bad json"})");
                        return;
                    }
                    int id = doc["id"] | -1;
                    if (id < 0 || id >= MAX_PILOTS) {
                        req2->send(400, "application/json", R"({"error":"bad id"})");
                        return;
                    }
                    const char* name   = doc["name"]       | "";
                    const char* phrase = doc["bindPhrase"] | "";
                    const char* uid    = doc["uid"]        | "";

                    strncpy(cfg[id].name,        name,   sizeof(cfg[id].name)        - 1);
                    strncpy(cfg[id].bindPhrase,  phrase, sizeof(cfg[id].bindPhrase)  - 1);
                    cfg[id].hasUid = (strlen(uid) == 17);
                    if (cfg[id].hasUid) {
                        sscanf(uid, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                               &cfg[id].uid[0], &cfg[id].uid[1], &cfg[id].uid[2],
                               &cfg[id].uid[3], &cfg[id].uid[4], &cfg[id].uid[5]);
                    }
                    saveOnePilot(id);
                    req2->send(200, "application/json", R"({"ok":true})");
                });
        });

    // ── POST /api/race/start ───────────────────────────────────────────────
    server.on("/api/race/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning  = true;
        raceStartMs  = millis();
        lapCount     = 0;
        for (int i = 0; i < MAX_PILOTS; i++) {
            rt[i].lapCount   = 0;
            rt[i].bestLapMs  = 0;
            rt[i].lastLapTs  = 0;
        }
        // Sync: tell gate node to reset pilot detection state
        sendGateCmd("race_start");

        JsonDocument doc;
        doc["type"] = "race_start";
        doc["ts"]   = raceStartMs;
        String msg; serializeJson(doc, msg);
        wsText(msg);
        req->send(200, "application/json", R"({"ok":true})");
    });

    // ── POST /api/race/stop ────────────────────────────────────────────────
    server.on("/api/race/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning = false;
        JsonDocument doc;
        doc["type"] = "race_stop";
        String msg; serializeJson(doc, msg);
        wsText(msg);
        req->send(200, "application/json", R"({"ok":true})");
    });

    // ── GET /api/laps ──────────────────────────────────────────────────────
    server.on("/api/laps", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", lapsJson());
    });

    // ── GET /api/uid?phrase=... ────────────────────────────────────────────
    // Compute ELRS UID (SHA-256 of bind phrase, first 6 bytes) server-side.
    // Avoids Web Crypto API which requires HTTPS (secure context).
    server.on("/api/uid", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("phrase")) {
            req->send(400, "text/plain", "missing phrase");
            return;
        }
        String phrase = req->getParam("phrase")->value();
        uint8_t hash[32];
        mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(phrase.c_str()),
            phrase.length(), hash, 0);
        char uid[18];
        snprintf(uid, sizeof(uid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
        req->send(200, "text/plain", uid);
    });

    // ── GET /api/status ───────────────────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["raceRunning"] = raceRunning;
        doc["raceStartMs"] = raceStartMs;
        doc["lapCount"]    = lapCount;
        doc["uptime"]      = millis();
        String s; serializeJson(doc, s);
        req->send(200, "application/json", s);
    });

    // ── Static files from LittleFS ─────────────────────────────────────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.begin();
    Serial.println("[Web] HTTP server started");
}

// ── Loop ───────────────────────────────────────────────────────────────────
static String uartBuf;

void loop() {
    ws.cleanupClients();

    // Read UART from Gate Node line-by-line
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            uartBuf.trim();
            if (uartBuf.length()) {
                processGateLine(uartBuf);
                uartBuf = "";
            }
        } else if (c != '\r') {
            uartBuf += c;
            if (uartBuf.length() > 512) uartBuf = "";  // guard overflow
        }
    }
}
