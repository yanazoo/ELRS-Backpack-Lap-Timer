#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define AP_SSID        "ELRS-Logger"
#define AP_CHANNEL     1
#define MAX_PILOTS     4
#define HISTORY_SIZE   500
#define WS_INTERVAL_MS 100

// ── UART from Reader ──────────────────────────────────────────
// フレーム: [0xAA][MAC 6][RSSI uint8][XOR校験][0x55] = 10 bytes
#define UART_RX_PIN  16     // ← Reader GPIO17
#define UART_BAUD    115200
#define FRAME_LEN    10
#define FRAME_START  0xAA
#define FRAME_END    0x55

struct Pilot {
    uint8_t  mac[6];
    uint8_t  uid[6];     // ELRSバインドUID (ホワイトリスト用)
    bool     uidSet;     // uid が設定済みかどうか
    char     name[32];
    int8_t   rssi;
    uint32_t lastSeen;
    bool     active;
    bool     crossing;
    int8_t   peakRssi;
    uint32_t peakTs;
    int8_t   enterAt;
    int8_t   exitAt;
};

struct Record {
    uint32_t ts;
    uint8_t  pilot;
    int8_t   rssi;
};

static Pilot        g_pilots[MAX_PILOTS];
static uint8_t      g_nPilots  = 0;
static Record       g_hist[HISTORY_SIZE];
static uint16_t     g_histHead  = 0;
static uint16_t     g_histCnt   = 0;
static uint16_t     g_minLapMs  = 3000;
static portMUX_TYPE g_mux       = portMUX_INITIALIZER_UNLOCKED;

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

static int findPilot(const uint8_t* mac) {
    for (int i = 0; i < MAX_PILOTS; i++)
        if (g_pilots[i].active && memcmp(g_pilots[i].mac, mac, 6) == 0) return i;
    return -1;
}

static bool parseMAC(const char* str, uint8_t* out) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &out[0], &out[1], &out[2], &out[3], &out[4], &out[5]) == 6;
}

