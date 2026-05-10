#include <Arduino.h>
#include <esp_wifi.h>
#include "promiscuous.h"
#include "config.h"

QueueHandle_t packetQueue;

static void IRAM_ATTR onPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const auto* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->payload[0] != 0xD0) return;  // Action frame only

    PacketInfo info;
    memcpy(info.mac, &pkt->payload[10], 6);
    info.rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    // Tag ESP-NOW frames (Vendor Specific 0x7F + Espressif OUI 18:FE:34 + type 0x04)
    // without hard-blocking — registered pilots are always processed regardless of tag.
    info.isEspNow = (pkt->rx_ctrl.sig_len >= 29 &&
                     pkt->payload[24] == 0x7F &&
                     pkt->payload[25] == 0x18 && pkt->payload[26] == 0xFE &&
                     pkt->payload[27] == 0x34 && pkt->payload[28] == 0x04);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(packetQueue, &info, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void setupPromiscuous() {
    packetQueue = xQueueCreate(64, sizeof(PacketInfo));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);

    Serial.printf("[Gate] Promiscuous mode on channel %d\n", ESPNOW_CHANNEL);
}
