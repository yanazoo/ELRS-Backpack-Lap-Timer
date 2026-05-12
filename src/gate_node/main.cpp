#include <Arduino.h>
#include "config.h"
#include "pilots.h"
#include "promiscuous.h"
#include "sd_gate.h"
#include "uart_gate.h"

static String   webCmdBuf;
static uint32_t lastRssiSend  = 0;
static uint32_t lastReadySend = 0;

void setup() {
    Serial.begin(DEBUG_BAUD);
    Serial.println("\n[Gate] ELRS Backpack Lap Timer — Gate Node");

    Serial1.begin(UART_BAUD, SERIAL_8N1, WEB_NODE_RX_PIN, WEB_NODE_TX_PIN);

    initPilots();
    sdInit();
    setupPromiscuous();

    char buf[64];
    snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
    Serial1.println(buf);
    lastReadySend = millis();
}

void loop() {
    uint32_t now = millis();

    if (!anyPilotRegistered() && now - lastReadySend >= 10000UL) {
        lastReadySend = now;
        char buf[64];
        snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
        Serial1.println(buf);
        sdSendStatus();
        Serial.println("[Gate] Re-sent ready + sd_status (no pilots registered)");
    }

    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            webCmdBuf.trim();
            if (webCmdBuf.length()) { processWebCmd(webCmdBuf); webCmdBuf = ""; }
        } else if (c != '\r') {
            webCmdBuf += c;
            if (webCmdBuf.length() > 512) webCmdBuf = "";
        }
    }

    PacketInfo info;
    while (xQueueReceive(packetQueue, &info, 0) == pdTRUE) {
        int idx = findPilot(info.mac);
        if (idx >= 0) {
            pilots[idx].rawRssi = info.rssi;
        } else if (info.isEspNow) {
            reportScanMac(info.mac, info.rssi);
        }
    }

    for (int i = 0; i < MAX_PILOTS; i++) {
        if (!pilots[i].hasUid) continue;
        PilotState& p = pilots[i];
        p.emaRssi = EMA_ALPHA * p.rawRssi + (1.0f - EMA_ALPHA) * p.emaRssi;
        float ema = p.emaRssi;

        if (!p.crossing) {
            if (ema > p.entryThreshold) {
                p.crossing = true;
                p.peakRssi = (int)ema;
                p.peakTime = now;
            }
        } else {
            if ((int)ema > p.peakRssi) { p.peakRssi = (int)ema; p.peakTime = now; }
            if (ema < p.exitThreshold) {
                if (now - p.lastLapTime >= gCooldownMs) {
                    uint32_t lapMs = (p.lastPeakTime > 0) ? (p.peakTime - p.lastPeakTime) : 0;
                    p.lastPeakTime = p.peakTime;
                    p.lastLapTime  = now;
                    sendLap(i, lapMs);
                }
                p.crossing = false;
            }
        }
    }

    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        for (int i = 0; i < MAX_PILOTS; i++) {
            if (pilots[i].hasUid) sendRssi(i, now);
        }
    }

    delay(10);
}
