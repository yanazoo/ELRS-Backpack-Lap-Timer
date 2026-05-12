/*
 * ELRS Backpack Lap Timer — Web Node
 * Hardware : XIAO ESP32-S3-B
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "data_model.h"
#include "nvs_store.h"
#include "gate_comm.h"
#include "ws_handler.h"
#include "http_routes.h"

static String uartBuf;

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

    // DNS: wildcard → AP IP (required for captive portal on all platforms)
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(30);
    dnsServer.start(53, "*", AP_IP);

    if (!LittleFS.begin(true)) Serial.println("[Web] LittleFS failed");

    // CORS headers for API access from browser
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin",  "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    initWsHandler();
    registerHttpRoutes();
    server.begin();

    Serial.printf("[Web] HTTP started  roster=%d/%d  active=%d,%d,%d,%d\n",
                  rosterCount, MAX_REGISTERED,
                  activePilots[0], activePilots[1], activePilots[2], activePilots[3]);

    delay(500);
    sendAllPilots();
    sendAllThresholds();
}

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
