'use strict';

var wsConn=null,wsRetry=1000;
var wsDot=document.getElementById('wsDot');

function wsConnect(){
  wsConn=new WebSocket('ws://'+location.host+'/ws');
  wsConn.onopen=()=>{wsRetry=1000;wsDot.className='ws-dot connected';};
  wsConn.onerror=()=>wsConn.close();
  wsConn.onclose=()=>{wsDot.className='ws-dot disconnected';setTimeout(()=>{wsRetry=Math.min(wsRetry*2,16000);wsConnect();},wsRetry);};
  wsConn.onmessage=e=>{var d;try{d=JSON.parse(e.data);}catch(ex){return;}onMsg(d);};
}

function onMsg(d){
  if(d.type==='init'){
    raceRunning=d.raceRunning||false;setBtns(raceRunning);
    if(raceRunning)startTimer();
    if(d.sdPresent!==undefined)updateSdSection(d.sdPresent);
    (d.pilots||[]).forEach(pd=>{
      var p=slots[pd.slot];if(!p)return;
      p.name=pd.name||'---';p.yomi=pd.yomi||'';p.rosterIdx=pd.rosterIdx;
      if(pd.lapCount)p.lapCount=pd.lapCount;
      if(pd.bestLap)p.bestLapMs=pd.bestLap;
      p.rssi=pd.rssi||-120;p.crossing=pd.crossing||false;
      if(pd.enter!==undefined){p.enterRssi=pd.enter;syncCalibSliders(pd.slot,pd.enter,p.exitRssi);}
      if(pd.exit!==undefined){p.exitRssi=pd.exit;syncCalibSliders(pd.slot,p.enterRssi,pd.exit);}
      activeSlotsLocal[pd.slot]=pd.rosterIdx;
      updateRaceCard(p);pushChart(p.id,p.rssi,p.crossing);
    });
    return;
  }
  if(d.type==='active_update'){
    (d.pilots||[]).forEach(pd=>{
      var p=slots[pd.slot];if(!p)return;
      p.name=pd.name||'---';p.yomi=pd.yomi||'';p.rosterIdx=pd.rosterIdx;
      if(pd.enter!==undefined){p.enterRssi=pd.enter;syncCalibSliders(pd.slot,pd.enter,p.exitRssi);}
      if(pd.exit!==undefined){p.exitRssi=pd.exit;syncCalibSliders(pd.slot,p.enterRssi,pd.exit);}
      activeSlotsLocal[pd.slot]=pd.rosterIdx;
      updateRaceCard(p);
    });
    buildCalibCards();
    return;
  }
  if(d.type==='scan'){
    var prev=scanResults[d.mac]||{};
    scanResults[d.mac]=Object.assign(prev,{rssi:d.rssi,ts:d.ts,receivedAt:Date.now()});
    var found=rosterData.find(r=>r.uid&&r.uid.toUpperCase()===d.mac.toUpperCase());
    if(found){scanResults[d.mac].assignedRosterId=found.id;scanResults[d.mac].pilotName=found.name;}
    updateScanList();return;
  }
  if(d.type==='rssi'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
    var prevCrossing=p.crossing;
    p.rssiSignal=d.signal!==false;
    p.rssi=p.rssiSignal?(d.rssi!==undefined?d.rssi:p.rssi):-120;
    p.crossing=p.rssiSignal&&(d.crossing!==undefined?d.crossing:p.crossing);
    if(d.name&&d.name!=='---')p.name=d.name;
    var calibActive=document.getElementById('pane-calib').classList.contains('active');
    if(raceRunning||calibActive){
      if(!prevCrossing&&p.crossing){ensureAudio();sfx.enter();}
      if(prevCrossing&&!p.crossing){ensureAudio();sfx.exit();}
    }
    updateRaceCard(p);
    var calR=document.getElementById('calRssi'+s);if(calR)calR.textContent=p.rssiSignal?p.rssi:'---';
    pushChart(s,p.rssiSignal?p.rssi:-120,p.crossing);
    return;
  }
  if(d.type==='gate_start'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
    if(d.name&&d.name!=='---')p.name=d.name;
    ensureAudio();sfx.enter();
    updateRaceCard(p);
    return;
  }
  if(d.type==='lap'){
    var s=d.pilot;if(s<0||s>=N)return;
    var p=slots[s];
    if(d.name&&d.name!=='---')p.name=d.name;
    if(d.yomi!==undefined)p.yomi=d.yomi;
    p.lapCount=d.lapCount!==undefined?d.lapCount:p.lapCount+1;
    var lapMs=d.lapTime||0;
    p.lapTimes.push(lapMs);p.cumulative+=lapMs;
    if(lapMs>0&&(!p.bestLapMs||lapMs<p.bestLapMs))p.bestLapMs=lapMs;
    updateRaceCard(p);addLapRow(p,lapMs,p.cumulative);checkFinished(p);
    if(d.newBest||lapMs===p.bestLapMs)sfx.best();
    speak(buildSpeech(p,p.lapCount,lapMs));
    return;
  }
  if(d.type==='race_start'){
    raceRunning=true;setBtns(true);startTimer();
    slots.forEach(p=>{p.lapCount=0;p.bestLapMs=0;p.lapTimes=[];p.cumulative=0;
      document.getElementById('lapBody'+p.id).innerHTML='';
      document.getElementById('rcDelta'+p.id).textContent='';
      updateRaceCard(p);});
    return;
  }
  if(d.type==='race_stop'){raceRunning=false;setBtns(false);stopTimer();return;}
  if(d.type==='sd_restore_done'){
    toast('✅ SDから'+((d.pilots&&JSON.parse(d.pilots).length)||'?')+'人分を復元しました',3000);
    loadRoster();return;
  }
  if(d.type==='sd_file_list'){sdFileList=d.files||[];renderSdFileList();return;}
  if(d.type==='sd_file_line'){if(d.path===sdDownloadPath)sdDownloadBuf.push(d.line);return;}
  if(d.type==='sd_file_done'){
    if(d.path===sdDownloadPath&&sdDownloadBuf.length){
      var blob=new Blob([sdDownloadBuf.join('\n')+'\n'],{type:'text/csv'});
      var a=document.createElement('a');
      a.href=URL.createObjectURL(blob);a.download=d.path.replace(/^\//,'');
      document.body.appendChild(a);a.click();document.body.removeChild(a);
      URL.revokeObjectURL(a.href);
      toast('✅ ダウンロード完了: '+a.download);
    }
    sdDownloadBuf=[];sdDownloadPath='';return;
  }
  if(d.type==='sd_delete_result'){
    if(d.ok){
      toast('🗑 削除完了: '+d.path);
      var delName=d.path.replace(/^\//,'');
      sdFileList=sdFileList.filter(function(f){return f.name!==delName;});
      renderSdFileList();
    }else{toast('削除失敗: '+d.path);}
    return;
  }
}

async function loadRoster(){
  try{
    var r=await fetch('/api/pilots');if(!r.ok)return;
    rosterData=await r.json();
    renderRoster();applyActiveToSlots();buildRaceCards();
    Object.keys(scanResults).forEach(function(mac){
      var found=rosterData.find(r=>r.uid&&r.uid.toUpperCase()===mac.toUpperCase());
      if(found){scanResults[mac].assignedRosterId=found.id;scanResults[mac].pilotName=found.name;}
    });
    updateScanList();
  }catch(e){}
}

async function loadAll(){
  try{
    var rs=await fetch('/api/sd/status');
    if(rs.ok){var sd=await rs.json();updateSdSection(sd.present);}
  }catch(e){}

  try{
    var ra=await fetch('/api/active');if(ra.ok){
      var active=await ra.json();
      active.forEach(a=>{
        activeSlotsLocal[a.slot]=a.rosterIdx;
        var p=slots[a.slot];p.rosterIdx=a.rosterIdx;p.name=a.name||'---';p.yomi=a.yomi||'';
        if(a.enter!==undefined)p.enterRssi=a.enter;
        if(a.exit!==undefined)p.exitRssi=a.exit;
      });
    }
  }catch(e){}

  try{
    var rp=await fetch('/api/pilots');if(rp.ok){
      rosterData=await rp.json();
      renderRoster();applyActiveToSlots();buildRaceCards();buildCalibCards();
    }
  }catch(e){}

  try{
    var rc=await fetch('/api/active');if(rc.ok){
      var ac=await rc.json();
      ac.forEach(a=>{
        var p=slots[a.slot];
        if(a.enter!==undefined){p.enterRssi=a.enter;syncCalibSliders(a.slot,a.enter,p.exitRssi);}
        if(a.exit!==undefined){p.exitRssi=a.exit;syncCalibSliders(a.slot,p.enterRssi,a.exit);}
      });
    }
  }catch(e){}

  try{
    var rsc=await fetch('/api/scan');if(rsc.ok){
      (await rsc.json()).forEach(s=>{
        scanResults[s.mac]=Object.assign(scanResults[s.mac]||{},{rssi:s.rssi,ts:s.ts});
        var found=rosterData.find(r=>r.uid&&r.uid.toUpperCase()===s.mac.toUpperCase());
        if(found){scanResults[s.mac].assignedRosterId=found.id;scanResults[s.mac].pilotName=found.name;}
      });
      updateScanList();
    }
  }catch(e){}

  var am=document.getElementById('announceMode');if(am)am.value=announceMode;
  var sr=document.getElementById('speechRate');if(sr)sr.value=speechRate;
  var srN=document.getElementById('speechRateN');if(srN)srN.value=speechRate;
  var lms=document.getElementById('lapModeSelect');if(lms)lms.value=lapMode;
  var cdEl=document.getElementById('cooldownInput');if(cdEl)cdEl.value=(cooldownMs/1000).toFixed(1);
  refreshVoiceBtns();
  slots.forEach(p=>updateRaceCard(p));

  // Push current settings to web node so it stays in sync after page reload
  try{
    await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({lapMode:lapMode==='immediate'?1:0,cooldownMs:cooldownMs})});
  }catch(e){}
}

// App init
buildRaceCards();
buildCalibCards();
wsConnect();
loadAll();
