#pragma once

// UART pins to Web Node
#define WEB_NODE_TX_PIN   26
#define WEB_NODE_RX_PIN   25
#define DEBUG_BAUD        115200
#define UART_BAUD         115200

// SD SPI Pins — LilyGo TTGO T8 V1.8
#define SD_CS_PIN    13
#define SD_MOSI_PIN  15
#define SD_MISO_PIN   2
#define SD_SCK_PIN   14

// WiFi promiscuous channel (must match aircraft node)
#define ESPNOW_CHANNEL    1

// Pilot detection
#define MAX_PILOTS         4
#define EMA_ALPHA          0.3f
#define DEFAULT_ENTRY_THR  (-80)
#define DEFAULT_EXIT_THR   (-90)
#define COOLDOWN_MS        3000UL
#define RSSI_INTERVAL_MS   50UL

// MAC scan
#define MAX_SCAN_MACS    8
#define SCAN_INTERVAL_MS 2000UL
