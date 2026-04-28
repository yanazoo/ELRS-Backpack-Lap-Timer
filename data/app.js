'use strict';

// ── Constants ─────────────────────────────────────────────────
const NUM_PILOTS = 4;
const P_CLASS    = ['p0', 'p1', 'p2', 'p3'];
const P_COLOR    = ['#58a6ff', '#f85149', '#3fb950', '#d29922'];

// ── State ─────────────────────────────────────────────────────
let g_pilots     = [];
let g_minLapMs   = 3000;
let raceActive   = false;
let raceStartMs  = 0;
let raceRafId    = null;
let totalLapGoal = 0;

let lapNos       = [0, 0, 0, 0];
let lapBest      = [Infinity, Infinity, Infinity, Infinity];
let lapTimesArr  = [[], [], [], []];
let prevCrossing = [false, false, false, false];

let voiceOn  = true;
let audioCtx = null;

let ws            = null;
let wsReconnTimer = null;

const rssiSeries = new Array(NUM_PILOTS).fill(null);
const rssiCharts = new Array(NUM_PILOTS).fill(null);

// ── Utilities ─────────────────────────────────────────────────
const el    = id => document.getElementById(id);
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const pad   = (n, w = 2) => String(Math.floor(Math.abs(n))).padStart(w, '0');

const toDisp = dbm => clamp(Math.round((dbm + 120) * 150 / 90), 0, 150);
const toDbm  = d   => Math.round(d * 90 / 150 - 120);

function hexRgba(hex, a) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${a})`;
}

function fmtLap(ms) {
  const m = Math.floor(ms / 60000);
  const s = Math.floor((ms % 60000) / 1000);
  const c = Math.floor((ms % 1000) / 10);
  return `${pad(m)}:${pad(s)}.${pad(c)}`;
}

// ── SHA-256 → UID (first 6 bytes of hash) ────────────────────
async function bindPhraseToUID(phrase) {
  if (!phrase) return '';
  const data = new TextEncoder().encode(phrase);
  const hash = await crypto.subtle.digest('SHA-256', data);
  return Array.from(new Uint8Array(hash))
    .slice(0, 6)
    .map(b => b.toString(16).padStart(2, '0').toUpperCase())
    .join(':');
}

// ── Card HTML Builders ────────────────────────────────────────
function buildRaceCard(i) {
  const pc = P_CLASS[i];
  return `
<div class="pilot-race-card" id="raceCard${i}">
  <div class="pilot-card-header ${pc}">
    <span id="pname${i}">Pilot ${i + 1}</span>
    <span class="pilot-freq-badge" id="pmac${i}"></span>
    <span class="pilot-lapcount">Lap <span id="lapCount${i}">0</span></span>
  </div>
  <div class="rssi-mini-bar">
    <span class="rssi-mini-label">RSSI</span>
    <div class="rssi-mini-track">
      <div class="rssi-mini-fill ${pc}-fill" id="rssiBar${i}" style="width:0%"></div>
    </div>
    <span class="rssi-mini-val" id="rssiNum${i}">---</span>
  </div>
  <div class="pilot-total-row">
    ベスト: <span class="pilot-total" id="pilotBest${i}">--:--.--</span>
    &nbsp;合計: <span id="pilotTotal${i}">0</span>周
  </div>
  <div class="lap-table-wrap">
    <table class="lapTable" id="lapTable${i}">
      <thead><tr><th>#</th><th>タイム</th><th>差</th></tr></thead>
      <tbody></tbody>
    </table>
  </div>
</div>`;
}

function buildConfigCard(i) {
  const pc = P_CLASS[i];
  return `
