/*
 * ELRS Backpack Lap Timer — Web Node
 * Hardware : XIAO ESP32-S3-B
 *
 * Pilot model
 *   - Roster: up to MAX_REGISTERED (50) pilots stored in NVS
 *     Each has a name + yomi (reading) + aircraft MAC (XIAO ESP32-C3 hardware MAC)
 *   - Active: up to MAX_ACTIVE (4) pilots selected from the roster for a race
 *     Only active pilots are sent to the Gate Node; gate slot 0-3 maps to them
 *
 * REST API
 *   GET/POST        /api/pilots          roster CRUD  {id?,name,uid,yomi}
 *   POST            /api/pilots/delete   {id}
 *   GET/POST        /api/active          active slot selection {slots:[idx,…]}
 *   GET/POST        /api/calib           per-roster-pilot thresholds {id,enter,exit}
 *   POST            /api/race/start|stop
 *   GET             /api/laps
 *   GET             /api/scan
 *   POST            /api/scan/clear
 *   GET             /api/status
 *   GET             /api/sd/status
 *   POST            /api/sd/pilots/backup
 *   POST            /api/sd/pilots/restore
 *
 * NVS layout ("pilots" namespace)
 *   "ver"       int   = 3 (format version)
 *   "count"     int   = number of registered pilots
 *   "p%d_name"  str   pilot name
 *   "p%d_yomi"  str   pilot name reading (pronunciation for TTS)
 *   "p%d_uid"   str   "AA:BB:CC:DD:EE:FF" or ""
 *   "p%d_enter" int   Enter RSSI threshold (dBm)
 *   "p%d_exit"  int   Exit  RSSI threshold (dBm)
 *
 * NVS layout ("active" namespace)
 *   "a0".."a3"  int   roster index for each race slot (-1 = empty)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ── Network ────────────────────────────────────────────────────────────────
static const char*     AP_SSID    = "ESP-NOW-LT";
static const char*     AP_PASS    = "esp-now-lt";
static const IPAddress AP_IP      (20, 0, 0, 1);
static const IPAddress AP_GATEWAY (20, 0, 0, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);

// ── UART ↔ Gate Node ──────────────────────────────────────────────────────
#define GATE_RX_PIN   3
#define GATE_TX_PIN   2
#define GATE_BAUD     115200

// ── Pilot limits ───────────────────────────────────────────────────────────
// NVS budget: ~224 bytes/pilot × 50 = 11,200 bytes ≈ 56% of 20 KB NVS → safe
#define MAX_REGISTERED  50
#define MAX_ACTIVE       4
#define MAX_LAPS       200
#define DEFAULT_ENTER  (-80)
#define DEFAULT_EXIT   (-90)

// ── Data structures ────────────────────────────────────────────────────────
struct PilotRoster {
    char    name[32];
    char    yomi[32];   // 読み方 — TTS pronunciation text (optional)
    uint8_t uid[6];
    bool    hasUid;
};

struct CalibConfig {
    int enterRssi;
    int exitRssi;
};

struct LapRecord {
    int      slot;
    int      rosterIdx;
    uint32_t lapTimeMs;
    uint32_t timestamp;
};

struct SlotRuntime {
    int      rssi;
    int      rawRssi;
    bool     crossing;
    uint32_t lastTs;
    uint32_t lapCount;
    uint32_t bestLapMs;
    uint32_t lastLapTs;
};

static PilotRoster  roster[MAX_REGISTERED];
static CalibConfig  rosterCal[MAX_REGISTERED];
static int          rosterCount    = 0;
static int          activePilots[MAX_ACTIVE];   // roster indices; -1 = empty slot
static SlotRuntime  rt[MAX_ACTIVE];
static LapRecord    laps[MAX_LAPS];
static int          lapCount    = 0;
static bool         raceRunning = false;
static uint32_t     raceStartMs = 0;

// ── SD card status (reported by gate node on boot) ─────────────────────────
static bool sdPresent = false;

// ── SD restore: accumulate sd_pilot_row messages before applying ──────────
static String restoreBuffer[MAX_REGISTERED];
static int    restoreCount = 0;

// ── Scan MAC list ──────────────────────────────────────────────────────────
#define MAX_SCAN_MACS 8
struct ScanMac { char mac[18]; int rssi; uint32_t ts; };
static ScanMac scanMacs[MAX_SCAN_MACS];
static int     scanMacCount = 0;

static void updateScanMac(const char* mac, int rssi) {
    for (int i = 0; i < scanMacCount; i++) {
        if (strcmp(scanMacs[i].mac, mac) == 0) {
            scanMacs[i].rssi = rssi; scanMacs[i].ts = millis(); return;
        }
    }
    if (scanMacCount < MAX_SCAN_MACS) {
        strncpy(scanMacs[scanMacCount].mac, mac, 17);
        scanMacs[scanMacCount].mac[17] = '\0';
        scanMacs[scanMacCount].rssi    = rssi;
        scanMacs[scanMacCount].ts      = millis();
        scanMacCount++;
    }
}

// ── Server ─────────────────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;
Preferences    prefs;

// ── Helpers ────────────────────────────────────────────────────────────────
static void uidToStr(const uint8_t* uid, char* buf) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
}

static const char* activeName(int slot) {
    int ri = activePilots[slot];
    return (ri >= 0 && ri < rosterCount) ? roster[ri].name : "---";
}

// Find which slot (0-3) a roster pilot occupies, -1 if not active
static int activeSlotOf(int ri) {
    for (int s = 0; s < MAX_ACTIVE; s++) if (activePilots[s] == ri) return s;
    return -1;
}

// ── NVS: roster ────────────────────────────────────────────────────────────
static void saveRosterPilot(int i) {
    if (i < 0 || i >= rosterCount) return;
    prefs.begin("pilots", false);
    char key[24];
    snprintf(key, sizeof(key), "p%d_name",  i); prefs.putString(key, roster[i].name);
    snprintf(key, sizeof(key), "p%d_yomi",  i); prefs.putString(key, roster[i].yomi);
    snprintf(key, sizeof(key), "p%d_uid",   i);
    if (roster[i].hasUid) {
        char u[18]; uidToStr(roster[i].uid, u); prefs.putString(key, u);
    } else { prefs.putString(key, ""); }
    snprintf(key, sizeof(key), "p%d_enter", i); prefs.putInt(key, rosterCal[i].enterRssi);
    snprintf(key, sizeof(key), "p%d_exit",  i); prefs.putInt(key, rosterCal[i].exitRssi);
    prefs.putInt("count", rosterCount);
    prefs.putInt("ver", 3);
    prefs.end();
}

static void saveRosterCount() {
    prefs.begin("pilots", false);
    prefs.putInt("count", rosterCount);
    prefs.putInt("ver", 3);
    prefs.end();
}

static void loadRosterConfig() {
    prefs.begin("pilots", true);
    rosterCount = prefs.getInt("count", 0);
    if (rosterCount > MAX_REGISTERED) rosterCount = MAX_REGISTERED;
    for (int i = 0; i < rosterCount; i++) {
        char key[24];
        snprintf(key, sizeof(key), "p%d_name", i);
        String n = prefs.getString(key, "");
        if (n.length()) strncpy(roster[i].name, n.c_str(), sizeof(roster[i].name)-1);
        else            snprintf(roster[i].name, sizeof(roster[i].name), "Pilot %d", i+1);

        snprintf(key, sizeof(key), "p%d_yomi", i);
        String y = prefs.getString(key, "");
        strncpy(roster[i].yomi, y.c_str(), sizeof(roster[i].yomi)-1);

        snprintf(key, sizeof(key), "p%d_uid", i);
        String u = prefs.getString(key, "");
        roster[i].hasUid = (u.length() == 17);
        if (roster[i].hasUid)
            sscanf(u.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                   &roster[i].uid[0], &roster[i].uid[1], &roster[i].uid[2],
                   &roster[i].uid[3], &roster[i].uid[4], &roster[i].uid[5]);

        snprintf(key, sizeof(key), "p%d_enter", i);
        rosterCal[i].enterRssi = prefs.getInt(key, DEFAULT_ENTER);
        snprintf(key, sizeof(key), "p%d_exit", i);
        rosterCal[i].exitRssi  = prefs.getInt(key, DEFAULT_EXIT);
    }
    prefs.end();
}

// ── NVS: active selection ──────────────────────────────────────────────────
static void saveActive() {
    prefs.begin("active", false);
    for (int s = 0; s < MAX_ACTIVE; s++) {
        char key[4]; snprintf(key, sizeof(key), "a%d", s);
        prefs.putInt(key, activePilots[s]);
    }
    prefs.end();
}

static void loadActiveConfig() {
    prefs.begin("active", true);
    for (int s = 0; s < MAX_ACTIVE; s++) {
        char key[4]; snprintf(key, sizeof(key), "a%d", s);
        int ri = prefs.getInt(key, -1);
        activePilots[s] = (ri >= 0 && ri < rosterCount) ? ri : -1;
    }
    prefs.end();
}

// ── Gate Node UART helpers ─────────────────────────────────────────────────
static void sendGateCmd(const char* action) {
    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"%s"})", action);
    Serial1.println(buf);
}

static void sendGatePilot(int slot) {
    int ri = activePilots[slot];
    char buf[96];
    if (ri < 0 || ri >= rosterCount || !roster[ri].hasUid) {
        snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":""})", slot);
        Serial1.println(buf);
        Serial.printf("[Web] → Gate slot%d cleared\n", slot);
    } else {
        char uid[18]; uidToStr(roster[ri].uid, uid);
        snprintf(buf, sizeof(buf),
                 R"({"type":"cmd","action":"set_pilot","pilot":%d,"uid":"%s"})", slot, uid);
        Serial1.println(buf);
        Serial.printf("[Web] → Gate slot%d = %s (%s)\n", slot, roster[ri].name, uid);
    }
    delay(30);
}

static void sendGateThreshold(int slot) {
    int ri = activePilots[slot];
    int enter = (ri >= 0 && ri < rosterCount) ? rosterCal[ri].enterRssi : DEFAULT_ENTER;
    int exit_ = (ri >= 0 && ri < rosterCount) ? rosterCal[ri].exitRssi  : DEFAULT_EXIT;
    char buf[96];
    snprintf(buf, sizeof(buf),
             R"({"type":"cmd","action":"set_threshold","pilot":%d,"enter":%d,"exit":%d})",
             slot, enter, exit_);
    Serial1.println(buf);
}

static void sendAllPilots() {
    for (int s = 0; s < MAX_ACTIVE; s++) { sendGatePilot(s); }
}

static void sendAllThresholds() {
    for (int s = 0; s < MAX_ACTIVE; s++) { sendGateThreshold(s); delay(30); }
}

// ── JSON builders ──────────────────────────────────────────────────────────
static String rosterJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < rosterCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]         = i;
        o["name"]       = roster[i].name;
        o["yomi"]       = roster[i].yomi;
        o["uid"]        = roster[i].hasUid ? [&](){
                            char u[18]; uidToStr(roster[i].uid, u); return String(u);
                          }() : String("");
        o["activeSlot"] = activeSlotOf(i);
        o["enter"]      = rosterCal[i].enterRssi;
        o["exit"]       = rosterCal[i].exitRssi;
    }
    String s; serializeJson(doc, s); return s;
}

static String activeJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int s = 0; s < MAX_ACTIVE; s++) {
        JsonObject o = arr.add<JsonObject>();
        int ri = activePilots[s];
        o["slot"]      = s;
        o["rosterIdx"] = ri;
        o["name"]      = activeName(s);
        o["yomi"]      = (ri >= 0 && ri < rosterCount) ? roster[ri].yomi : "";
        if (ri >= 0 && ri < rosterCount) {
            o["enter"] = rosterCal[ri].enterRssi;
            o["exit"]  = rosterCal[ri].exitRssi;
        }
    }
    String s; serializeJson(doc, s); return s;
}

static String lapsJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < lapCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        int ri = laps[i].rosterIdx;
        o["slot"]    = laps[i].slot;
        o["name"]    = (ri >= 0 && ri < rosterCount) ? roster[ri].name : "---";
        o["lapTime"] = laps[i].lapTimeMs;
        o["ts"]      = laps[i].timestamp;
    }
    String s; serializeJson(doc, s); return s;
}

static String scanJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < scanMacCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["mac"] = scanMacs[i].mac; o["rssi"] = scanMacs[i].rssi; o["ts"] = scanMacs[i].ts;
    }
    String s; serializeJson(doc, s); return s;
}

// ── WebSocket ──────────────────────────────────────────────────────────────
static void wsText(const String& msg) { ws.textAll(msg); }

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType evType, void*, uint8_t*, size_t) {
    if (evType != WS_EVT_CONNECT) return;
    // Send full state snapshot to newly connected client
    JsonDocument doc;
    doc["type"]        = "init";
    doc["raceRunning"] = raceRunning;
    doc["raceStartMs"] = raceStartMs;
    doc["sdPresent"]   = sdPresent;
    JsonArray pa = doc["pilots"].to<JsonArray>();
    for (int s = 0; s < MAX_ACTIVE; s++) {
        JsonObject o = pa.add<JsonObject>();
        int ri = activePilots[s];
        o["slot"]      = s;
        o["rosterIdx"] = ri;
        o["name"]      = activeName(s);
        o["yomi"]      = (ri >= 0 && ri < rosterCount) ? roster[ri].yomi : "";
        o["lapCount"]  = rt[s].lapCount;
        o["bestLap"]   = rt[s].bestLapMs;
        o["rssi"]      = rt[s].rssi;
        o["crossing"]  = rt[s].crossing;
        o["enter"]     = (ri >= 0) ? rosterCal[ri].enterRssi : DEFAULT_ENTER;
        o["exit"]      = (ri >= 0) ? rosterCal[ri].exitRssi  : DEFAULT_EXIT;
    }
    String msg; serializeJson(doc, msg); client->text(msg);
}

// ── Gate line processor ────────────────────────────────────────────────────
static void processGateLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char* type = doc["type"] | "";

    if (strcmp(type, "ready") == 0) {
        sendAllPilots(); sendAllThresholds();
        Serial.println("[Web] Gate ready — sent pilots + thresholds");
        return;
    }
    if (strcmp(type, "sd_status") == 0) {
        // Gate node reports SD card presence on boot
        sdPresent = doc["present"] | false;
        Serial.printf("[Web] SD status from gate: %s\n", sdPresent ? "present" : "absent");
        return;
    }
    if (strcmp(type, "scan") == 0) {
        const char* mac = doc["mac"] | "";
        if (strlen(mac) == 17) updateScanMac(mac, doc["rssi"] | -120);
        wsText(line); return;
    }
    if (strcmp(type, "rssi") == 0) {
        int s = doc["pilot"] | -1;
        if (s < 0 || s >= MAX_ACTIVE) return;
        rt[s].rssi     = doc["rssi"]     | -120;
        rt[s].rawRssi  = doc["raw"]      | -120;
        rt[s].crossing = doc["crossing"] | false;
        rt[s].lastTs   = doc["ts"]       | 0u;
        // Forward to WS clients with active pilot name
        JsonDocument wd;
        wd["type"]     = "rssi";
        wd["pilot"]    = s;
        wd["name"]     = activeName(s);
        wd["rssi"]     = rt[s].rssi;
        wd["raw"]      = rt[s].rawRssi;
        wd["crossing"] = rt[s].crossing;
        wd["ts"]       = rt[s].lastTs;
        String wm; serializeJson(wd, wm); wsText(wm);
        return;
    }
    if (strcmp(type, "lap") == 0) {
        // Item 6: ignore lap events when race is not running
        if (!raceRunning) return;

        int s = doc["pilot"] | -1;
        if (s < 0 || s >= MAX_ACTIVE) return;
        int ri = activePilots[s];
        uint32_t ts = doc["ts"] | 0u;

        uint32_t lapMs = 0;
        if (rt[s].lastLapTs > 0) lapMs = ts - rt[s].lastLapTs;
        rt[s].lastLapTs = ts;
        rt[s].lapCount++;

        bool newBest = false;
        if (lapMs > 0 && (rt[s].bestLapMs == 0 || lapMs < rt[s].bestLapMs)) {
            rt[s].bestLapMs = lapMs; newBest = true;
        }
        if (lapCount < MAX_LAPS) laps[lapCount++] = { s, ri, lapMs, ts };

        JsonDocument wd;
        wd["type"]     = "lap";
        wd["pilot"]    = s;
        wd["name"]     = activeName(s);
        wd["yomi"]     = (ri >= 0 && ri < rosterCount) ? roster[ri].yomi : "";
        wd["lapTime"]  = lapMs;
        wd["bestLap"]  = rt[s].bestLapMs;
        wd["lapCount"] = rt[s].lapCount;
        wd["newBest"]  = newBest;
        wd["rssi"]     = doc["rssi"] | -120;
        wd["ts"]       = ts;
        String wm; serializeJson(wd, wm); wsText(wm);
        return;
    }
    if (strcmp(type, "sd_pilot_row") == 0) {
        // Accumulate restore rows; applied when sd_restore_done arrives
        if (restoreCount < MAX_REGISTERED) {
            restoreBuffer[restoreCount++] = line;
        }
        return;
    }
    if (strcmp(type, "sd_restore_done") == 0) {
        // Apply accumulated pilot rows to roster and NVS
        Serial.printf("[Web] SD restore: applying %d pilots\n", restoreCount);
        rosterCount = 0;
        for (int i = 0; i < MAX_ACTIVE; i++) activePilots[i] = -1;
        for (int i = 0; i < restoreCount && i < MAX_REGISTERED; i++) {
            JsonDocument row;
            if (deserializeJson(row, restoreBuffer[i]) != DeserializationError::Ok) continue;
            int id = rosterCount++;
            strncpy(roster[id].name, row["name"] | "", sizeof(roster[id].name)-1);
            strncpy(roster[id].yomi, row["yomi"] | "", sizeof(roster[id].yomi)-1);
            const char* mac = row["mac"] | "";
            roster[id].hasUid = (strlen(mac) == 17);
            if (roster[id].hasUid)
                sscanf(mac, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                       &roster[id].uid[0], &roster[id].uid[1], &roster[id].uid[2],
                       &roster[id].uid[3], &roster[id].uid[4], &roster[id].uid[5]);
            rosterCal[id].enterRssi = row["enter"] | DEFAULT_ENTER;
            rosterCal[id].exitRssi  = row["exit"]  | DEFAULT_EXIT;
            saveRosterPilot(id);
        }
        saveRosterCount();
        saveActive();
        restoreCount = 0;

        // Broadcast restored roster to all WS clients
        JsonDocument wd;
        wd["type"]    = "sd_restore_done";
        wd["pilots"]  = serialized(rosterJson());
        String wm; serializeJson(wd, wm); wsText(wm);
        Serial.println("[Web] SD restore applied to roster");
        return;
    }
}

// ── POST body accumulator ──────────────────────────────────────────────────
struct BodyBuf { char* buf; size_t total; };
static void handleBody(AsyncWebServerRequest* req,
                       uint8_t* data, size_t len, size_t index, size_t total,
                       std::function<void(AsyncWebServerRequest*, const char*)> cb) {
    if (index == 0) req->_tempObject = new BodyBuf{ new char[total+1], total };
    auto* bb = reinterpret_cast<BodyBuf*>(req->_tempObject);
    if (!bb) return;
    memcpy(bb->buf + index, data, len);
    if (index + len == total) {
        bb->buf[total] = '\0';
        cb(req, bb->buf);
        delete[] bb->buf; delete bb; req->_tempObject = nullptr;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Web] ELRS Lap Timer — Web Node");

    Serial1.begin(GATE_BAUD, SERIAL_8N1, GATE_RX_PIN, GATE_TX_PIN);

    for (int s = 0; s < MAX_ACTIVE; s++) activePilots[s] = -1;
    memset(rt, 0, sizeof(rt));
    loadRosterConfig();
    loadActiveConfig();

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASS, 6);
    Serial.printf("[Web] AP  SSID=%s  IP=%s\n", AP_SSID, AP_IP.toString().c_str());

    // Captive portal: resolve every DNS query to our IP
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", AP_IP);

    if (!LittleFS.begin(true)) Serial.println("[Web] LittleFS failed");

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── GET /api/pilots ────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", rosterJson());
    });

    // ── POST /api/pilots  {id?:-1, name, yomi, uid} ───────────────────────
    server.on("/api/pilots", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;   // -1 = new pilot
                    if (id < -1 || id >= rosterCount)
                        { req2->send(400,"application/json",R"({"error":"bad id"})"); return; }
                    if (id == -1) {
                        if (rosterCount >= MAX_REGISTERED)
                            { req2->send(400,"application/json",R"({"error":"roster full"})"); return; }
                        id = rosterCount++;
                        rosterCal[id].enterRssi = DEFAULT_ENTER;
                        rosterCal[id].exitRssi  = DEFAULT_EXIT;
                    }
                    strncpy(roster[id].name, doc["name"] | "", sizeof(roster[id].name)-1);
                    strncpy(roster[id].yomi, doc["yomi"] | "", sizeof(roster[id].yomi)-1);
                    const char* uid = doc["uid"] | "";
                    roster[id].hasUid = (strlen(uid) == 17);
                    if (roster[id].hasUid)
                        sscanf(uid, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                               &roster[id].uid[0], &roster[id].uid[1], &roster[id].uid[2],
                               &roster[id].uid[3], &roster[id].uid[4], &roster[id].uid[5]);
                    saveRosterPilot(id);
                    // If this pilot is active, sync gate
                    int slot = activeSlotOf(id);
                    if (slot >= 0) { sendGatePilot(slot); sendGateThreshold(slot); }
                    JsonDocument resp; resp["ok"]=true; resp["id"]=id;
                    String s; serializeJson(resp,s); req2->send(200,"application/json",s);
                });
        });

    // ── POST /api/pilots/delete  {id} ─────────────────────────────────────
    server.on("/api/pilots/delete", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;
                    if (id < 0 || id >= rosterCount)
                        { req2->send(400,"application/json",R"({"error":"bad id"})"); return; }
                    // Update active slots before shifting
                    for (int s = 0; s < MAX_ACTIVE; s++) {
                        if (activePilots[s] == id) activePilots[s] = -1;
                        else if (activePilots[s] > id) activePilots[s]--;
                    }
                    // Shift roster + calib
                    for (int i = id; i < rosterCount-1; i++) {
                        roster[i]    = roster[i+1];
                        rosterCal[i] = rosterCal[i+1];
                    }
                    rosterCount--;
                    // Rewrite NVS: save all remaining pilots + new count
                    prefs.begin("pilots", false);
                    prefs.putInt("count", rosterCount);
                    prefs.putInt("ver", 3);
                    for (int i = id; i < rosterCount; i++) {
                        char key[24];
                        snprintf(key,sizeof(key),"p%d_name", i);  prefs.putString(key, roster[i].name);
                        snprintf(key,sizeof(key),"p%d_yomi", i);  prefs.putString(key, roster[i].yomi);
                        snprintf(key,sizeof(key),"p%d_uid",  i);
                        if (roster[i].hasUid) {
                            char u[18]; uidToStr(roster[i].uid,u); prefs.putString(key,u);
                        } else { prefs.putString(key,""); }
                        snprintf(key,sizeof(key),"p%d_enter",i);  prefs.putInt(key, rosterCal[i].enterRssi);
                        snprintf(key,sizeof(key),"p%d_exit", i);  prefs.putInt(key, rosterCal[i].exitRssi);
                    }
                    // Clear the last shifted entry from NVS
                    char key[24];
                    snprintf(key,sizeof(key),"p%d_name", rosterCount);  prefs.remove(key);
                    snprintf(key,sizeof(key),"p%d_yomi", rosterCount);  prefs.remove(key);
                    snprintf(key,sizeof(key),"p%d_uid",  rosterCount);  prefs.remove(key);
                    snprintf(key,sizeof(key),"p%d_enter",rosterCount);  prefs.remove(key);
                    snprintf(key,sizeof(key),"p%d_exit", rosterCount);  prefs.remove(key);
                    prefs.end();
                    saveActive();
                    sendAllPilots();
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── GET /api/active ────────────────────────────────────────────────────
    server.on("/api/active", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", activeJson());
    });

    // ── POST /api/active  {slots:[idx0,idx1,idx2,idx3]} ───────────────────
    server.on("/api/active", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    JsonArray slots = doc["slots"].as<JsonArray>();
                    if (!slots) { req2->send(400,"application/json",R"({"error":"no slots"})"); return; }
                    for (int s = 0; s < MAX_ACTIVE && s < (int)slots.size(); s++) {
                        int ri = slots[s].as<int>();
                        activePilots[s] = (ri >= 0 && ri < rosterCount) ? ri : -1;
                    }
                    saveActive();
                    sendAllPilots();
                    sendAllThresholds();
                    // Notify all WS clients of new active selection
                    JsonDocument wd; wd["type"]="active_update";
                    JsonArray pa = wd["pilots"].to<JsonArray>();
                    for (int s=0;s<MAX_ACTIVE;s++){
                        JsonObject o=pa.add<JsonObject>();
                        int ri=activePilots[s];
                        o["slot"]=s; o["rosterIdx"]=ri; o["name"]=activeName(s);
                        o["yomi"]=(ri>=0&&ri<rosterCount)?roster[ri].yomi:"";
                        if(ri>=0){o["enter"]=rosterCal[ri].enterRssi; o["exit"]=rosterCal[ri].exitRssi;}
                    }
                    String wm; serializeJson(wd,wm); wsText(wm);
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── POST /api/calib  {id, enter, exit} ────────────────────────────────
    server.on("/api/calib", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;
                    if (id < 0 || id >= rosterCount)
                        { req2->send(400,"application/json",R"({"error":"bad id"})"); return; }
                    rosterCal[id].enterRssi = doc["enter"] | rosterCal[id].enterRssi;
                    rosterCal[id].exitRssi  = doc["exit"]  | rosterCal[id].exitRssi;
                    saveRosterPilot(id);
                    int slot = activeSlotOf(id);
                    if (slot >= 0) sendGateThreshold(slot);
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── POST /api/race/start ───────────────────────────────────────────────
    server.on("/api/race/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning = true; raceStartMs = millis(); lapCount = 0;
        for (int s = 0; s < MAX_ACTIVE; s++) {
            rt[s].lapCount=0; rt[s].bestLapMs=0; rt[s].lastLapTs=0;
        }
        sendAllPilots();
        sendGateCmd("race_start");
        JsonDocument doc; doc["type"]="race_start"; doc["ts"]=raceStartMs;
        String msg; serializeJson(doc,msg); wsText(msg);
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── POST /api/race/stop ────────────────────────────────────────────────
    server.on("/api/race/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning = false;
        JsonDocument doc; doc["type"]="race_stop";
        String msg; serializeJson(doc,msg); wsText(msg);
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── GET /api/laps ──────────────────────────────────────────────────────
    server.on("/api/laps", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", lapsJson());
    });

    // ── GET /api/scan / POST /api/scan/clear ──────────────────────────────
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", scanJson());
    });
    server.on("/api/scan/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        scanMacCount = 0; req->send(200,"application/json",R"({"ok":true})");
    });

    // ── GET /api/status ────────────────────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["raceRunning"] = raceRunning; doc["raceStartMs"] = raceStartMs;
        doc["lapCount"]    = lapCount;    doc["rosterCount"]  = rosterCount;
        doc["uptime"]      = millis();    doc["sdPresent"]    = sdPresent;
        String s; serializeJson(doc,s); req->send(200,"application/json",s);
    });

    // ── GET /api/sd/status ─────────────────────────────────────────────────
    server.on("/api/sd/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc; doc["present"] = sdPresent;
        String s; serializeJson(doc,s); req->send(200,"application/json",s);
    });

    // ── POST /api/sd/pilots/backup — send all roster pilots to SD via gate ─
    server.on("/api/sd/pilots/backup", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!sdPresent) {
            req->send(503,"application/json",R"({"error":"no sd card"})");
            return;
        }
        sendGateCmd("sd_begin_backup");
        delay(20);
        for (int i = 0; i < rosterCount; i++) {
            JsonDocument row;
            row["type"]  = "cmd";
            row["action"]= "sd_backup_row";
            row["name"]  = roster[i].name;
            row["yomi"]  = roster[i].yomi;
            char u[18];
            if (roster[i].hasUid) uidToStr(roster[i].uid, u);
            else u[0] = '\0';
            row["mac"]   = u;
            row["enter"] = rosterCal[i].enterRssi;
            row["exit"]  = rosterCal[i].exitRssi;
            serializeJson(row, Serial1); Serial1.print('\n');
            delay(10);
        }
        sendGateCmd("sd_end_backup");
        req->send(200,"application/json",R"({"ok":true})");
        Serial.printf("[Web] SD backup: sent %d pilots\n", rosterCount);
    });

    // ── POST /api/sd/pilots/restore — request gate to stream /pilots.json ──
    server.on("/api/sd/pilots/restore", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!sdPresent) {
            req->send(503,"application/json",R"({"error":"no sd card"})");
            return;
        }
        restoreCount = 0;  // reset accumulator; gate will send sd_pilot_row × N then sd_restore_done
        sendGateCmd("sd_restore_request");
        req->send(200,"application/json",R"({"ok":true})");
        Serial.println("[Web] SD restore: request sent to gate");
    });

    // ── Captive portal detection probes ────────────────────────────────────
    auto cpRedirect = [](AsyncWebServerRequest* req) {
        req->redirect("http://20.0.0.1/");
    };
    server.on("/generate_204",              HTTP_GET, cpRedirect);
    server.on("/gen_204",                   HTTP_GET, cpRedirect);
    server.on("/hotspot-detect.html",       HTTP_GET, cpRedirect);
    server.on("/library/test/success.html", HTTP_GET, cpRedirect);
    server.on("/bag",                       HTTP_GET, cpRedirect);
    server.on("/connecttest.txt",           HTTP_GET, cpRedirect);
    server.on("/ncsi.txt",                  HTTP_GET, cpRedirect);
    server.on("/redirect",                  HTTP_GET, cpRedirect);
    server.on("/success.txt",               HTTP_GET, cpRedirect);
    server.on("/canonical.html",            HTTP_GET, cpRedirect);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Catch-all: redirect unknown paths; API 404 returns JSON
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->url().startsWith("/api"))
            req->send(404, "application/json", R"({"error":"not found"})");
        else
            req->redirect("http://20.0.0.1/");
    });

    server.begin();
    Serial.printf("[Web] HTTP started  roster=%d/%d  active=%d,%d,%d,%d\n",
                  rosterCount, MAX_REGISTERED,
                  activePilots[0], activePilots[1], activePilots[2], activePilots[3]);

    delay(500);
    sendAllPilots();
    sendAllThresholds();
}

// ── Loop ───────────────────────────────────────────────────────────────────
static String uartBuf;
void loop() {
    dnsServer.processNextRequest();
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
