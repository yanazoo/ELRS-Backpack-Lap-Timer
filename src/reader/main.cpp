#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ── Config ────────────────────────────────────────────────────
#define LISTEN_CHANNEL  1       // ELRSバックパックのESP-NOWチャンネル
#define UART_TX_PIN     17      // → Server GPIO16
#define UART_BAUD       115200

// ── UARTフレーム ──────────────────────────────────────────────
// [0xAA][MAC 6][RSSI uint8][XOR校験][0x55] = 10 bytes
#define FRAME_LEN   10
#define FRAME_START 0xAA
#define FRAME_END   0x55

// ── ISR→loop FIFO ────────────────────────────────────────────
struct Pkt { uint8_t mac[6]; int8_t rssi; };
static Pkt          g_q[16];
static volatile int g_qH   = 0;
static volatile int g_qT   = 0;
static portMUX_TYPE g_qMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onPromiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const auto*    pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* p   = pkt->payload;
    const uint16_t len = pkt->rx_ctrl.sig_len;

    // 802.11 Action frame + Espressif ESP-NOW OUI
    if (len < 28 || p[0] != 0xD0) return;
    if (p[24] != 0x7F || p[25] != 0x18 || p[26] != 0xFE || p[27] != 0x34) return;

    const uint8_t* mac  = p + 10;
    const int8_t   rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);

    portENTER_CRITICAL_ISR(&g_qMux);
    const int next = (g_qH + 1) & 15;
    if (next != g_qT) {     // FIFO満杯でなければ積む
        memcpy(g_q[g_qH].mac, mac, 6);
        g_q[g_qH].rssi = rssi;
        g_qH = next;
    }
    portEXIT_CRITICAL_ISR(&g_qMux);
}

static void sendFrame(const uint8_t* mac, int8_t rssi) {
    uint8_t f[FRAME_LEN];
    f[0] = FRAME_START;
    memcpy(&f[1], mac, 6);
    f[7] = static_cast<uint8_t>(rssi);
    uint8_t cs = 0;
    for (int i = 1; i <= 7; i++) cs ^= f[i];
    f[8] = cs;
    f[9] = FRAME_END;
    Serial2.write(f, FRAME_LEN);
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(UART_BAUD, SERIAL_8N1, -1, UART_TX_PIN);  // TX only

    Serial.printf("\n=== ELRS Reader  ch.%d ===\n", LISTEN_CHANNEL);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);
    esp_wifi_set_channel(LISTEN_CHANNEL, WIFI_SECOND_CHAN_NONE);

    Serial.println("Listening for ESP-NOW frames...");
}

void loop() {
    // FIFOからUARTへ送出
    portENTER_CRITICAL(&g_qMux);
    int h = g_qH;
    portEXIT_CRITICAL(&g_qMux);

    int t = g_qT;
    while (t != h) {
        sendFrame(g_q[t].mac, g_q[t].rssi);
        t = (t + 1) & 15;
        portENTER_CRITICAL(&g_qMux);
        g_qT = t;
        h    = g_qH;
        portEXIT_CRITICAL(&g_qMux);
    }
}
