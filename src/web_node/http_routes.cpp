#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "http_routes.h"
#include "data_model.h"
#include "json_api.h"
#include "gate_comm.h"
#include "nvs_store.h"
#include "ws_handler.h"

// Captive portal redirect — sent for all OS probe URLs
static void cpRedirect(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(302);
    res->addHeader("Location", "http://20.0.0.1/");
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

void registerHttpRoutes() {
    // ── Captive portal probes (registered FIRST so they take priority) ────────
    // Android (AOSP / Google)
    server.on("/generate_204",                           HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/gen_204",                                HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/connecttest.txt",                        HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    // iOS / macOS
    server.on("/hotspot-detect.html",                   HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/library/test/success.html",              HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/success.txt",                           HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/canonical.html",                        HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    // Windows (NCSI / WPAD)
    server.on("/ncsi.txt",                              HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/redirect",                              HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/fwlink",                                HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/wpad.dat",                              HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    // Misc
    server.on("/bag",                                   HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/kindle-wifi/wifistub.html",             HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    server.on("/chat",                                  HTTP_ANY, [](AsyncWebServerRequest* r){ cpRedirect(r); });
    // Noisy browser requests — suppress
    server.on("/favicon.ico",         HTTP_ANY, [](AsyncWebServerRequest* r){ r->send(204); });
    server.on("/apple-touch-icon.png",HTTP_ANY, [](AsyncWebServerRequest* r){ r->send(204); });
    server.on("/robots.txt",          HTTP_ANY, [](AsyncWebServerRequest* r){ r->send(204); });

    // ── GET /api/pilots ───────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", rosterJson());
    });

    // ── POST /api/pilots ──────────────────────────────────────────────────────
    server.on("/api/pilots", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    int id = doc["id"] | -1;
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
                    int slot = activeSlotOf(id);
                    if (slot >= 0) { sendGatePilot(slot); sendGateThreshold(slot); }
                    JsonDocument resp; resp["ok"]=true; resp["id"]=id;
                    String s; serializeJson(resp,s); req2->send(200,"application/json",s);
                });
        });

    // ── POST /api/pilots/delete ───────────────────────────────────────────────
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
                    for (int s = 0; s < MAX_ACTIVE; s++) {
                        if (activePilots[s] == id) activePilots[s] = -1;
                        else if (activePilots[s] > id) activePilots[s]--;
                    }
                    for (int i = id; i < rosterCount-1; i++) {
                        roster[i]    = roster[i+1];
                        rosterCal[i] = rosterCal[i+1];
                    }
                    rosterCount--;
                    nvsSaveRangeFromIndex(id);
                    saveActive();
                    sendAllPilots();
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── GET /api/active ───────────────────────────────────────────────────────
    server.on("/api/active", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", activeJson());
    });

    // ── POST /api/active ──────────────────────────────────────────────────────
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
                    JsonDocument wd; wd["type"]="active_update";
                    JsonArray pa = wd["pilots"].to<JsonArray>();
                    for (int s=0; s<MAX_ACTIVE; s++) {
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

    // ── POST /api/calib ───────────────────────────────────────────────────────
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

    // ── POST /api/race/start ──────────────────────────────────────────────────
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

    // ── POST /api/race/stop ───────────────────────────────────────────────────
    server.on("/api/race/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        raceRunning = false;
        JsonDocument doc; doc["type"]="race_stop";
        String msg; serializeJson(doc,msg); wsText(msg);
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── GET /api/laps ─────────────────────────────────────────────────────────
    server.on("/api/laps", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", lapsJson());
    });

    // ── GET /api/scan / POST /api/scan/clear / POST /api/scan/refresh ─────────
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", scanJson());
    });
    server.on("/api/scan/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        scanMacCount = 0; req->send(200,"application/json",R"({"ok":true})");
    });
    server.on("/api/scan/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
        sendGateCmd("scan_refresh");
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── GET /api/status ───────────────────────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["raceRunning"] = raceRunning; doc["raceStartMs"] = raceStartMs;
        doc["lapCount"]    = lapCount;    doc["rosterCount"]  = rosterCount;
        doc["uptime"]      = millis();    doc["sdPresent"]    = sdPresent;
        String s; serializeJson(doc,s); req->send(200,"application/json",s);
    });

    // ── GET /api/sd/status ────────────────────────────────────────────────────
    server.on("/api/sd/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc; doc["present"] = sdPresent;
        String s; serializeJson(doc,s); req->send(200,"application/json",s);
    });

    // ── POST /api/sd/pilots/backup ────────────────────────────────────────────
    server.on("/api/sd/pilots/backup", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!sdPresent) { req->send(503,"application/json",R"({"error":"no sd card"})"); return; }
        sendGateCmd("sd_begin_backup"); delay(20);
        for (int i = 0; i < rosterCount; i++) {
            JsonDocument row;
            row["type"]  = "cmd"; row["action"]= "sd_backup_row";
            row["name"]  = roster[i].name; row["yomi"]  = roster[i].yomi;
            char u[18];
            if (roster[i].hasUid) uidToStr(roster[i].uid, u); else u[0] = '\0';
            row["mac"]   = u;
            row["enter"] = rosterCal[i].enterRssi; row["exit"]  = rosterCal[i].exitRssi;
            serializeJson(row, Serial1); Serial1.print('\n'); delay(10);
        }
        sendGateCmd("sd_end_backup");
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── POST /api/sd/pilots/restore ───────────────────────────────────────────
    server.on("/api/sd/pilots/restore", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!sdPresent) { req->send(503,"application/json",R"({"error":"no sd card"})"); return; }
        restoreCount = 0;
        sendGateCmd("sd_restore_request");
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── POST /api/sd/files/list ───────────────────────────────────────────────
    server.on("/api/sd/files/list", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!sdPresent) { req->send(503,"application/json",R"({"error":"no sd card"})"); return; }
        sendGateCmd("sd_list_files");
        req->send(200,"application/json",R"({"ok":true})");
    });

    // ── POST /api/sd/files/download ───────────────────────────────────────────
    server.on("/api/sd/files/download", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    if (!sdPresent) { req2->send(503,"application/json",R"({"error":"no sd card"})"); return; }
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    const char* path = doc["path"] | "";
                    if (!strlen(path)) { req2->send(400,"application/json",R"({"error":"no path"})"); return; }
                    char buf[128];
                    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"sd_read_file","path":"%s"})", path);
                    Serial1.println(buf);
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── POST /api/sd/files/delete ─────────────────────────────────────────────
    server.on("/api/sd/files/delete", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    if (!sdPresent) { req2->send(503,"application/json",R"({"error":"no sd card"})"); return; }
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    const char* path = doc["path"] | "";
                    if (!strlen(path)) { req2->send(400,"application/json",R"({"error":"no path"})"); return; }
                    char buf[128];
                    snprintf(buf, sizeof(buf), R"({"type":"cmd","action":"sd_delete_file","path":"%s"})", path);
                    Serial1.println(buf);
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── GET/POST /api/settings ────────────────────────────────────────────────
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["lapMode"]    = lapMode;
        doc["cooldownMs"] = cooldownMs;
        String s; serializeJson(doc,s); req->send(200,"application/json",s);
    });
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest*){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            handleBody(req, data, len, idx, total,
                [](AsyncWebServerRequest* req2, const char* body){
                    JsonDocument doc;
                    if (deserializeJson(doc, body) != DeserializationError::Ok)
                        { req2->send(400,"application/json",R"({"error":"bad json"})"); return; }
                    if (!doc["lapMode"].isNull())    lapMode    = (uint8_t)(int)(doc["lapMode"]);
                    if (!doc["cooldownMs"].isNull()) {
                        cooldownMs = (uint32_t)(int)(doc["cooldownMs"]);
                        if (cooldownMs < 500)   cooldownMs = 500;
                        if (cooldownMs > 30000) cooldownMs = 30000;
                        sendGateCooldown();
                    }
                    req2->send(200,"application/json",R"({"ok":true})");
                });
        });

    // ── Static files (last — after all API and captive portal routes) ──────────
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ── 404 / captive portal catch-all ────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) { req->send(204); return; }
        if (req->url().startsWith("/api"))
            req->send(404,"application/json",R"({"error":"not found"})");
        else
            cpRedirect(req);
    });
}
