'use strict';

var timerEl=document.getElementById('timer');

function startTimer(){
  if(countdownH){clearInterval(countdownH);countdownH=null;}
  timerFrozenMs=0;raceStartPerf=performance.now();
  timerEl.classList.add('running');
  if(timerH)clearInterval(timerH);
  timerH=setInterval(()=>{timerEl.textContent=fmtTimer(performance.now()-raceStartPerf);},50);
}
function resumeTimer(){
  if(countdownH){clearInterval(countdownH);countdownH=null;}
  raceStartPerf=performance.now()-timerFrozenMs;
  timerEl.classList.add('running');
  if(timerH)clearInterval(timerH);
  timerH=setInterval(()=>{timerEl.textContent=fmtTimer(performance.now()-raceStartPerf);},50);
}
function stopTimer(){
  if(timerH){timerFrozenMs=performance.now()-raceStartPerf;clearInterval(timerH);timerH=null;}
  timerEl.classList.remove('running');
}

function applyActiveToSlots(){
  for(var s=0;s<N;s++){
    var ri=activeSlotsLocal[s];
    var r=rosterById[ri];
    slots[s].rosterIdx=ri;
    slots[s].name=r?r.name:'---';
    slots[s].yomi=r?r.yomi:'';
    slots[s].enterRssi=r?r.enter:-80;
    slots[s].exitRssi =r?r.exit :-90;
  }
}

function buildRaceCards(){
  var g=document.getElementById('pilotGrid');g.innerHTML='';
  slots.forEach(p=>{
    var d=document.createElement('div');
    d.id='rc-'+p.id;d.className='pilot-race-card';
    d.innerHTML=
      '<div class="pilot-card-header '+PCLS[p.id]+'">'
        +'<span class="p-name" id="rcName'+p.id+'">'+esc(p.name==='---'?'Ch'+(p.id+1):p.name)+'</span>'
        +'<span class="crossing-badge" id="rcBadge'+p.id+'">CROSSING</span>'
        +'<span class="p-lapcount" id="rcLaps'+p.id+'"></span>'
      +'</div>'
      +'<div class="rssi-mini-bar">'
        +'<span class="rssi-mini-label">RSSI</span>'
        +'<div class="rssi-mini-track"><div class="rssi-mini-fill '+PCLS[p.id]+'-fill" id="rssiBar'+p.id+'"></div></div>'
        +'<span class="rssi-mini-val" id="rssiVal'+p.id+'">--- dBm</span>'
      +'</div>'
      +'<div class="pilot-best-row">'
        +'<span class="best-label">BEST</span>'
        +'<span class="pilot-best" id="rcBest'+p.id+'">--:--.---</span>'
        +'<span class="pilot-delta" id="rcDelta'+p.id+'"></span>'
      +'</div>'
      +'<div class="lap-table-wrap">'
        +'<table class="lapTable"><thead><tr><th>周</th><th>タイム</th><th>累計</th></tr></thead>'
        +'<tbody id="lapBody'+p.id+'"></tbody></table>'
      +'</div>';
    g.appendChild(d);
    // Cache DOM refs once so the hot RSSI path avoids repeated getElementById
    p.el={
      card : d,
      name : document.getElementById('rcName'+p.id),
      laps : document.getElementById('rcLaps'+p.id),
      bar  : document.getElementById('rssiBar'+p.id),
      val  : document.getElementById('rssiVal'+p.id),
      best : document.getElementById('rcBest'+p.id),
      badge: document.getElementById('rcBadge'+p.id),
      delta: document.getElementById('rcDelta'+p.id),
      body : document.getElementById('lapBody'+p.id)
    };
  });
}

function updateRaceCard(p){
  var e=p.el;if(!e)return;
  var nm=p.name==='---'?'Ch'+(p.id+1):p.name;
  if(e._name!==nm){e.name.textContent=nm;e._name=nm;}
  var _lbl=lapMode==='immediate'?(p.lapCount?p.lapCount+'周':''):(p.lapCount===1?'HS':p.lapCount?(p.lapCount-1)+'周':'');
  if(e._lbl!==_lbl){e.laps.textContent=_lbl;e._lbl=_lbl;}
  var _w=(p.rssiSignal?dbPct(p.rssi):0);
  if(e._w!==_w){e.bar.style.width=_w+'%';e._w=_w;}
  var _val=p.rssiSignal?p.rssi+' dBm':'--- dBm';
  if(e._val!==_val){e.val.textContent=_val;e._val=_val;}
  var _best=fmt(p.bestLapMs);
  if(e._best!==_best){e.best.textContent=_best;e._best=_best;}
  if(e._cross!==p.crossing){
    if(p.crossing){e.badge.style.display='inline-block';e.badge.style.color=PCOLORS[p.id];}
    else{e.badge.style.display='none';}
    e._cross=p.crossing;
  }
}