<div class="config-pilot-card ${pc}-border">
  <div class="config-pilot-header ${pc}">Pilot ${i + 1}</div>
  <div class="config-pilot-body">
    <div class="config-row">
      <label>名前</label>
      <input type="text" id="cfgName${i}" value="Pilot ${i + 1}">
    </div>
    <div class="config-row">
      <label>MAC (検出済)</label>
      <span class="mac-display" id="cfgMac${i}">未検出</span>
    </div>
    <div class="config-row">
      <label>バインドフレーズ</label>
      <input type="text" id="cfgPhrase${i}" placeholder="bind phrase"
             oninput="onPhraseInput(${i})" autocomplete="off" spellcheck="false">
      <button class="btn-secondary" style="padding:4px 8px;font-size:11px"
              onclick="clearUid(${i})">✕</button>
    </div>
    <div class="config-row">
      <label>UID (自動計算)</label>
      <span class="mac-display" id="cfgUidDisp${i}">未設定</span>
    </div>
    <button class="btn-save" onclick="savePilotConfig(${i})">💾 保存</button>
  </div>
</div>`;
}

function buildCalibCard(i) {
  const pc  = P_CLASS[i];
  const enD = toDisp(-80);
  const exD = toDisp(-90);
  return `
<div class="calib-card" id="calibCard${i}">
  <div class="calib-header ${pc}">
    <span id="calibName${i}">Pilot ${i + 1}</span>
    <span class="rssi-live-badge" id="calibRssi${i}">--- dBm</span>
  </div>
  <canvas id="rssiChart${i}" height="130"></canvas>
  <div class="calib-sliders">
    <div class="config-row">
      <label>Enter 閾値</label>
      <div class="slider-num">
        <input type="range"  id="enterRssi${i}"  min="0" max="150" value="${enD}"
               oninput="onRssiSlider(${i},'enter')">
        <input type="number" id="enterRssiN${i}" min="-120" max="30" value="-80"
               oninput="onRssiNum(${i},'enter')">
        <span class="unit">dBm</span>
      </div>
    </div>
    <div class="config-row">
      <label>Exit 閾値</label>
      <div class="slider-num">
        <input type="range"  id="exitRssi${i}"  min="0" max="150" value="${exD}"
               oninput="onRssiSlider(${i},'exit')">
        <input type="number" id="exitRssiN${i}" min="-120" max="30" value="-90"
               oninput="onRssiNum(${i},'exit')">
        <span class="unit">dBm</span>
      </div>
    </div>
    <button class="btn-save" onclick="saveCalibConfig(${i})">💾 保存</button>
  </div>
</div>`;
}

function initCards() {
  const rg = el('raceGrid');
  const cg = el('configGrid');
  const kg = el('calibGrid');
  for (let i = 0; i < NUM_PILOTS; i++) {
    rg.insertAdjacentHTML('beforeend', buildRaceCard(i));
    cg.insertAdjacentHTML('beforeend', buildConfigCard(i));
    kg.insertAdjacentHTML('beforeend', buildCalibCard(i));
  }
}

// ── SmoothieChart ─────────────────────────────────────────────
function createRssiChart(i) {
  const canvas = el(`rssiChart${i}`);
  if (!canvas) return;

  const series = new TimeSeries();
  rssiSeries[i] = series;

  const color = P_COLOR[i];
  const chart  = new SmoothieChart({
    responsive      : true,
    millisPerPixel  : 50,
    minValue        : 0,
    maxValue        : 150,
    grid: {
      fillStyle        : '#161b22',
      strokeStyle      : '#30363d',
      lineWidth        : 1,
      millisPerLine    : 5000,
      verticalSections : 3,
    },
    labels          : { disabled: true },
    horizontalLines : [
      { value: toDisp(-80), color: color,                lineWidth: 1 },
      { value: toDisp(-90), color: hexRgba(color, 0.5), lineWidth: 1 },
    ],
  });

  chart.addTimeSeries(series, {
    strokeStyle : color,
    fillStyle   : hexRgba(color, 0.12),
    lineWidth   : 2,
  });
  chart.streamTo(canvas, 500);
  rssiCharts[i] = chart;
}

function updateChartLines(i) {
  const chart = rssiCharts[i];
  if (!chart) return;
  const en = toDisp(parseInt(el(`enterRssiN${i}`).value) || -80);
  const ex = toDisp(parseInt(el(`exitRssiN${i}`).value)  || -90);
  chart.options.horizontalLines = [
    { value: en, color: P_COLOR[i],                 lineWidth: 1 },
    { value: ex, color: hexRgba(P_COLOR[i], 0.5), lineWidth: 1 },
  ];
}

// ── WebSocket ─────────────────────────────────────────────────
function wsConnect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  try {
    ws = new WebSocket(`ws://${location.hostname}/ws`);
  } catch (_) {
    scheduleReconnect();
    return;
  }
  ws.onopen = () => {
    el('sseStatus').className = 'sse-dot connected';
    clearTimeout(wsReconnTimer);
    wsReconnTimer = null;
  };
  ws.onmessage = ({ data }) => {
    try {
      const parsed = JSON.parse(data);
      if (parsed.type === 'lap') handleLapEvent(parsed);
      else processData(parsed);
    } catch (_) {}
  };
  ws.onclose = ws.onerror = () => {
    el('sseStatus').className = 'sse-dot disconnected';
    scheduleReconnect();
  };
}

