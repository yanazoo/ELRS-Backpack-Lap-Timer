# ELRS Backpack Lap Timer

ELRSのBackpack ESP-NOW通信を活用した、FPVドローンレース用ラップタイマー。

**特徴:**
- 機体側の改造不要（XIAO ESP32-C3を機体に搭載するだけ）
- 最大50人のパイロット名簿（ロースター）管理、最大4スロット同時レース
- RSSIピーク検出 + RotorHazard準拠の状態機械でゲート通過を計測
- EMAフィルタ（α=0.3）による滑らかなRSSI処理
- GitHub Darkテーマ Web UI（日本語TTS、Canvas波形グラフ、SDファイルブラウザ）
- SDカードへのレースCSV自動記録・パイロット情報バックアップ/復元

---

## ハードウェア構成

```
┌─────────────────────────┐    UART (双方向)    ┌─────────────────────────┐
│   ESP32-WROVER-E-A      │ ←───────────────→ │   XIAO ESP32-S3-B       │
│  LilyGo TTGO T8 V1.8    │                   │     (Web Node)          │
│     (Gate Node)         │                   │                         │
│                         │                   │ - WiFi AP               │
│ - WiFi NULLモード        │                   │   SSID: ESP-NOW-LT      │
│ - Promiscuousモード       │                   │   PASS: esp-now-lt      │
│   ESP-NOW パケット受信   │                   │   IP:   20.0.0.1        │
│ - EMAフィルタ処理        │                   │ - ESPAsyncWebServer     │
│ - RSSI状態機械           │                   │ - WebSocket /ws         │
│ - SDカード記録           │                   │ - LittleFS (Web UI)     │
│   CS=13 MOSI=15         │                   │ - NVS設定保存           │
│   MISO=2 SCK=14         │                   │                         │
└─────────────────────────┘                   └─────────────────────────┘
       ゲートに設置                                   ピット・手元に設置
  アンテナ: パッチアンテナ推奨                       スマホからWiFi接続

┌─────────────────────────┐
│   XIAO ESP32-C3         │
│   (Aircraft Node)       │
│                         │
│ - ESP-NOW ビーコン送信  │
│ - 機体に搭載            │
└─────────────────────────┘
```

### ピン配線

| ESP32-WROVER-E (Gate) | 方向 | XIAO ESP32-S3 (Web) |
|----------------------|------|---------------------|
| GPIO26 (TX1) | → | GPIO3 / D2 (RX1) |
| GPIO25 (RX1) | ← | GPIO2 / D1 (TX1) |
| GND | — | GND |

---

## パイロットモデル

- **ロースター**: NVSに最大50人のパイロット情報（名前・読み方・機体MAC・RSSI閾値）を保存
- **アクティブスロット**: ロースターから最大4人を選択してゲートに割り当て
- **機体識別**: XIAO ESP32-C3のハードウェアMACアドレスでパイロットを一意識別
- **機体スキャン**: ESP-NOWフレームを受信すると未登録機体が自動スキャンリストに出現

---

## ソースコード構成

### Gate Node (`src/gate_node/`)

| ファイル | 役割 |
|---------|------|
| `config.h` | ピン定義・タイミング定数（EMA_ALPHA、COOLDOWN_MSなど） |
| `pilots.h/cpp` | PilotState配列・初期化/検索/スキャン報告 |
| `promiscuous.h/cpp` | ISRコールバック・FreeRTOSキュー・WiFi設定 |
| `sd_gate.h/cpp` | SDカード初期化・レースCSV・バックアップ/復元・ファイルブラウザ |
| `uart_gate.h/cpp` | `sendLap`・`sendRssi`・`processWebCmd`ディスパッチ |
| `main.cpp` | `setup()`/`loop()`・EMA状態機械（約90行） |

### Web Node (`src/web_node/`)

| ファイル | 役割 |
|---------|------|
| `config.h` | UART定義・パイロット上限・WiFi AP設定 |
| `data_model.h` | 全構造体定義・グローバル変数extern宣言 |
| `nvs_store.h/cpp` | NVS読み書き（`roster[]`・`prefs`を所有） |
| `gate_comm.h/cpp` | Gate UARTプロトコル・`processGateLine`（`rt[]`・`laps[]`を所有） |
| `json_api.h/cpp` | `rosterJson`/`activeJson`/`lapsJson`/`scanJson`・`handleBody` |
| `ws_handler.h/cpp` | WebSocket・`wsText`・`onWsEvent` |
| `http_routes.h/cpp` | 全`server.on()`ルート登録 |
| `main.cpp` | `setup()`/`loop()`（約55行） |