function addLapRow(p, lapMs, cumMs){
  var e=p.el,tbody=e?e.body:document.getElementById('lapBody'+p.id);
  var isBest=(p.bestLapMs===lapMs&&lapMs>0);
  if(isBest)tbody.querySelectorAll('tr.best-row').forEach(r=>r.classList.remove('best-row'));
  var tr=document.createElement('tr');
  if(isBest)tr.className='best-row';
  var _lap=lapMode==='immediate'?p.lapCount:(p.lapCount===1?'HS':(p.lapCount-1));
  tr.innerHTML='<td>'+_lap+'</td><td class="lap-time">'+fmt(lapMs)+'</td><td>'+fmt(cumMs)+'</td>';
  tbody.insertBefore(tr,tbody.firstChild);
  var delta=e?e.delta:document.getElementById('rcDelta'+p.id);
  if(p.bestLapMs&&lapMs){var d=lapMs-p.bestLapMs;if(d===0){delta.textContent='★ BEST';delta.className='pilot-delta faster';}else{delta.textContent=fmtDelta(d);delta.className='pilot-delta '+(d<0?'faster':'slower');}}
  var card=e?e.card:document.getElementById('rc-'+p.id);
  card.classList.remove('flash');void card.offsetWidth;card.classList.add('flash');
  card.addEventListener('animationend',()=>card.classList.remove('flash'),{once:true});
}

function startRace(){
  ensureAudio();warmUpSpeech();
  var btnStart=document.getElementById('btnStart');
  var btnClear=document.getElementById('btnClear');
  btnStart.disabled=true;
  if(btnClear)btnClear.disabled=true;
  // Resume a paused race (Stop → Start) without countdown or reset.
  if(raceStarted && !raceRunning){
    fetch('/api/race/resume',{method:'POST'})
      .catch(e=>{btnStart.disabled=false;if(btnClear)btnClear.disabled=false;});
    return;
  }
  sfx.count();
  var cdStart=performance.now();
  timerEl.classList.remove('running');
  timerEl.textContent='3.00';
  if(countdownH)clearInterval(countdownH);
  countdownH=setInterval(()=>{
    var rem=Math.max(0,3000-(performance.now()-cdStart));
    timerEl.textContent=(rem/1000).toFixed(2);
    if(rem===0){clearInterval(countdownH);countdownH=null;}
  },50);
  setTimeout(async()=>{
    try{await fetch('/api/race/start',{method:'POST'});}
    catch(e){btnStart.disabled=false;}
  },3300);
}
function stopRace(){
  ensureAudio();beepSeq([[880,.2,'sine',0],[440,.2,'sine',220],[220,.3,'sine',440]]);
  fetch('/api/race/stop',{method:'POST'}).catch(()=>{});
}
async function clearAllLaps(){
  var hasData=slots.some(p=>p.lapCount>0);
  if(hasData){
    try{
      var r=await fetch('/api/race/save',{method:'POST'});
      if(r.ok){
        var j=await r.json();
        if(j.saved)toast('✅ レース結果をSDに保存しました');
        else if(j.reason==='off')toast('ℹ️ SDログOFF — 未保存のままクリアしました');
        else if(j.reason==='empty')toast('記録なし');
        else toast('⚠️ SDカードなし — 未保存のままクリアしました',3500);
      }
      else toast('⚠️ SD保存エラー');
    }catch(e){toast('⚠️ SD保存 接続エラー');}
  }
  stopTimer();
  timerFrozenMs=0;
  timerEl.textContent='00:00';
  slots.forEach(p=>{
    p.lapCount=0;p.bestLapMs=0;p.lapTimes=[];p.cumulative=0;
    var e=p.el;
    var b=e?e.body:document.getElementById('lapBody'+p.id);if(b)b.innerHTML='';
    var d=e?e.delta:document.getElementById('rcDelta'+p.id);if(d)d.textContent='';
    updateRaceCard(p);
  });
  // Clear ends the race; next Start is a fresh race.
  raceStarted=false;
  setBtns(false);
}

function setBtns(running){
  document.getElementById('btnStart').disabled=running;
  document.getElementById('btnStop').disabled=!running;
  // Clear is only allowed while stopped (not during a running race).
  var btnClear=document.getElementById('btnClear');
  if(btnClear)btnClear.disabled=running;
}
function checkFinished(p){
  var tot=parseInt(document.getElementById('totalLaps').value)||0;
  if(!raceRunning||tot===0)return;
  if(p.lapCount>=tot){
    beepSeq([[880,.2,'sine',0],[1320,.2,'sine',220],[1760,.3,'sine',440]]);
    if(voiceEnabled&&announceMode!=='beep'&&announceMode!=='none'){
      var u=new SpeechSynthesisUtterance(getSpokenName(p)+'、フィニッシュ');u.lang='ja-JP';u.rate=speechRate;speechSynthesis.speak(u);
    }
  }
}