function scheduleReconnect() {
  if (!wsReconnTimer) {
    wsReconnTimer = setTimeout(() => { wsReconnTimer = null; wsConnect(); }, 3000);
  }
}

// ── Data Processing ───────────────────────────────────────────
function processData(data) {
  if (typeof data.minLapMs === 'number') {
    g_minLapMs = data.minLapMs;
    const sec = (g_minLapMs / 1000).toFixed(1);
    const gsl = el('globalMinLap');
    const gnl = el('globalMinLapN');
    if (gsl && document.activeElement !== gsl) gsl.value = sec;
    if (gnl && document.activeElement !== gnl) gnl.value = sec;
  }

  const pilots = Array.isArray(data.pilots) ? data.pilots : [];
  g_pilots = pilots;

  pilots.forEach(p => {
    const i = p.id;
    if (i < 0 || i >= NUM_PILOTS) return;

    const dispVal = toDisp(p.rssi);
    const barPct  = (dispVal / 150 * 100).toFixed(1) + '%';

    // Names
    const pn = el(`pname${i}`);     if (pn) pn.textContent = p.name;
    const cn = el(`calibName${i}`); if (cn) cn.textContent = p.name;

    // Config card (skip if user is editing)
    const cfgN = el(`cfgName${i}`);
    if (cfgN && document.activeElement !== cfgN) cfgN.value = p.name;
    const cfgM = el(`cfgMac${i}`);
    if (cfgM) cfgM.textContent = p.active ? p.mac : '未検出';
    // Show server UID only when user is not typing a bind phrase
    const cfgUidDisp = el(`cfgUidDisp${i}`);
    const phraseEl   = el(`cfgPhrase${i}`);
    if (cfgUidDisp && (!phraseEl || document.activeElement !== phraseEl)) {
      if (!phraseEl || !phraseEl.value.trim()) {
        cfgUidDisp.textContent = p.uid || '未設定';
      }
    }
    const pmac = el(`pmac${i}`);
    if (pmac) pmac.textContent = p.active ? p.mac.slice(-8) : '---';

    // RSSI display
    const bar   = el(`rssiBar${i}`);    if (bar)   bar.style.width    = barPct;
    const num   = el(`rssiNum${i}`);    if (num)   num.textContent    = `${p.rssi} dBm`;
    const crssi = el(`calibRssi${i}`);  if (crssi) crssi.textContent = `${p.rssi} dBm`;

    // SmoothieChart
    if (rssiSeries[i]) rssiSeries[i].append(Date.now(), dispVal);

    // Threshold sliders (skip if focused)
    const enS = el(`enterRssi${i}`), enN = el(`enterRssiN${i}`);
    if (enS && document.activeElement !== enS && document.activeElement !== enN) {
      enS.value = toDisp(p.enterAt);
      enN.value = p.enterAt;
    }
    const exS = el(`exitRssi${i}`), exN = el(`exitRssiN${i}`);
    if (exS && document.activeElement !== exS && document.activeElement !== exN) {
      exS.value = toDisp(p.exitAt);
      exN.value = p.exitAt;
    }

    // CLEAR → CROSSING: gate detection sound (220 Hz square, 0.05 s)
    if (!prevCrossing[i] && p.crossing) {
      beep(220, 0.05, 'square');
    }
    prevCrossing[i] = p.crossing;

    // Crossing visual indicator on race card
    const card = el(`raceCard${i}`);
    if (card) card.classList.toggle('crossing-active', p.crossing);
  });
}