// ── UARTフレーム処理 (onPromiscuousの代替) ────────────────────
static void processFrame(const uint8_t* frame) {
    const uint8_t* mac  = frame + 1;
    const int8_t   rssi = static_cast<int8_t>(frame[7]);
    const uint32_t now  = millis();

    portENTER_CRITICAL(&g_mux);
    int idx = findPilot(mac);

    if (idx < 0) {
        // UID ホワイトリスト照合
        bool anyUid = false;
        for (int i = 0; i < MAX_PILOTS; i++)
            if (g_pilots[i].uidSet) { anyUid = true; break; }

        if (anyUid) {
            // UID設定あり → 一致するスロットにのみ割り当て
            for (int i = 0; i < MAX_PILOTS; i++) {
                if (g_pilots[i].uidSet && memcmp(g_pilots[i].uid, mac, 6) == 0) {
                    memcpy(g_pilots[i].mac, mac, 6);
                    g_pilots[i].active = true;
                    if (i >= g_nPilots) g_nPilots = i + 1;
                    idx = i;
                    Serial.printf("[UID] P%d matched %02X:%02X:%02X:%02X:%02X:%02X\n",
                                  i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    break;
                }
            }
            // 未登録MACは無視
        } else {
            // UID未設定 → 先着順で自動割り当て
            if (g_nPilots < MAX_PILOTS) {
                idx = g_nPilots++;
                memcpy(g_pilots[idx].mac, mac, 6);
                snprintf(g_pilots[idx].name, sizeof(g_pilots[idx].name), "Pilot %d", idx + 1);
                g_pilots[idx].active   = true;
                g_pilots[idx].crossing = false;
                g_pilots[idx].peakRssi = -127;
                g_pilots[idx].enterAt  = -80;
                g_pilots[idx].exitAt   = -90;
            }
        }
    }
    if (idx >= 0) {
        g_pilots[idx].rssi     = rssi;
        g_pilots[idx].lastSeen = now;

        // RotorHazard 状態機械
        if (!g_pilots[idx].crossing) {
            if (rssi > g_pilots[idx].enterAt) {
                g_pilots[idx].crossing = true;
                g_pilots[idx].peakRssi = rssi;
                g_pilots[idx].peakTs   = now;
            }
        } else {
            if (rssi > g_pilots[idx].peakRssi) {
                g_pilots[idx].peakRssi = rssi;
                g_pilots[idx].peakTs   = now;
            }
            if (rssi < g_pilots[idx].exitAt) {
                g_pilots[idx].crossing = false;
            }
        }

        g_hist[g_histHead] = {now, static_cast<uint8_t>(idx), rssi};
        g_histHead         = (g_histHead + 1) % HISTORY_SIZE;
        if (g_histCnt < HISTORY_SIZE) g_histCnt++;
    }
    portEXIT_CRITICAL(&g_mux);
}

// ── UARTパーサー ──────────────────────────────────────────────
static uint8_t g_rxBuf[FRAME_LEN];
static uint8_t g_rxPos   = 0;
static bool    g_inFrame = false;

static void pollUart() {
    while (Serial2.available()) {
        const uint8_t b = static_cast<uint8_t>(Serial2.read());
        if (!g_inFrame) {
            if (b == FRAME_START) {
                g_rxBuf[0] = b;
                g_rxPos    = 1;
                g_inFrame  = true;
            }
        } else {
            g_rxBuf[g_rxPos++] = b;
            if (g_rxPos == FRAME_LEN) {
                g_inFrame = false;
                g_rxPos   = 0;
                if (g_rxBuf[FRAME_LEN - 1] == FRAME_END) {
                    uint8_t cs = 0;
                    for (int i = 1; i <= 7; i++) cs ^= g_rxBuf[i];
                    if (cs == g_rxBuf[8])
                        processFrame(g_rxBuf);
                    else
                        Serial.println("[WARN] UART checksum error");
                }
            }
        }
    }
}

struct PilotSnap { Pilot data[MAX_PILOTS]; uint8_t n; };

static PilotSnap snapPilots() {
    PilotSnap s{};
    portENTER_CRITICAL(&g_mux);
    s.n = MAX_PILOTS;   // 常に4スロット全て送出
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
        o["uid"]      = s.data[i].uidSet  ? macStr(s.data[i].uid)  : "";
        o["rssi"]     = s.data[i].rssi;
        o["ts"]       = s.data[i].lastSeen;
        o["crossing"] = s.data[i].crossing;
        o["enterAt"]  = s.data[i].enterAt;
        o["exitAt"]   = s.data[i].exitAt;
        o["active"]   = s.data[i].active;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static void loadPilotPrefs() {
    prefs.begin("pilots", true);

    // 全スロット初期化
    for (int i = 0; i < MAX_PILOTS; i++) {
        memset(g_pilots[i].mac, 0, 6);
        memset(g_pilots[i].uid, 0, 6);
        g_pilots[i].uidSet   = false;
        snprintf(g_pilots[i].name, sizeof(g_pilots[i].name), "Pilot %d", i + 1);
        g_pilots[i].rssi     = -100;
        g_pilots[i].active   = false;
        g_pilots[i].crossing = false;
        g_pilots[i].peakRssi = -127;
        g_pilots[i].enterAt  = -80;
        g_pilots[i].exitAt   = -90;
    }

    // UID設定を全スロットから読み込む (MAC到着前から設定可能)
    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.getBytes(k, g_pilots[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        g_pilots[i].uidSet = prefs.getBool(k, false);
    }

    // 過去に検出済みのパイロットデータを読み込む
    uint8_t n = min((unsigned)prefs.getUChar("n", 0), (unsigned)MAX_PILOTS);
    for (int i = 0; i < n; i++) {
        char k[12];
        snprintf(k, sizeof(k), "mac%d", i);
        prefs.getBytes(k, g_pilots[i].mac, 6);
        snprintf(k, sizeof(k), "name%d", i);
        prefs.getString(k, g_pilots[i].name, sizeof(g_pilots[i].name));
        snprintf(k, sizeof(k), "en%d", i);
        g_pilots[i].enterAt  = (int8_t)prefs.getChar(k, -80);
        snprintf(k, sizeof(k), "ex%d", i);
        g_pilots[i].exitAt   = (int8_t)prefs.getChar(k, -90);
        g_pilots[i].active   = true;
    }
    g_nPilots = n;

    prefs.end();
    Serial.printf("[Pilots] loaded %d active\n", g_nPilots);
}

static void savePilotPrefs(const PilotSnap& s) {
    prefs.begin("pilots", false);

    // アクティブパイロット数を記録
    uint8_t n = 0;
    for (int i = 0; i < MAX_PILOTS; i++)
        if (s.data[i].active) n = i + 1;
    prefs.putUChar("n", n);

    for (int i = 0; i < MAX_PILOTS; i++) {
        char k[12];
        // UID設定は全スロット常に保存
        snprintf(k, sizeof(k), "uid%d", i);
        prefs.putBytes(k, s.data[i].uid, 6);
        snprintf(k, sizeof(k), "us%d", i);
        prefs.putBool(k, s.data[i].uidSet);

        if (s.data[i].active) {
            snprintf(k, sizeof(k), "mac%d", i);
            prefs.putBytes(k, s.data[i].mac, 6);
            snprintf(k, sizeof(k), "name%d", i);
            prefs.putString(k, s.data[i].name);
            snprintf(k, sizeof(k), "en%d", i);
            prefs.putChar(k, s.data[i].enterAt);
            snprintf(k, sizeof(k), "ex%d", i);
            prefs.putChar(k, s.data[i].exitAt);
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

    Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, -1);  // RX only
    Serial.printf("UART RX on GPIO%d @ %d baud\n", UART_RX_PIN, UART_BAUD);

    if (!LittleFS.begin(true))
        Serial.println("[WARN] LittleFS mount failed");

    loadPilotPrefs();
    loadSettings();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, "", AP_CHANNEL);
    Serial.printf("AP: %s  CH: %d  IP: %s\n",
                  AP_SSID, AP_CHANNEL, WiFi.softAPIP().toString().c_str());

    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* c,
                  AwsEventType t, void*, uint8_t*, size_t) {
        if (t == WS_EVT_CONNECT) {
            Serial.printf("[WS] client %u connected\n", c->id());
            c->text(buildJson(snapPilots()));
        }
    });
    server.addHandler(&ws);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

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
            // 名前更新
            if (strlen(name) > 0)
                strncpy(g_pilots[id].name, name, sizeof(g_pilots[id].name) - 1);
            // UID更新: "AA:BB:CC:DD:EE:FF" 形式 or "" でクリア
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
            if (id < g_nPilots) {
                g_pilots[id].enterAt = en;
                g_pilots[id].exitAt  = ex;
            }
            portEXIT_CRITICAL(&g_mux);
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
    pollUart();
    ws.cleanupClients();
    const uint32_t now = millis();
    if (now - g_lastWs >= WS_INTERVAL_MS && ws.count() > 0) {
        g_lastWs = now;
        ws.textAll(buildJson(snapPilots()));
    }
}