### Frontend (`data/`)

| ファイル | 役割 |
|---------|------|
| `index.html` | HTMLシェル＋CSSのみ（インラインJSなし） |
| `js/globals.js` | 定数・スロット状態・フォーマット関数・`switchTab` |
| `js/audio.js` | Web Audio API・`sfx`オブジェクト（RotorHazard準拠音域）・TTS音声キュー |
| `js/race.js` | レースカード・タイマー・レース制御・`applyActiveToSlots` |
| `js/config.js` | ロースターCRUD・スキャン・SDバックアップ/復元・`updateSdSection` |
| `js/calib.js` | Canvasチャート・rAFループ・閾値スライダー・`syncCalibSliders` |
| `js/sd.js` | SDファイルブラウザ（一覧・ダウンロード・削除） |
| `js/ws.js` | WebSocket・`onMsg`ディスパッチ・`loadRoster`・`loadAll`・アプリ初期化 |

---

## UART プロトコル

### Gate → Web

```json
{"type":"lap",          "pilot":0,"uid":"AA:BB:CC:DD:EE:FF","rssi":-72,"ts":123456}
{"type":"rssi",         "pilot":0,"rssi":-85,"raw":-87,"crossing":false,"ts":123460}
{"type":"ready",        "pilots":4}
{"type":"sd_status",    "present":true}
{"type":"scan",         "mac":"AA:BB:CC:DD:EE:FF","rssi":-75,"ts":123470}
{"type":"sd_pilot_row", "name":"...","yomi":"...","mac":"...","enter":-80,"exit":-90}
{"type":"sd_restore_done"}
{"type":"sd_file_list", "files":[{"name":"race_001.csv","size":1024}]}
{"type":"sd_file_line", "path":"/race_001.csv","line":"0,AA:BB,..."}
{"type":"sd_file_done", "path":"/race_001.csv"}
{"type":"sd_delete_result","path":"/race_001.csv","ok":true}
```

### Web → Gate

```json
{"type":"cmd","action":"race_start"}
{"type":"cmd","action":"set_pilot",    "pilot":0,"uid":"AA:BB:CC:DD:EE:FF"}
{"type":"cmd","action":"set_threshold","pilot":0,"enter":-80,"exit":-90}
{"type":"cmd","action":"sd_begin_backup"}
{"type":"cmd","action":"sd_backup_row","name":"...","yomi":"...","mac":"...","enter":-80,"exit":-90}
{"type":"cmd","action":"sd_end_backup"}
{"type":"cmd","action":"sd_restore_request"}
{"type":"cmd","action":"sd_list_files"}
{"type":"cmd","action":"sd_read_file",  "path":"/race_001.csv"}
{"type":"cmd","action":"sd_delete_file","path":"/race_001.csv"}
```

---

## RSSIピーク検出アルゴリズム

```
生RSSI → EMAフィルタ (α=0.3、毎ループ適用) → Enter/Exit 状態機械 → ゲート通過イベント
```

### 状態機械（RotorHazard準拠）

```
CLEAR（待機）
 └─(ema > EnterAt)→ CROSSING（通過中）
      ├─(ema > peak) → ピーク更新・ピーク時刻記録
      └─(ema < ExitAt かつ cooldown経過)→ sendLap(peakTime) → CLEAR
```

### 調整パラメータ

| パラメータ | デフォルト | 説明 |
|-----------|-----------|------|
| EnterAt | -80 dBm | 通過開始判定RSSI |
| ExitAt | -90 dBm | 通過終了判定RSSI |
| EMA_ALPHA | 0.3 | 平滑化係数 |
| COOLDOWN_MS | 3000 ms | 最小ラップ時間 |
| RSSI_INTERVAL_MS | 50 ms | テレメトリ送信間隔 (20Hz) |

キャリブタブのスライダーで**パイロット別・ランタイム変更可能**（Gate Nodeへ即時反映）。

---

## Web UI

**接続:** WiFi SSID `ESP-NOW-LT` (PASS: `esp-now-lt`) → ブラウザで `http://20.0.0.1`

