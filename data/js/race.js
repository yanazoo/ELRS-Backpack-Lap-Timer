'use strict';

var timerEl=document.getElementById('timer');

function startTimer(){
  if(countdownH){clearInterval(countdownH);countdownH=null;}
  raceStartPerf=performance.now();
  timerEl.classList.add('running');
  if(timerH)clearInterval(timerH);
  timerH=setInterval(()=>{timerEl.textContent=fmtTimer(performance.now()-raceStartPerf);},50);
}
function stopTimer(){clearInterval(timerH);timerH=null;timerEl.classList.remove('running');}

function applyActiveToSlots(){
  for(var s=0;s<N;s++){
    var ri=activeSlotsLocal[s];
    var r=rosterData.find(x=>x.id===ri);
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
  });
}

function updateRaceCard(p){
  var $=id=>document.getElementById(id);
  $('rcName'+p.id).textContent=p.name==='---'?'Ch'+(p.id+1):p.name;
  $('rcLaps'+p.id).textContent=p.lapCount===1?'HS':p.lapCount?p.lapCount+'周':'';
  $('rssiBar'+p.id).style.width=(p.rssiSignal?dbPct(p.rssi):0)+'%';
  $('rssiVal'+p.id).textContent=p.rssiSignal?p.rssi+' dBm':'--- dBm';
  $('rcBest'+p.id).textContent=fmt(p.bestLapMs);
  var badge=$('rcBadge'+p.id);
  if(p.crossing){badge.style.display='inline-block';badge.style.color=PCOLORS[p.id];}
  else{badge.style.display='none';}
}

function addLapRow(p, lapMs, cumMs){
  var tbody=document.getElementById('lapBody'+p.id);
  var isBest=(p.bestLapMs===lapMs&&lapMs>0);
  if(isBest)tbody.querySelectorAll('tr.best-row').forEach(r=>r.classList.remove('best-row'));
  var tr=document.createElement('tr');
  if(isBest)tr.className='best-row';
  tr.innerHTML='<td>'+(p.lapCount===1?'HS':p.lapCount)+'</td><td class="lap-time">'+fmt(lapMs)+'</td><td>'+fmt(cumMs)+'</td>';
  tbody.insertBefore(tr,tbody.firstChild);
  var delta=document.getElementById('rcDelta'+p.id);
  if(p.bestLapMs&&lapMs){var d=lapMs-p.bestLapMs;if(d===0){delta.textContent='★ BEST';delta.className='pilot-delta faster';}else{delta.textContent=fmtDelta(d);delta.className='pilot-delta '+(d<0?'faster':'slower');}}
  var card=document.getElementById('rc-'+p.id);
  card.classList.remove('flash');void card.offsetWidth;card.classList.add('flash');
  card.addEventListener('animationend',()=>card.classList.remove('flash'),{once:true});
}

function startRace(){
  ensureAudio();
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
  setTimeout(async()=>{try{await fetch('/api/race/start',{method:'POST'});}catch(e){}},3300);
}
function stopRace(){
  ensureAudio();beepSeq([[880,.2,'sine',0],[440,.2,'sine',220],[220,.3,'sine',440]]);
  fetch('/api/race/stop',{method:'POST'}).catch(()=>{});
}
function clearAllLaps(){
  slots.forEach(p=>{
    p.lapCount=0;p.bestLapMs=0;p.lapTimes=[];p.cumulative=0;
    var b=document.getElementById('lapBody'+p.id);if(b)b.innerHTML='';
    var d=document.getElementById('rcDelta'+p.id);if(d)d.textContent='';
    updateRaceCard(p);
  });
}

function setBtns(running){
  document.getElementById('btnStart').disabled=running;
  document.getElementById('btnStop').disabled=!running;
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
