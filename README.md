# ELRS Backpack ラップタイマー — Claude Code 引継ぎ文書

作成日時: 2026-04-23 (JST)

-----

## プロジェクト概要

ELRSのBackpack ESP-NOW通信を活用して、RotorHazardのような
FPVドローンレース用ラップタイマーを開発する。

**最大の特徴：**

- 機体側の改造不要（参加者はELRS TX BackpackのTelemetry=ESP-NOWをONにするだけ）
- バインドフレーズ由来のUIDでパイロットを自動識別
- RSSIピーク検出でゲート通過タイミングを計測

-----

## RotorHazardとの比較

|項目      |RotorHazard   |本システム                   |
|--------|--------------|------------------------|
|検出方式    |5.8GHz映像電波RSSI|2.4GHz ELRS ESP-NOW RSSI|
|パイロット識別 |映像送信機の周波数     |ELRSバインドUID（より確実）       |
|映像送信機   |必須            |不要                      |
|デジタル映像対応|別途必要          |ネイティブ対応                 |
|ゲートノード  |専用ハード         |ESP32のみ                 |

-----

## システムアーキテクチャ

```
[Pilot TX] (ELRS Backpack Telemetry=ESP-NOW ON)
    ↓ 2.4GHz ESP-NOW broadcast（UID含むパケット）
[Gate Node: ESP32]
    - Promiscuousモードで全パケット受信
    - 送信元MACアドレスでパイロット識別
    - EMAフィルタ + ピークホールド検出でゲート通過判定
    ↓ ESP-NOW
[PhobosLT or タイマーサーバー: ESP32]
    ↓ WebSocket
[Web UI / HDZero OSD]
```

-----

## ハードウェア構成

### ESP32の役割分担（2台構成）

```
┌─────────────────────────┐        UART        ┌─────────────────────────┐
│   ESP32-A（信号解析）    │ ─────────────────→ │   ESP32-B（Web/WiFi）   │
│                         │  ラップトリガー送信  │                         │
│ - ESP-NOWパケット受信    │                    │ - WiFi APモード         │
│   (Promiscuousモード)   │                    │ - WebサーバーホストUI   │
│ - EMAフィルタ処理       │                    │ - WebSocketでスマホ配信 │
│ - RSSIピーク検出        │                    │ - ラップタイム管理      │
│ - パイロットUID識別      │                    │ - パイロット設定保存    │
│                         │                    │   (Preferences/NVS)    │
└─────────────────────────┘                    └─────────────────────────┘
       ゲート設置                                      ピット・手元に設置
  アンテナ: パッチアンテナ推奨                        スマホからWiFi接続
```

### 分割理由

ESP32はWiFi使用中にPromiscuousモードとの干渉が発生する可能性がある。
役割を完全分離することで：

- ESP32-A: WiFi無効化してPromiscuousに専念 → 受信安定性向上
- ESP32-B: WiFiに専念 → Web UIの安定性向上

### UART通信仕様

|項目   |内容                     |
|-----|-----------------------|
|ボーレート|115200 bps             |
|接続ピン |ESP32-A TX → ESP32-B RX|
|プロトコル|JSON（1行1パケット、改行区切り）    |

**ESP32-A → ESP32-B 送信フォーマット:**

```json
{"type":"lap","pilot":0,"uid":"AA:BB:CC:DD:EE:FF","rssi":-72,"ts":123456}\n
{"type":"rssi","pilot":0,"rssi":-85,"ts":123460}\n
```

- `lap`: ゲート通過イベント（ラップトリガー）
- `rssi`: RSSIリアルタイム値（Calibrationタブの波形表示用、間引き送信可）

### ハードウェアリスト

- **ESP32-A（信号解析）:** ESP32 DevKitC または同等品
  - アンテナ: 指向性パッチアンテナ推奨
  - 電源: バッテリー or USB（ゲートに設置）
- **ESP32-B（Web/WiFi）:** ESP32 DevKitC または同等品
  - 電源: バッテリー or USB（ピットに設置）
- **接続:** UARTケーブル（TX-RX、GND共通）
- 参加者側: ELRS TX Backpack搭載のプロポ（改造不要）

-----

## 参加者の設定（機体側）