// ── Lap event from server (Reader detected gate exit) ─────────
function handleLapEvent(evt) {
  const i = evt.pilot;
  if (i < 0 || i >= NUM_PILOTS || !raceActive) return;
  const nowMs = Date.now();
  if (nowMs - raceStartMs < g_minLapMs) return;
  addLap(i, nowMs - raceStartMs);
}

// ── Race Control ──────────────────────────────────────────────
function startRace() {
  if (raceActive) return;
  clearAllLaps();
  totalLapGoal = parseInt(el('totalLaps').value) || 0;
  showCountdown(() => {
    raceActive  = true;
    raceStartMs = Date.now();
    prevCrossing = [false, false, false, false];
    tick();
  });
}

function stopRace() {
  if (!raceActive) return;
  raceActive = false;
  if (raceRafId) { cancelAnimationFrame(raceRafId); raceRafId = null; }
  el('timer').textContent = '00:00:00';
  beepRaceEnd();
}

function clearAllLaps() {
  lapNos      = [0, 0, 0, 0];
  lapBest     = [Infinity, Infinity, Infinity, Infinity];
  lapTimesArr = [[], [], [], []];
  for (let i = 0; i < NUM_PILOTS; i++) {
    const tb = document.querySelector(`#lapTable${i} tbody`);
    if (tb) tb.innerHTML = '';
    const lc = el(`lapCount${i}`);   if (lc) lc.textContent = '0';
    const pb = el(`pilotBest${i}`);  if (pb) pb.textContent = '--:--.--';
    const pt = el(`pilotTotal${i}`); if (pt) pt.textContent = '0';
  }
}

function tick() {
  if (!raceActive) return;
  const ms = Date.now() - raceStartMs;
  const h  = Math.floor(ms / 3600000);
  const m  = Math.floor((ms % 3600000) / 60000);
  const s  = Math.floor((ms % 60000) / 1000);
  el('timer').textContent = `${pad(h)}:${pad(m)}:${pad(s)}`;
  raceRafId = requestAnimationFrame(tick);
}

function showCountdown(cb) {
  const overlay = el('countdownOverlay');
  const numEl   = el('countdownNum');
  overlay.style.display = 'flex';
  let cnt = 3;
  (function step() {
    numEl.className = '';
    void numEl.offsetWidth;
    if (cnt > 0) {
      numEl.textContent = cnt;
      numEl.classList.add('cd-anim');
      beep(440, 0.15);   // 3, 2, 1 beeps at 440 Hz
      cnt--;
      setTimeout(step, 1000);
    } else {
      numEl.textContent = 'GO!';
      numEl.classList.add('cd-anim', 'cd-go');
      beep(880, 0.4);    // GO at 880 Hz
      setTimeout(() => { overlay.style.display = 'none'; cb(); }, 700);
    }
  })();
}

