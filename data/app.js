'use strict';

// ── Constants ─────────────────────────────────────────────────
const NUM_PILOTS = 4;
const P_CLASS    = ['p0', 'p1', 'p2', 'p3'];
const P_COLOR    = ['#79c0ff', '#ff7b72', '#56d364', '#e3b341'];

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

let g_roster     = [];  // pilot roster (up to 100)

// ── Utilities ─────────────────────────────────────────────────
const el      = id => document.getElementById(id);
const clamp   = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const escHtml = s => String(s)
  .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
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

// ── MD5 (RFC 1321) — pure JS, works over plain HTTP ──────────
// crypto.subtle.digest() is unavailable on http:// origins, and ELRS
// actually derives UID via MD5 (not SHA-256). Returns Uint8Array(16).
function md5(str) {
  const bytes = new TextEncoder().encode(str);
  const len = bytes.length;
  const padLen = ((len + 8) >>> 6 << 6) + 64;
  const padded = new Uint8Array(padLen);
  padded.set(bytes);
  padded[len] = 0x80;
  const view = new DataView(padded.buffer);
  const bitLen = len * 8;
  view.setUint32(padLen - 8, bitLen >>> 0, true);
  view.setUint32(padLen - 4, Math.floor(bitLen / 0x100000000) >>> 0, true);

  const T = [
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
  ];
  const S = [
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
  ];

  let a=0x67452301, b=0xefcdab89, c=0x98badcfe, d=0x10325476;
  for (let blk = 0; blk < padLen; blk += 64) {
    const M = new Uint32Array(16);
    for (let i = 0; i < 16; i++) M[i] = view.getUint32(blk + i*4, true);
    let A=a, B=b, C=c, D=d;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16)      { F = (B & C) | (~B & D);     g = i; }
      else if (i < 32) { F = (D & B) | (~D & C);     g = (5*i + 1) & 15; }
      else if (i < 48) { F = B ^ C ^ D;              g = (3*i + 5) & 15; }
      else             { F = C ^ (B | ~D);           g = (7*i) & 15; }
      const tmp = D;
      D = C; C = B;
      const x = (A + F + T[i] + M[g]) >>> 0;
      B = (B + ((x << S[i]) | (x >>> (32 - S[i])))) >>> 0;
      A = tmp;
    }
    a = (a + A) >>> 0;
    b = (b + B) >>> 0;
    c = (c + C) >>> 0;
    d = (d + D) >>> 0;
  }

  const out = new Uint8Array(16);
  const ov = new DataView(out.buffer);
  ov.setUint32(0, a, true); ov.setUint32(4, b, true);
  ov.setUint32(8, c, true); ov.setUint32(12, d, true);
  return out;
}