1. TX BackpackのLuaスクリプトを開く
1. `Telemetry` → `ESP-NOW` に設定
1. 以上。バインドフレーズはそのまま使用

-----

## UID（パイロット識別）の仕組み

- ELRSのバインドフレーズ → SHA256 → 6バイトUID
- TX BackpackがESP-NOWパケットを送信するとき、MACアドレスがUID由来
- ゲートノードはパケットの送信元MACを見るだけでパイロット識別可能
- 事前にパイロットのバインドフレーズ→UIDをWeb UIで登録しておく

UIDの生成確認: https://expresslrs.github.io/web-flasher/

-----

## RSSIピーク検出アルゴリズム（設計済み）

### RotorHazardの思想を参考にする

RotorHazardはノードを **Crossing（通過中）** と **Clear（クリア）** の2状態で管理する。
RSSIが低い状態がClear、RSSIが高い状態がCrossing。
Crossingが終了してClearに戻った時点でラップを確定記録する。

この思想を本システムに適用する：

- **EnterAt** → `ENTRY_THRESHOLD`（ドローンが近づいたと判断するRSSI値）
- **ExitAt** → `EXIT_THRESHOLD`（ドローンが離れたと判断するRSSI値）
- ラップ確定タイミングは **ピーク時刻**（RotorHazard同様、通過の中心点）

```
生RSSI → EMAフィルタ（α=0.3）→ EnterAt/ExitAt状態機械 → ゲート通過イベント
```

### 状態機械（RotorHazard準拠）

```
CLEAR（待機）
 └─(rssi > EnterAt)→ CROSSING（通過中）
      ├─(rssi > peak) → ピーク更新・ピーク時刻記録
      └─(rssi < ExitAt)→ triggerLap(peakTime) → CLEAR（確定）
```

RotorHazardと同様、**ExitAtをEnterAtより低く**設定することでヒステリシスを持たせ、
ノイズによる誤検出を防ぐ。

### 調整パラメータ（実測で決定する）

|パラメータ          |対応RotorHazard|初期値    |説明            |
|---------------|-------------|-------|--------------|
|ENTRY_THRESHOLD|EnterAt      |-80 dBm|通過開始判定RSSI    |
|EXIT_THRESHOLD |ExitAt       |-90 dBm|通過終了判定RSSI    |
|EMA_ALPHA      |（内部フィルタ）     |0.3    |平滑化係数         |
|COOLDOWN_MS    |MinLapTime   |3000 ms|最小ラップ時間（誤検出抑制）|

## Web UI 演出仕様（視覚・音響）

### 基本方針

RotorHazardのシンプルな通知に加え、**レース現場の興奮を高める演出**を実装する。
すべてブラウザのWeb APIのみで実現（外部ライブラリ不要）。

-----

### 視覚演出

#### ① RSSIリアルタイムバー（Calibrationタブ）

```
Pilot 1  ████████████░░░░░░  -72dBm  CROSSING
Pilot 2  ██░░░░░░░░░░░░░░░░  -95dBm  CLEAR
```

- バーの色がCLEAR（暗）→ CROSSING（アクセントカラーで発光）に変化
- EnterAt / ExitAt ラインをバー上にオーバーレイ表示
- CROSSINGの瞬間にカード全体がパルスアニメーション（CSS `@keyframes`）

#### ② ゲート通過フラッシュ（Raceタブ）

- ラップ確定時、該当パイロットのカードが**一瞬フラッシュ**（白→アクセントカラー）
- 画面全体に薄いフラッシュオーバーレイ（0.3秒）
- ラップタイムが大きく表示されてフェードイン

```css
/* ラップ確定アニメーション例 */
@keyframes lapFlash {
  0%   { background: #ffffff; transform: scale(1.03); }
  30%  { background: var(--accent); }
  100% { background: var(--card-bg); transform: scale(1.0); }
}
```

#### ③ ラップタイム表示

- 最新ラップ: **超大フォント**（Orbitron、中央に1〜2秒表示後フェードアウト）
- ベストラップ更新時: ゴールドカラーで `🏆 BEST LAP!` テキストアニメーション
- 前ラップ比較: `▲ +0.32s` / `▼ -0.18s` をカラーで表示（遅い=赤、速い=緑）

#### ④ RSSI波形グラフ（Calibrationタブ）

