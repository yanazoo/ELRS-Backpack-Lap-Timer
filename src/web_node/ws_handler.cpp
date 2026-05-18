#include <Arduino.h>
#include <ArduinoJson.h>
#include "ws_handler.h"
#include "data_model.h"
#include "json_api.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer      dnsServer;

void wsText(const String& msg) { ws.textAll(msg); }
void wsText(const char* msg)   { ws.textAll(msg); }

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType evType, void*, uint8_t*, size_t) {
    if (evType != WS_EVT_CONNECT) return;
    JsonDocument doc;
    doc["type"]        = "init";
    doc["raceRunning"] = raceRunning;
    doc["racePaused"]  = (racePauseStartMs != 0);
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

void initWsHandler() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
}
