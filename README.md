# ELRS Backpack Lap Timer

ELRSのBackpack ESP-NOW通信を活用した、FPVドローンレース用ラップタイマー。

**特徴:**
- 機体側の改造不要（ELRS TX Backpack の Telemetry=ESP-NOW をONにするだけ）
- バインドフレーズ由来のUIDでパイロットを自動識別
- RSSIピーク検出 + RotorHazard準拠の状態機械でゲート通過を計測
- PhobosLT_4ch スタイルのWeb UI（GitHub Darkテーマ、日本語TTS、Canvas波形グラフ）

---

## RotorHazardとの比較

| 項目 | RotorHazard | 本システム |
|------|-------------|-----------|
| 検出方式 | 5.8GHz映像電波RSSI | 2.4GHz ELRS ESP-NOW RSSI |
| パイロット識別 | 映像送信機の周波数 | ELRSバインドUID（より確実） |
| 映像送信機 | 必須 | 不要 |
| デジタル映像対応 | 別途必要 | ネイティブ対応 |
| ゲートノード | 専用ハード | ESP32-WROVER-E + XIAO ESP32-S3 |

---

## ハードウェア構成

```
┌──────────────────────────┐    UART (双方向)    ┌──────────────────────────┐
│   ESP32-WROVER-E-A       │ ←────────────────→ │   XIAO ESP32-S3-B        │
│     (Gate Node)          │                    │     (Web Node)           │
│                          │                    │                          │
│ - WiFi NULLモード        │                    │ - WiFi AP モード         │
│ - Promiscuousモード       │                    │   SSID: ELRS bp-LT       │
│   ESP-NOW パケット受信   │                    │   PASS: elrsbp-lt        │
│ - EMAフィルタ処理        │                    │   IP:   20.0.0.1         │
│ - RSSI状態機械           │                    │ - ESPAsyncWebServer      │
│ - パイロットUID識別       │                    │ - WebSocket /ws          │
│ - パイロット別閾値管理    │                    │ - LittleFS (index.html)  │
│                          │                    │ - NVS設定保存            │
└──────────────────────────┘                    └──────────────────────────┘
       ゲートに設置                                    ピット・手元に設置
  アンテナ: パッチアンテナ推奨                        スマホからWiFi接続
```

### ピン配線

| ESP32-WROVER-E-A | 方向 | XIAO ESP32-S3-B |
|-----------------|------|-----------------|
| GPIO26 (TX1) | → | GPIO3 / D2 (RX1) |
| GPIO25 (RX1) | ← | GPIO2 / D1 (TX1) |
| GND | — | GND |

---

## UART プロトコル

### Gate → Web（ラップ・テレメトリ）

```json
{"type":"lap",  "pilot":0,"uid":"AA:BB:CC:DD:EE:FF","rssi":-72,"ts":123456}
{"type":"rssi", "pilot":0,"uid":"AA:BB:CC:DD:EE:FF","rssi":-85,"raw":-87,"crossing":false,"ts":123460}
{"type":"ready","pilots":4}
```

### Web → Gate（レース制御・閾値設定）

```json
{"type":"cmd","action":"race_start"}
{"type":"cmd","action":"set_threshold","pilot":0,"enter":-80,"exit":-90}
```

---

## RSSIピーク検出アルゴリズム

```
生RSSI → EMAフィルタ (α=0.3) → EnterAt/ExitAt 状態機械 → ゲート通過イベント
```

### 状態機械（RotorHazard準拠）

```
CLEAR（待機）
 └─(ema > EnterAt)→ CROSSING（通過中）
      ├─(ema > peak) → ピーク更新・ピーク時刻記録
      └─(ema < ExitAt)→ triggerLap(peakTime) → CLEAR（確定）
```

### 調整パラメータ

| パラメータ | 対応RotorHazard | デフォルト値 | 説明 |
|-----------|----------------|------------|------|
| EnterAt | EnterAt | -80 dBm | 通過開始判定RSSI |
| ExitAt | ExitAt | -90 dBm | 通過終了判定RSSI |
| EMA_ALPHA | — | 0.3 | 平滑化係数 |
| COOLDOWN_MS | MinLapTime | 3000 ms | 最小ラップ時間 |

**パイロット別にランタイムで変更可能**（Calibタブのスライダーで設定→Gate Nodeへ即時反映）。

---

## Web UI（PhobosLT_4ch スタイル）

**デザイン:**
- GitHub Darkテーマ（`#0d1117` ベース）
- システムフォント（`-apple-system, BlinkMacSystemFont, "Segoe UI", Arial`）
- パイロット4色：`#58a6ff` (青) / `#f85149` (赤) / `#3fb950` (緑) / `#d29922` (琥珀)

### Race タブ

- レースタイマー、開始 / 停止 / クリアボタン
- パイロット4列グリッド：COSSINGバッジ、RSSSIミニバー、ベストラップ＋デルタ表示
- パイロット別ラップ表（周回 / タイム / 累計列）、ベストラップ行グリーンハイライト
- **ビープカウントダウン**（フルスクリーンオーバーレイなし）：3×440Hz → 880Hz

### Config タブ

- グローバル設定：アナウンスモード選択、発話速度スライダー
- パイロット別：名前・バインドフレーズ入力 → UID自動表示（`/api/uid` 経由）

### Calib タブ

- パイロット別 Canvas RSSI波形グラフ（Enter ライン=緑、Exit ライン=赤）
- Enter / Exit 閾値スライダー（-120〜-40 dBm）
- 保存時にGate Nodeへ即時反映

---

## REST API（Web Node）

| エンドポイント | 説明 |
|-------------|------|
| `GET/POST /api/pilots` | パイロット設定（名前・UID）— NVS保存 |
| `GET /api/uid?phrase=...` | バインドフレーズ → UID（SHA-256、ESP32側で計算） |
| `GET/POST /api/calib` | Enter/Exit閾値 — NVS保存＋Gate Node反映 |
| `POST /api/race/start` | レース開始（Gate Nodeにrace_start送信） |
| `POST /api/race/stop` | レース停止 |
| `GET /api/laps` | ラップ履歴 |

---

## UID（パイロット識別）の仕組み

- ELRSバインドフレーズ → SHA-256 → 先頭6バイト = UID
- TX BackpackがESP-NOWパケットを送信するときのMACアドレスがUID由来
- Gate NodeはパケットのMACを見るだけでパイロットを識別
- Web UIのConfigタブでバインドフレーズを入力するとUIDが自動計算される

> **注:** `crypto.subtle.digest` はHTTPS必須のため、HTTP環境では動作しない。  
> 本システムは `GET /api/uid` エンドポイントでESP32側（mbedTLS）のSHA-256を使用。

---

## ビルド・書き込み手順

```bash
# Gate Node ビルド＋書き込み
pio run -e gate_node -t upload

# Web Node ビルド＋書き込み
pio run -e web_node -t upload

# LittleFS（Web UI）書き込み
pio run -e web_node -t uploadfs
```

---

## 参加者の設定（機体側）

1. TX BackpackのLuaスクリプトを開く
2. `Telemetry` → `ESP-NOW` に設定
3. 以上。バインドフレーズはそのまま使用

---

## 関連リポジトリ

- PhobosLT（既存ラップタイマー）: [yanazoo/PhobosLT_4ch](https://github.com/yanazoo/PhobosLT_4ch)
- ELRS Backpack公式: [ExpressLRS/Backpack](https://github.com/ExpressLRS/Backpack)