// ELRS UID = first 6 bytes of MD5(bindPhrase). Matches expresslrs.github.io/web-flasher
function bindPhraseToUID(phrase) {
  if (!phrase) return '';
  const hash = md5(phrase);
  return Array.from(hash.slice(0, 6))
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

  // Detected devices (from Reader, for diagnostic UI)
  if (Array.isArray(data.detected)) renderDetected(data.detected);

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

// ── Detected devices panel ───────────────────────────────────
function renderDetected(list) {
  const wrap = el('detectedList');
  if (!wrap) return;
  if (!list.length) {
    wrap.innerHTML = '<div class="detected-empty">未受信。Reader電源・WiFiチャンネル・ELRSバックパック設定を確認してください。</div>';
    return;
  }
  const sorted = list.slice().sort((a, b) => b.rssi - a.rssi);
  wrap.innerHTML = sorted.map(d => {
    // 各スロットに割り当て済みか確認
    const slotBtns = [0,1,2,3].map(s => {
      const assigned = g_pilots[s]?.uid === d.mac;
      return `<button class="p${s}${assigned ? ' slot-active' : ''}"
                onclick="toggleDetectedSlot('${d.mac}',${s})"
                title="${assigned ? 'スロット'+(s+1)+'から外す' : 'スロット'+(s+1)+'に割り当て'}">
                P${s+1}${assigned ? '✓' : ''}</button>`;
    }).join('');
    return `
    <div class="detected-row">
      <span class="detected-mac">${escHtml(d.mac)}</span>
      <span class="detected-rssi">${d.rssi} dBm</span>
      <span class="detected-age">${Math.round((d.age || 0) / 1000)}s</span>
      <span class="detected-assign">${slotBtns}</span>
      <button class="detected-add-roster" onclick="openAddRoster('${d.mac}')" title="ロスターに追加">＋</button>
      <button class="detected-del" onclick="deleteDetected('${d.mac}')" title="削除">✕</button>
    </div>`;
  }).join('');
}

async function setChannel(ch) {
  if (await apiFetch('/api/channel', 'POST', { channel: parseInt(ch) })) {
    showToast(`WiFiチャンネル ${ch} に変更しました`);
  }
}

async function toggleDetectedSlot(mac, slot) {
  const already = g_pilots[slot]?.uid === mac;
  if (already) {
    const name = g_pilots[slot]?.name || `Pilot ${slot + 1}`;
    if (await apiFetch('/api/pilots', 'POST', { id: slot, name, uid: '' }))
      showToast(`スロット${slot + 1}の割り当てを解除しました`);
  } else {
    const name = el(`cfgName${slot}`)?.value?.trim() || g_pilots[slot]?.name || `Pilot ${slot + 1}`;
    if (await apiFetch('/api/pilots', 'POST', { id: slot, name, uid: mac }))
      showToast(`${mac} をP${slot + 1}に割り当てました`);
  }
}

async function deleteDetected(mac) {
  const enc = encodeURIComponent(mac);
  if (await apiFetch(`/api/detected?mac=${enc}`, 'DELETE'))
    showToast('検出リストから削除しました');
}

// ── Pilot Roster ──────────────────────────────────────────────
async function loadRoster() {
  try {
    const res = await fetch('/api/roster');
    if (!res.ok) return;
    const data = await res.json();
    g_roster = data.pilots || [];
    renderRoster();
  } catch (_) {}
}

function filterRoster() { renderRoster(); }

function renderRoster() {
  const wrap = el('rosterList');
  if (!wrap) return;
  const q = (el('rosterSearch')?.value || '').toLowerCase();
  const items = g_roster.filter(p => !q || p.name.toLowerCase().includes(q));

  const count = el('rosterCount');
  if (count) count.textContent = `${g_roster.length}/100`;

  if (!items.length) {
    wrap.innerHTML = `<div class="roster-empty">${q ? '該当なし' : 'パイロット未登録'}</div>`;
    return;
  }
  wrap.innerHTML = items.map(p => {
    const uid = p.uid || '';
    const uidDisp = p.phrase
      ? `<span title="${escHtml(p.phrase)}">🔑 ${escHtml(p.phrase.slice(0,14))}${p.phrase.length>14?'…':''}</span>`
      : `<span>${escHtml(uid) || '未設定'}</span>`;
    const slotBtns = [0,1,2,3].map(s => {
      const active = uid && g_pilots[s]?.uid === uid;
      return `<button class="roster-slot-btn p${s}${active?' active-slot':''}"
                onclick="toggleRosterSlot(${p.id},${s})"
                title="${active?'スロット'+(s+1)+'から外す':'スロット'+(s+1)+'に割り当て'}">
                S${s+1}${active?'✓':''}</button>`;
    }).join('');
    return `
    <div class="roster-row" id="rosterRow${p.id}">
      <span class="roster-name">${escHtml(p.name)}</span>
      <span class="roster-uid">${uidDisp}</span>
      <span class="roster-slots">${slotBtns}</span>
      <span class="roster-actions">
        <button class="roster-edit-btn" onclick="editRoster(${p.id})" title="編集">✎</button>
        <button class="roster-del-btn"  onclick="deleteRoster(${p.id})" title="削除">✕</button>
      </span>
    </div>`;
  }).join('');
}

// ── ロスター追加フォーム ──────────────────────────────────────
let _editRosterId = -1;

function openAddRoster(prefillMac) {
  _editRosterId = -1;
  el('rosterFormTitle').textContent = '新規パイロット追加';
  el('rosterFormName').value   = '';
  el('rosterFormPhrase').value = '';
  el('rosterFormMac').value    = prefillMac || '';
  el('rosterFormUidPreview').textContent = prefillMac ? 'UID: ' + prefillMac : 'UID: --';
  el('rosterAddForm').classList.remove('hidden');
  el('rosterFormName').focus();
}

function editRoster(id) {
  const p = g_roster.find(r => r.id === id);
  if (!p) return;
  _editRosterId = id;
  el('rosterFormTitle').textContent = '編集: ' + p.name;
  el('rosterFormName').value   = p.name;
  el('rosterFormPhrase').value = p.phrase || '';
  el('rosterFormMac').value    = (!p.phrase && p.uid) ? p.uid : '';
  el('rosterFormUidPreview').textContent = 'UID: ' + (p.uid || '--');
  el('rosterAddForm').classList.remove('hidden');
  el('rosterFormName').focus();
}

function closeAddRoster() {
  el('rosterAddForm').classList.add('hidden');
  _editRosterId = -1;
}

function updateRosterUidPreview() {
  const phrase = el('rosterFormPhrase').value.trim();
  const mac    = el('rosterFormMac').value.trim();
  if (phrase) {
    el('rosterFormUidPreview').textContent = 'UID: ' + bindPhraseToUID(phrase);
  } else if (mac) {
    el('rosterFormUidPreview').textContent = 'UID: ' + mac;
  } else {
    el('rosterFormUidPreview').textContent = 'UID: --';
  }
}

async function saveRosterForm() {
  const name   = el('rosterFormName').value.trim();
  const phrase = el('rosterFormPhrase').value.trim();
  const mac    = el('rosterFormMac').value.trim();
  if (!name) { showToast('名前を入力してください', true); return; }

  // バインドフレーズがある場合は優先してUIDを計算
  let uid = '';
  if (phrase)     uid = bindPhraseToUID(phrase);
  else if (mac)   uid = mac;

  const body = { name };
  if (phrase) body.phrase = phrase;
  if (uid)    body.uid    = uid;
  if (_editRosterId >= 0) body.id = _editRosterId;

  const res = await fetch('/api/roster', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (res.ok) {
    showToast(_editRosterId >= 0 ? '更新しました' : `${name} を登録しました`);
    closeAddRoster();
    await loadRoster();
  } else {
    const j = await res.json().catch(() => ({}));
    showToast(j.error || '保存失敗', true);
  }
}

async function deleteRoster(id) {
  if (!confirm('このパイロットをロスターから削除しますか？')) return;
  if (await apiFetch(`/api/roster?id=${id}`, 'DELETE')) {
    g_roster = g_roster.filter(p => p.id !== id);
    renderRoster();
    showToast('削除しました');
  }
}

async function toggleRosterSlot(rosterId, slot) {
  const p = g_roster.find(r => r.id === rosterId);
  if (!p || !p.uid) { showToast('UIDが未設定です', true); return; }
  const already = g_pilots[slot]?.uid === p.uid;
  if (already) {
    const name = g_pilots[slot]?.name || `Pilot ${slot + 1}`;
    if (await apiFetch('/api/pilots', 'POST', { id: slot, name, uid: '' }))
      showToast(`スロット${slot + 1}の割り当てを解除しました`);
  } else {
    if (await apiFetch('/api/pilots', 'POST', { id: slot, name: p.name, uid: p.uid }))
      showToast(`${p.name} をS${slot + 1}に割り当てました`);
  }
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
  loadRoster();
});