- SmoothieChartでリアルタイムスクロール波形
- EnterAt / ExitAtをグラフ上に水平破線で表示
- CROSSING区間を背景色で塗りつぶし（半透明アクセントカラー）
- パイロットごとに異なる色の波形

-----

### 音響演出（Web Audio API）

すべて`AudioContext`で生成するビープ音。音源ファイル不要。

```javascript
const ctx = new AudioContext();

function beep(freq, duration, type = 'sine', vol = 0.3) {
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.frequency.value = freq;
    osc.type = type;
    gain.gain.setValueAtTime(vol, ctx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + duration);
    osc.start(ctx.currentTime);
    osc.stop(ctx.currentTime + duration);
}
```

#### イベント別サウンド

|イベント        |音                        |説明                  |
|------------|-------------------------|--------------------|
|ゲート通過（通常ラップ）|880Hz・0.15秒・sine         |短い高音ビープ             |
|ベストラップ更新    |880Hz→1320Hz→1760Hz・各0.1秒|上昇3音                |
|レース開始カウントダウン|440Hz×3回→880Hz           |RotorHazard準拠の3+1ビープ|
|レース終了       |880Hz→440Hz→220Hz・各0.2秒  |下降3音                |
|CROSSING検出開始|220Hz・0.05秒・square       |低い短音（任意でON/OFF）     |

#### 音量・ON/OFF設定

- Web UI右上にスピーカーアイコンでミュートトグル
- 音量スライダー（0〜100%）
- 設定はlocalStorageに保存

-----

### 実装ライブラリ

|用途         |ライブラリ                  |CDN          |
|-----------|-----------------------|-------------|
|RSSI波形グラフ  |SmoothieChart          |`smoothie.js`|
|ラップ表アニメーション|CSS Animations（ライブラリ不要）|—            |
|音響         |Web Audio API（ブラウザ標準）  |—            |

-----

### ユーザー操作メモ

- **iOSのSafari**: `AudioContext`はユーザー操作後（タップ後）でないと音が出ない
  → 「START RACE」ボタンタップ時に`ctx.resume()`を呼ぶことで対処
- **音の遅延**: WebSocketでラップイベントを受信後即座に`beep()`を呼ぶことで
  体感遅延を最小化（UARTのレイテンシ＋WebSocket配信遅延は合計10〜20ms程度）

-----

## 開発フェーズ

### フェーズ1（最初にやること）：RSSIロギング ← **ここから開始**

**目的:** 実際のゲート通過時のRSSI波形を確認してパラメータを決める

**作成ファイル:**

- `src/main.cpp` — ESP32 PlatformIOスケッチ
- `data/index.html` — リアルタイム波形表示Web UI（LittleFS配信）

**機能:**

- ESP32がAPモードで起動（SSID: `ELRS-Logger`）
- Promiscuousモードで全ESP-NOWパケット受信
- 送信元MACを自動検出・パイロット登録
- WebSocket経由でRSSI履歴をリアルタイム配信
- Web UIで波形グラフ表示（SmoothieChart使用）
- Web UIでパイロット名を設定（MAC→名前のマッピング）
- CSVダウンロード機能（波形データの保存・分析用）

**スタック:**

- PlatformIO + ESP32 Arduino framework
- ESPAsyncWebServer + AsyncWebSocket
- LittleFS（index.html配信）
- ArduinoJson
- SmoothieChart（Web UI）

### フェーズ2：ピーク検出実装

- フェーズ1の波形データをもとにパラメータ決定
- ピークホールド状態機械実装
- シリアルログでトリガーイベント確認

### フェーズ3：PhobosLT統合

- ESP-NOWでゲートノード → PhobosLTへラップトリガー送信
- PhobosLTのパイロット管理UIにUID/名前登録機能追加
- 既存のWebSocket/OSDラップ表示と接続

-----

## パイロット登録 Web UI 仕様

### 概要

バインドフレーズを入力するとUID（6バイト）を自動計算し、
ESP32のNVS（Preferences）に保存するWeb UI。

### 対象ファイル

`data/index.html` のConfigタブ内に実装する。

### デザイン

**PhobosLTの4chカードレイアウトを踏襲する。**

