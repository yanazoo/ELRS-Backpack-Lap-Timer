/*
 * ELRS Backpack Lap Timer — Aircraft Node
 * Hardware : XIAO ESP32-C3 / XIAO ESP32-C6
 *
 * Role
 *   Broadcasts a tiny ESP-NOW beacon every BROADCAST_MS on ESPNOW_CHANNEL.
 *   The gate node (ESP32-WROVER-E) sniffs these frames via promiscuous mode
 *   and uses the WiFi RSSI to detect gate crossings.
 *
 *   Source MAC = hardware MAC of this board (unique per unit).
 *   Print the MAC on first boot and register it in the Web UI Config tab.
 *
 * Wiring
 *   5V  → aircraft receiver / VTX 5V pad
 *   GND → aircraft GND
 *   (no signal wires needed)
 *
 * Note for ESP32-C6 (XIAO ESP32-C6):
 *   The C6 defaults to 802.11ax (WiFi 6). ESP-NOW requires legacy
 *   802.11b/g/n, so we force the protocol after WiFi init.
 */

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

#define ESPNOW_CHANNEL   1
#define BROADCAST_MS   100

static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void setup() {
    Serial.begin(115200);
    delay(300);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

#ifdef CONFIG_IDF_TARGET_ESP32C6
    // C6 defaults to 802.11ax (WiFi 6); ESP-NOW needs legacy 802.11b/g/n
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
#endif

    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[Aircraft] ESP-NOW init failed — restarting");
        delay(1000);
        ESP.restart();
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcast, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.printf("[Aircraft] MAC     : %s\n", WiFi.macAddress().c_str());
    Serial.printf("[Aircraft] Channel : %d\n", ESPNOW_CHANNEL);
    Serial.printf("[Aircraft] Interval: %d ms\n", BROADCAST_MS);
    Serial.println("[Aircraft] Broadcasting — register this MAC in Web UI Config tab");
}

void loop() {
    static const uint8_t payload = 0xBC;   // arbitrary marker byte
    esp_now_send(kBroadcast, &payload, sizeof(payload));
    delay(BROADCAST_MS);
}
