/*
 * ELRS Backpack Lap Timer — Web Node
 * Hardware : XIAO ESP32-S3-B
 *
 * Roles
 *   - WiFi AP  SSID="ELRS bp-LT"  PASS="elrsbp-lt"  IP=20.0.0.1
 *   - Serve index.html from LittleFS
 *   - WebSocket /ws — real-time push to browsers
 *   - REST API:
 *       GET/POST /api/pilots    pilot name + bind phrase + UID
 *       GET/POST /api/calib     per-pilot Enter/Exit RSSI thresholds
 *       POST     /api/race/start|stop
 *       GET      /api/laps
 *       GET      /api/uid?phrase=...   SHA-256 UID (mbedTLS)
 *       GET      /api/status
 *   - Receive JSON lines from Gate Node via Serial1 (RX=GPIO3/D2)
 *   - Send sync commands to Gate Node via Serial1  (TX=GPIO2/D1)
 *   - Store pilot & calib config in NVS (Preferences)
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
static const char*    AP_SSID    = "ELRS bp-LT";
static const char*    AP_PASS    = "elrsbp-lt";
static const IPAddress AP_IP      (20, 0, 0, 1);
static const IPAddress AP_GATEWAY (20, 0, 0, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);

// ── UART ↔ Gate Node ──────────────────────────────────────────────────────
#define GATE_RX_PIN   3
#define GATE_TX_PIN   2
#define GATE_BAUD     115200

// ── Data structures ────────────────────────────────────────────────────────
#define MAX_PILOTS  4
#define MAX_LAPS    200

struct PilotConfig {
    char    name[32];
    char    bindPhrase[64];
    uint8_t uid[6];
    bool    hasUid;
};

struct CalibConfig {
    int enterRssi;   // dBm
    int exitRssi;    // dBm
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
    uint32_t lastLapTs;
};

static PilotConfig  cfg[MAX_PILOTS];
static CalibConfig  cal[MAX_PILOTS];
static PilotRuntime rt[MAX_PILOTS];
static LapRecord    laps[MAX_LAPS];
static int          lapCount   = 0;
static bool         raceRunning = false;
static uint32_t     raceStartMs = 0;

// ── Server & WebSocket ─────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences    prefs;

// ── NVS: pilots ────────────────────────────────────────────────────────────
static void loadPilotConfig() {
    prefs.begin("pilots", true);
    for (int i = 0; i < MAX_PILOTS; i++) {
        char key[24];
        snprintf(key, sizeof(key), "p%d_name", i);
        String n = prefs.getString(key, "");
        if (n.length()) strncpy(cfg[i].name, n.c_str(), sizeof(cfg[i].name) - 1);
        else            snprintf(cfg[i].name, sizeof(cfg[i].name), "Pilot %d", i + 1);

        snprintf(key, sizeof(key), "p%d_phrase", i);
        strncpy(cfg[i].bindPhrase,
                prefs.getString(key, "").c_str(),
                sizeof(cfg[i].bindPhrase) - 1);

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

static void savePilot(int i) {
    prefs.begin("pilots", false);
    char key[24];
    snprintf(key, sizeof(key), "p%d_name",   i); prefs.putString(key, cfg[i].name);
    snprintf(key, sizeof(key), "p%d_phrase", i); prefs.putString(key, cfg[i].bindPhrase);
    snprintf(key, sizeof(key), "p%d_uid", i);
    if (cfg[i].hasUid) {
        char u[18];
        snprintf(u, sizeof(u), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg[i].uid[0], cfg[i].uid[1], cfg[i].uid[2],
                 cfg[i].uid[3], cfg[i].uid[4], cfg[i].uid[5]);
        prefs.putString(key, u);
    } else {
        prefs.putString(key, "");   // always overwrite to clear stale NVS data
    }
    prefs.end();
}

// ── NVS: calib ─────────────────────────────────────────────────────────────
static void loadCalibConfig() {
    prefs.begin("calib", true);
    for (int i = 0; i < MAX_PILOTS; i++) {
        char key[24];
        snprintf(key, sizeof(key), "p%d_enter", i);
        cal[i].enterRssi = prefs.getInt(key, -80);
        snprintf(key, sizeof(key), "p%d_exit", i);
        cal[i].exitRssi  = prefs.getInt(key, -90);
    }
    prefs.end();
}

static void saveCalib(int i) {
    prefs.begin("calib", false);
    char key[24];
    snprintf(key, sizeof(key), "p%d_enter", i); prefs.putInt(key, cal[i].enterRssi);
    snprintf(key, sizeof(key), "p%d_exit",  i); prefs.putInt(key, cal[i].exitRssi);
    prefs.end();
}

// ── Gate Node UART helpers ─────────────────────────────────────────────────
static void sendGateCmd(const char* action) {
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"%s"})", action);
    Serial1.println(buf);
}

static void sendGateThreshold(int i) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             R"({"type":"cmd","action":"set_threshold","pilot":%d,"enter":%d,"exit":%d})",
             i, cal[i].enterRssi, cal[i].exitRssi);
    Serial1.println(buf);
    Serial.printf("[Web] → Gate threshold p%d: enter=%d exit=%d\n",
                  i, cal[i].enterRssi, cal[i].exitRssi);
}

static void sendAllThresholds() {
    for (int i = 0; i < MAX_PILOTS; i++) { sendGateThreshold(i); delay(30); }
}

static void sendGatePilot(int i) {
    char buf[96];
    if (!cfg[i].hasUid) {
        snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":""})", i);
        Serial1.println(buf);
        Serial.printf("[Web] → Gate pilot p%d cleared\n", i);
    } else {
        char uid[18];
        snprintf(uid, sizeof(uid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg[i].uid[0], cfg[i].uid[1], cfg[i].uid[2],
                 cfg[i].uid[3], cfg[i].uid[4], cfg[i].uid[5]);
        snprintf(buf, sizeof(buf),
                 R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":"%s"})", i, uid);
        Serial1.println(buf);
        Serial.printf("[Web] → Gate pilot p%d UID=%s\n", i, uid);
    }
    delay(30);   // give Gate Node UART RX buffer time to drain between commands
}

static void sendAllPilots() {
    for (int i = 0; i < MAX_PILOTS; i++) sendGatePilot(i);
}

// ── JSON builders ──────────────────────────────────────────────────────────
static String pilotsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_PILOTS; i++) {
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
        } else { o["uid"] = ""; }
    }
    String s; serializeJson(doc, s); return s;
}

static String calibJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_PILOTS; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]    = i;
        o["enter"] = cal[i].enterRssi;
        o["exit"]  = cal[i].exitRssi;
    }
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

// ── WebSocket ──────────────────────────────────────────────────────────────
static void wsText(const String& msg) { ws.textAll(msg); }

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType evType, void*, uint8_t*, size_t) {
    if (evType != WS_EVT_CONNECT) return;

    JsonDocument doc;
    doc["type"]        = "init";
    doc["raceRunning"] = raceRunning;
    doc["raceStartMs"] = raceStartMs;
    JsonArray pa = doc["pilots"].to<JsonArray>();
    for (int i = 0; i < MAX_PILOTS; i++) {
        JsonObject o = pa.add<JsonObject>();
        o["id"]       = i;
        o["name"]     = cfg[i].name;
        o["lapCount"] = rt[i].lapCount;
        o["bestLap"]  = rt[i].bestLapMs;
        o["rssi"]     = rt[i].rssi;
        o["crossing"] = rt[i].crossing;
        o["enter"]    = cal[i].enterRssi;
        o["exit"]     = cal[i].exitRssi;
    }
    String msg; serializeJson(doc, msg);
    client->text(msg);
}

// ── Gate Node line processor ───────────────────────────────────────────────
static void processGateLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";

    if (strcmp(type, "ready") == 0) {
        // Gate node (re)booted — push all pilot UIDs then calibration thresholds
        sendAllPilots();
        sendAllThresholds();
        Serial.println("[Web] Gate node ready — sent pilots + thresholds");
        return;
    }

    if (strcmp(type, "rssi") == 0) {
        int idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        rt[idx].rssi     = doc["rssi"]     | -120;
        rt[idx].rawRssi  = doc["raw"]      | -120;
        rt[idx].crossing = doc["crossing"] | false;
        rt[idx].lastTs   = doc["ts"]       | 0u;
        wsText(line);
        return;
    }

    if (strcmp(type, "lap") == 0) {
        int      idx = doc["pilot"] | -1;
        if (idx < 0 || idx >= MAX_PILOTS) return;
        uint32_t ts  = doc["ts"] | 0u;

        uint32_t lapMs = 0;
        if (rt[idx].lastLapTs > 0 && raceRunning)
            lapMs = ts - rt[idx].lastLapTs;
        rt[idx].lastLapTs = ts;
        rt[idx].lapCount++;

        bool newBest = false;
        if (lapMs > 0 && (rt[idx].bestLapMs == 0 || lapMs < rt[idx].bestLapMs)) {
            rt[idx].bestLapMs = lapMs;
            newBest = true;
        }
        if (lapCount < MAX_LAPS) laps[lapCount++] = { idx, lapMs, ts };

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
        String wm; serializeJson(wd, wm);
        wsText(wm);
    }
}

// ── POST body accumulator ──────────────────────────────────────────────────
struct BodyBuf { char* buf; size_t total; };

static void handleBody(AsyncWebServerRequest* req,
                        uint8_t* data, size_t len, size_t index, size_t total,
                        std::function<void(AsyncWebServerRequest*, const char*)> cb) {
    if (index == 0) req->_tempObject = new BodyBuf{ new char[total + 1], total };
    auto* bb = reinterpret_cast<BodyBuf*>(req->_tempObject);
    if (!bb) return;
    memcpy(bb->buf + index, data, len);
    if (index + len == total) {
        bb->buf[total] = '\0';
        cb(req, bb->buf);
        delete[] bb->buf; delete bb;
        req->_tempObject = nullptr;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Web] ELRS Backpack Lap Timer — Web Node");

    Serial1.begin(GATE_BAUD, SERIAL_8N1, GATE_RX_PIN, GATE_TX_PIN);

    loadPilotConfig();
    loadCalibConfig();
    memset(rt, 0, sizeof(rt));

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    // Use ch6 for AP so it doesn't overlap with ESP-NOW on ch1
    WiFi.softAP(AP_SSID, AP_PASS, 6);
    Serial.printf("[Web] AP up  SSID=%s  IP=%s  ch=6\n", AP_SSID, AP_IP.toString().c_str());

    if (!LittleFS.begin(true))
        Serial.println("[Web] LittleFS mount failed — upload data/ first");

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── GET /api/pilots ────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", pilotsJson());
    });

    // ── POST /api/pilots ───────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handleBody(req, data, len, index, total,
                [](AsyncWebServerRequest* req2, const char* body) {
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400, "application/json", R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;
                    if (id < 0 || id >= MAX_PILOTS)
                        { req2->send(400, "application/json", R"({"error":"bad id"})"); return; }
                    strncpy(cfg[id].name,       doc["name"]       | "", sizeof(cfg[id].name)       - 1);
                    strncpy(cfg[id].bindPhrase, doc["bindPhrase"] | "", sizeof(cfg[id].bindPhrase) - 1);
                    const char* uid = doc["uid"] | "";
                    cfg[id].hasUid = (strlen(uid) == 17);
                    if (cfg[id].hasUid)
                        sscanf(uid, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                               &cfg[id].uid[0], &cfg[id].uid[1], &cfg[id].uid[2],
                               &cfg[id].uid[3], &cfg[id].uid[4], &cfg[id].uid[5]);
                    savePilot(id);
                    sendGatePilot(id);   // register UID on gate node immediately
                    req2->send(200, "application/json", R"({"ok":true})");
                });
        });

    // ── GET /api/calib ─────────────────────────────────────────────────────
    server.on("/api/calib", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", calibJson());
    });

    // ── POST /api/calib ────────────────────────────────────────────────────
    server.on("/api/calib", HTTP_POST,
        [](AsyncWebServerRequest*) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handleBody(req, data, len, index, total,
                [](AsyncWebServerRequest* req2, const char* body) {
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400, "application/json", R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;
                    if (id < 0 || id >= MAX_PILOTS)
                        { req2->send(400, "application/json", R"({"error":"bad id"})"); return; }
                    cal[id].enterRssi = doc["enter"] | cal[id].enterRssi;
                    cal[id].exitRssi  = doc["exit"]  | cal[id].exitRssi;
                    saveCalib(id);
                    sendGateThreshold(id);   // apply immediately to gate node
                    req2->send(200, "application/json", R"({"ok":true})");
                });
        });

    // ── POST /api/race/start ───────────────────────────────────────────────
    server.on("/api/race/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning = true;
        raceStartMs = millis();
        lapCount    = 0;
        for (int i = 0; i < MAX_PILOTS; i++) {
            rt[i].lapCount  = 0;
            rt[i].bestLapMs = 0;
            rt[i].lastLapTs = 0;
        }
        sendGateCmd("race_start");   // sync gate node state
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
    server.on("/api/uid", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("phrase")) { req->send(400, "text/plain", "missing phrase"); return; }
        String phrase = req->getParam("phrase")->value();
        uint8_t hash[32];
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(phrase.c_str()),
                       phrase.length(), hash, 0);
        char uid[18];
        snprintf(uid, sizeof(uid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
        req->send(200, "text/plain", uid);
    });

    // ── GET /api/status ────────────────────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["raceRunning"] = raceRunning;
        doc["raceStartMs"] = raceStartMs;
        doc["lapCount"]    = lapCount;
        doc["uptime"]      = millis();
        String s; serializeJson(doc, s);
        req->send(200, "application/json", s);
    });

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.begin();
    Serial.println("[Web] HTTP server started");

    // Proactively push pilots + thresholds on boot.
    // Gate Node may have already sent "ready" before our loop() started,
    // so we can't rely solely on the "ready" trigger.
    delay(500);   // brief wait for Gate Node UART to settle
    sendAllPilots();
    sendAllThresholds();
    Serial.println("[Web] Boot sync: sent pilots + thresholds to Gate Node");
}

// ── Loop ───────────────────────────────────────────────────────────────────
static String uartBuf;

void loop() {
    ws.cleanupClients();

    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            uartBuf.trim();
            if (uartBuf.length()) { processGateLine(uartBuf); uartBuf = ""; }
        } else if (c != '\r') {
            uartBuf += c;
            if (uartBuf.length() > 512) uartBuf = "";
        }
    }
}