- ダークテーマ（背景: #0a0a0a 〜 #111）
- フォント: `Orbitron`（見出し・UID表示）＋ `Share Tech Mono`（入力・数値）
- 4つのパイロットカードを2×2グリッド（モバイル時は1列）で配置
- 各カードにPilot番号をアクセントカラーで大きく表示
- アクセントカラー: Pilot1=シアン / Pilot2=オレンジ / Pilot3=グリーン / Pilot4=マゼンタ
  （PhobosLTの4ch配色に合わせる）

### 各カードの入力項目

```
┌─────────────────────────────┐
│  ① PILOT 1                  │  ← アクセントカラーで強調
│                             │
│  Name                       │
│  [________________]         │  ← テキスト入力
│                             │
│  Bind Phrase                │
│  [________________]         │  ← テキスト入力
│                             │
│  UID                        │
│  AA:BB:CC:DD:EE:FF          │  ← 自動計算・読み取り専用表示
│                             │
│  [    SAVE    ]             │  ← 保存ボタン
└─────────────────────────────┘
```

### UID自動計算ロジック（ELRS準拠）

ELRSのバインドフレーズ → UID変換はSHA256ベース。
**JavaScriptで実装する。**

```javascript
// ELRS公式アルゴリズム（ExpressLRS/src/lib/SX1280Driver/SX1280.cpp参照）
// bindPhrase → UTF-8バイト列 → SHA256 → 先頭6バイトがUID

async function bindPhraseToUID(phrase) {
    const encoder = new TextEncoder();
    const data = encoder.encode(phrase);
    const hashBuffer = await crypto.subtle.digest('SHA-256', data);
    const hashArray = Array.from(new Uint8Array(hashBuffer));
    // 先頭6バイトをUIDとして使用
    const uid = hashArray.slice(0, 6);
    return uid.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(':');
}

// バインドフレーズ入力のたびにリアルタイム計算
phraseInput.addEventListener('input', async () => {
    const uid = await bindPhraseToUID(phraseInput.value);
    uidDisplay.textContent = uid;
});
```

**検証方法:** https://expresslrs.github.io/web-flasher/ で同じフレーズを入力して
表示されるUIDと一致することを確認すること。

### API（ESP32側）

```
GET  /api/pilots          → 全パイロット設定をJSON返却
POST /api/pilots          → パイロット設定を保存
     Body: {"id":0,"name":"Taro","bindPhrase":"my-phrase","uid":[0xAA,...]}
```

### 保存先

ESP32の `Preferences`（NVS）に保存。キー例:

- `pilot0_name`
- `pilot0_uid` （6バイトのバイナリ or HEX文字列）

### 動作フロー

1. ページロード時に `/api/pilots` で既存設定を取得・表示
1. バインドフレーズ入力 → リアルタイムでUID計算・表示
1. SAVEボタン → `/api/pilots` にPOST → ESP32がNVSに保存
1. 保存成功でカードにチェックマーク表示
1. 保存されたUIDはゲートノードのパイロット識別に使用される

-----

## platformio.ini

```ini
[env:elrs_logger]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
board_build.filesystem = littlefs
lib_deps =
    ESP Async WebServer
    ArduinoJson
```

-----

## 注意事項

- ⚠️ WiFi SSID/Passwordをコードにハードコードする場合は
  GitHubにUPする前に必ず公開用ダミー値に変更すること
- ESP-NOWのPromiscuousモードはWiFi APモードと共存可能（要検証）
- PromiscuousコールバックはISRコンテキスト → portENTER_CRITICAL_ISR使用
- パケットフィルタ（ActionFrame判定）は実測で調整が必要

-----

## 関連リポジトリ

- PhobosLT（既存ラップタイマー）: yanazoo/PhobosLT
- ELRS Backpack例: druckgott/ELRS-Backpack-Example-ESPNOW
- ELRS Backpack公式: ExpressLRS/Backpack

-----

## 参考情報

- ESP-NOW Promiscuousパケット構造:
  `wifi_promiscuous_pkt_t` → `rx_ctrl.rssi` でRSSI取得
  送信元MAC: `payload[10..15]`（ActionFrame）
- ELRS Backpack v3.4以降でTelemetry ESP-NOW対応
- Generic Race Timer Backpackターゲットがすでに公式に存在する
  （RotorHazard→ELRS OSD送信用。本プロジェクトはその逆方向）