// ── Lap Management ────────────────────────────────────────────
function addLap(i, raceElapsedMs) {
  const lapN    = ++lapNos[i];
  const times   = lapTimesArr[i];
  const prevSum = times.reduce((s, t) => s + t, 0);
  const lapMs   = raceElapsedMs - prevSum;
  times.push(lapMs);

  const prevBest = lapBest[i];
  const isBest   = lapMs < prevBest;
  if (isBest) lapBest[i] = lapMs;

  const diffStr = (prevBest === Infinity || isBest)
    ? '★'
    : `+${fmtLap(lapMs - prevBest)}`;

  const tbody = document.querySelector(`#lapTable${i} tbody`);
  if (tbody) {
    if (isBest) tbody.querySelectorAll('.best-row').forEach(r => r.classList.remove('best-row'));
    const tr = document.createElement('tr');
    if (isBest) tr.classList.add('best-row');
    tr.innerHTML = `<td>${lapN}</td><td class="lap-time">${fmtLap(lapMs)}</td><td>${diffStr}</td>`;
    tbody.insertBefore(tr, tbody.firstChild);
  }

  const lc = el(`lapCount${i}`);   if (lc) lc.textContent = lapN;
  const pb = el(`pilotBest${i}`);  if (pb) pb.textContent = fmtLap(lapBest[i]);
  const pt = el(`pilotTotal${i}`); if (pt) pt.textContent = lapN;

  // Lap flash animation on race card
  const card = el(`raceCard${i}`);
  if (card) {
    card.classList.remove('lap-flash');
    void card.offsetWidth;
    card.classList.add('lap-flash');
    card.addEventListener('animationend', () => card.classList.remove('lap-flash'), { once: true });
  }

  // Sound: ascending triad for best lap, single tone otherwise
  if (isBest) beepBestLap();
  else beep(880, 0.15);

  if (voiceOn) speakJa(`${lapN}周`);
  if (totalLapGoal > 0 && lapN >= totalLapGoal) stopRace();
}

// ── Slider Sync ───────────────────────────────────────────────
function syncSN(slId, numId) {
  const e = el(numId); if (e) e.value = el(slId).value;
}
function syncNS(numId, slId, minV, maxV) {
  const e = el(slId);
  if (e) e.value = clamp(parseFloat(el(numId).value) || minV, minV, maxV);
}

function onRssiSlider(i, type) {
  const prefix = type === 'enter' ? 'enter' : 'exit';
  el(`${prefix}RssiN${i}`).value = toDbm(parseInt(el(`${prefix}Rssi${i}`).value));
  updateChartLines(i);
}
function onRssiNum(i, type) {
  const prefix = type === 'enter' ? 'enter' : 'exit';
  el(`${prefix}Rssi${i}`).value = toDisp(parseInt(el(`${prefix}RssiN${i}`).value) || -80);
  updateChartLines(i);
}

// ── API ───────────────────────────────────────────────────────
async function apiFetch(url, method, body) {
  try {
    const res = await fetch(url, {
      method,
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      const j = await res.json().catch(() => ({}));
      showToast(j.error || `HTTP ${res.status}`, true);
      return false;
    }
    return true;
  } catch (_) {
    showToast('通信エラー', true);
    return false;
  }
}

// ── Settings ─────────────────────────────────────────────────
async function saveGlobalConfig() {
  const ms = Math.round(clamp(parseFloat(el('globalMinLapN').value) || 3, 0.5, 30) * 1000);
  if (await apiFetch('/api/settings', 'POST', { minLapMs: ms })) {
    g_minLapMs = ms;
    showToast('グローバル設定を保存しました');
  }
}

async function onPhraseInput(i) {
  const phrase  = (el(`cfgPhrase${i}`) || {}).value?.trim() || '';
  const dispEl  = el(`cfgUidDisp${i}`);
  if (!dispEl) return;
  dispEl.textContent = phrase ? await bindPhraseToUID(phrase) : '未設定';
}

async function savePilotConfig(i) {
  const nameEl   = el(`cfgName${i}`);
  const phraseEl = el(`cfgPhrase${i}`);
  const name = nameEl ? nameEl.value.trim() : '';
  if (!name) { showToast('名前を入力してください', true); return; }
  const phrase = phraseEl ? phraseEl.value.trim() : '';
  const uid = phrase ? await bindPhraseToUID(phrase) : '';
  if (await apiFetch('/api/pilots', 'POST', { id: i, name, uid })) {
    showToast(`Pilot ${i + 1} 保存しました`);
    const pn = el(`pname${i}`);     if (pn) pn.textContent = name;
    const cn = el(`calibName${i}`); if (cn) cn.textContent = name;
  }
}

async function clearUid(i) {
  const phraseEl = el(`cfgPhrase${i}`);
  if (phraseEl) phraseEl.value = '';
  const dispEl = el(`cfgUidDisp${i}`);
  if (dispEl) dispEl.textContent = '未設定';
  const name = el(`cfgName${i}`)?.value?.trim() || '';
  if (await apiFetch('/api/pilots', 'POST', { id: i, name, uid: '' })) {
    showToast(`Pilot ${i + 1} UID をクリアしました`);
  }
}

async function saveCalibConfig(i) {
  const en = parseInt(el(`enterRssiN${i}`).value);
  const ex = parseInt(el(`exitRssiN${i}`).value);
  if (isNaN(en) || isNaN(ex)) { showToast('値を入力してください', true); return; }
  if (ex >= en) { showToast('Exit閾値 < Enter閾値 にしてください', true); return; }
  if (await apiFetch('/api/thresholds', 'POST', { id: i, enterAt: en, exitAt: ex })) {
    showToast(`Pilot ${i + 1} 閾値を保存しました`);
    updateChartLines(i);
  }
}

async function resetAllPilots() {
  if (!confirm('全パイロットのデータをリセットしますか？この操作は取り消せません。')) return;
  if (await apiFetch('/api/reset', 'POST', {})) {
    clearAllLaps();
    showToast('リセットしました');
  }
}

// ── Voice / Sound ─────────────────────────────────────────────
function getAudioCtx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return audioCtx;
}

