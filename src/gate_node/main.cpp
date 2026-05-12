/*
 * ELRS Backpack Lap Timer — Gate Node
 * Hardware : ESP32-WROVER-E-A (LilyGo TTGO T8 V1.8)
 *
 * UART protocol — Gate→Web (1 JSON line per event):
 *   {"type":"lap",       "pilot":0,"uid":"AA:BB","rssi":-72,"ts":123456}
 *   {"type":"rssi",      "pilot":0,"rssi":-85,"raw":-87,"crossing":false,"ts":123460}
 *   {"type":"ready",     "pilots":4}
 *   {"type":"sd_status", "present":true/false}
 *   {"type":"sd_pilot_row", <pilot fields>}
 *   {"type":"sd_restore_done"}
 *
 * UART protocol — Web→Gate:
 *   {"type":"cmd","action":"race_start"}
 *   {"type":"cmd","action":"set_pilot","pilot":0,"uid":"AA:BB:CC:DD:EE:FF"}
 *   {"type":"cmd","action":"set_threshold","pilot":0,"enter":-80,"exit":-90}
 *   {"type":"cmd","action":"scan_refresh"}
 *   {"type":"cmd","action":"sd_begin_backup"}
 *   {"type":"cmd","action":"sd_backup_row","name":"...","yomi":"...","mac":"...","enter":-80,"exit":-90}
 *   {"type":"cmd","action":"sd_end_backup"}
 *   {"type":"cmd","action":"sd_restore_request"}
 *   {"type":"cmd","action":"sd_list_files"}
 *   {"type":"cmd","action":"sd_read_file","path":"/race_001.csv"}
 *   {"type":"cmd","action":"sd_delete_file","path":"/race_001.csv"}
 *
 * Wiring:
 *   GPIO26 (TX1) → XIAO ESP32-S3 GPIO3 (RX1)
 *   GPIO25 (RX1) ← XIAO ESP32-S3 GPIO2 (TX1)
 *   SD: CS=13, MOSI=15, MISO=2, SCK=14
 */

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

    // Re-send "ready" + "sd_status" every 10 s until Web Node registers at least one pilot
    if (!anyPilotRegistered() && now - lastReadySend >= 10000UL) {
        lastReadySend = now;
        char buf[64];
        snprintf(buf, sizeof(buf), R"({"type":"ready","pilots":%d})", MAX_PILOTS);
        Serial1.println(buf);
        sdSendStatus();
        Serial.println("[Gate] Re-sent ready + sd_status (no pilots registered)");
    }

    // Read commands from Web Node
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

    // Drain ISR queue: registered pilots update rawRssi; unknown ESP-NOW MACs go to scan
    PacketInfo info;
    while (xQueueReceive(packetQueue, &info, 0) == pdTRUE) {
        int idx = findPilot(info.mac);
        if (idx >= 0) {
            pilots[idx].rawRssi = info.rssi;
        } else if (info.isEspNow) {
            reportScanMac(info.mac, info.rssi);
        }
    }

    // EMA filter + state machine — only for registered pilots
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
                    p.lastLapTime = now;
                    sendLap(i);
                }
                p.crossing = false;
            }
        }
    }

    // Periodic RSSI telemetry to Web Node
    if (now - lastRssiSend >= RSSI_INTERVAL_MS) {
        lastRssiSend = now;
        for (int i = 0; i < MAX_PILOTS; i++) {
            if (pilots[i].hasUid) sendRssi(i, now);
        }
    }

    delay(10);
}