### Race タブ
- 3秒カウントダウン + レースタイマー（開始/停止/クリア）
- パイロット4列グリッド：CROSSINGバッジ・RSSSIバー・ベストラップ+デルタ表示
- パイロット別ラップ表（周回 / タイム / 累計）

### Config タブ
- **機体スキャン**: 電源ON後自動検出 → 名前入力でロースターに追加
- **パイロット情報**: 最大50人（名前・読み方・機体MAC・チャンネル割当）
- **グローバル設定**: アナウンスモード・発話速度（`localStorage`に自動保存）
- **SDカード**: パイロット情報のバックアップ/復元（SDカード検出時のみ表示）

### Calib タブ
- パイロット別 Canvas RSSI波形グラフ（60fps rAFループ、動的Yスケール）
- Enter/Exit 閾値スライダー（変更から800msデバウンスで自動保存）

### SD タブ
- SDカード内ファイル一覧表示
- レースCSVファイルのダウンロード（WebSocket経由ストリーミング）・削除

---

## REST API（Web Node）

| エンドポイント | メソッド | 説明 |
|-------------|---------|------|
| `/api/pilots` | GET/POST | ロースター取得・追加/更新（NVS保存） |
| `/api/pilots/delete` | POST `{id}` | ロースターからパイロット削除 |
| `/api/active` | GET/POST | アクティブスロット取得・設定 |
| `/api/calib` | POST `{id,enter,exit}` | RSSI閾値更新（NVS保存＋Gate反映） |
| `/api/race/start` | POST | レース開始（Gate Nodeにrace_start送信） |
| `/api/race/stop` | POST | レース停止 |
| `/api/laps` | GET | ラップ履歴取得 |
| `/api/scan` | GET | スキャン済み未登録MAC一覧 |
| `/api/scan/clear` | POST | スキャンリストクリア |
| `/api/status` | GET | システム状態（raceRunning, lapCount等） |
| `/api/sd/status` | GET | SDカード有無 |
| `/api/sd/pilots/backup` | POST | ロースターをSDカードに保存 |
| `/api/sd/pilots/restore` | POST | SDカードからロースター復元 |
| `/api/sd/files/list` | POST | SDファイル一覧取得（WS経由）|
| `/api/sd/files/download` | POST `{path}` | ファイルダウンロード（WS経由）|
| `/api/sd/files/delete` | POST `{path}` | ファイル削除（WS経由）|

---

## ビルド・書き込み手順

### 必要環境
- PlatformIO Core または PlatformIO IDE (VS Code拡張)

### ビルド＋書き込み

```bash
# Gate Node (ESP32-WROVER-E / LilyGo TTGO T8 V1.8)
pio run -e gate_node -t upload

# Web Node (XIAO ESP32-S3)
pio run -e web_node -t upload

# Web UI (LittleFS) 書き込み
pio run -e web_node -t uploadfs

# Aircraft Node (XIAO ESP32-C3) — 機体側
pio run -e aircraft_node -t upload
```

### SDカードについて（LilyGo TTGO T8 V1.8）

| ピン | GPIO |
|-----|------|
| CS | 13 |
| MOSI | 15 |
| MISO | 2 |
| SCK | 14 |

FAT32フォーマットのmicroSDカードを使用。レースCSVは `/race_001.csv`, `/race_002.csv`, ... の形式で自動保存。

---

## 機体側の設定（Aircraft Node）

XIAO ESP32-C3に `aircraft_node` ファームウェアを書き込み、機体に搭載するだけ。
ESP-NOWビーコンを自動送信するため、TX Backpack等の設定は不要。

---

## 効果音（RotorHazard準拠）

| イベント | 音域 | 波形 |
|---------|------|------|
| ラップ検出 | 1200Hz → 1800Hz | square |
| ベストラップ | 1200/1800Hz 交互×3 | square |
| カウントダウン | 440Hz×3 → 880Hz | triangle |
| ENTER閾値超過 | 880Hz | sine |
| EXIT閾値下回り | 1100Hz | sine |

---

## 関連リポジトリ

- PhobosLT（既存ラップタイマー）: [yanazoo/PhobosLT_4ch](https://github.com/yanazoo/PhobosLT_4ch)
- ELRS Backpack公式: [ExpressLRS/Backpack](https://github.com/ExpressLRS/Backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