function beep(freq = 880, dur = 0.15, type = 'sine') {
  if (!voiceOn) return;
  try {
    const ctx  = getAudioCtx();
    const osc  = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = type;
    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.3, ctx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + dur);
    osc.start();
    osc.stop(ctx.currentTime + dur);
  } catch (_) {}
}

// Best lap: ascending triad 880 → 1320 → 1760 Hz
function beepBestLap() {
  if (!voiceOn) return;
  try {
    const ctx = getAudioCtx();
    [880, 1320, 1760].forEach((freq, i) => {
      const osc  = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.frequency.value = freq;
      const t = ctx.currentTime + i * 0.12;
      gain.gain.setValueAtTime(0.35, t);
      gain.gain.exponentialRampToValueAtTime(0.001, t + 0.1);
      osc.start(t);
      osc.stop(t + 0.1);
    });
  } catch (_) {}
}

// Race end: descending 880 → 440 → 220 Hz
function beepRaceEnd() {
  if (!voiceOn) return;
  try {
    const ctx = getAudioCtx();
    [880, 440, 220].forEach((freq, i) => {
      const osc  = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.frequency.value = freq;
      const t = ctx.currentTime + i * 0.18;
      gain.gain.setValueAtTime(0.35, t);
      gain.gain.exponentialRampToValueAtTime(0.001, t + 0.15);
      osc.start(t);
      osc.stop(t + 0.15);
    });
  } catch (_) {}
}

function speakJa(text) {
  if (!voiceOn || !window.speechSynthesis) return;
  const u = new SpeechSynthesisUtterance(text);
  u.lang = 'ja-JP';
  u.rate = 1.1;
  speechSynthesis.cancel();
  speechSynthesis.speak(u);
}

function toggleVoice() {
  voiceOn = !voiceOn;
  const btn = el('voiceToggleBtn');
  if (btn) {
    btn.textContent = voiceOn ? '🔊 音声 ON' : '🔇 音声 OFF';
    btn.classList.toggle('on', voiceOn);
  }
}

function testVoice() {
  beep(880, 0.1);
  speakJa('テスト');
}

// ── UI ────────────────────────────────────────────────────────
function switchTab(name) {
  document.querySelectorAll('.tab-pane').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  const pane = el(name); if (pane) pane.classList.add('active');
  const btn  = document.querySelector(`.tab-btn[data-tab="${name}"]`);
  if (btn) btn.classList.add('active');
}

let _toastTimer = null;
function showToast(msg, err = false) {
  const t = el('toast');
  if (!t) return;
  t.textContent = msg;
  t.style.borderColor = err ? 'var(--err)' : 'var(--ok)';
  t.classList.add('show');
  clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => t.classList.remove('show'), 2500);
}

// ── Init ─────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  initCards();
  for (let i = 0; i < NUM_PILOTS; i++) createRssiChart(i);
  switchTab('calib');
  wsConnect();
});
