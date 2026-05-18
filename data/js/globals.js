'use strict';

const PCOLORS = ['var(--p0)','var(--p1)','var(--p2)','var(--p3)'];
const PCLS    = ['p0','p1','p2','p3'];
const N = 4;

const slots = Array.from({length:N}, (_,i) => ({
  id:i, name:'---', yomi:'', rosterIdx:-1, rssi:-120, crossing:false,
  lapCount:0, bestLapMs:0, lapTimes:[], cumulative:0,
  enterRssi:-80, exitRssi:-90, rssiSignal:false
}));

var rosterData = [];
var rosterById = {}, rosterByUid = {};
function rebuildRosterIndex(){
  rosterById={};rosterByUid={};
  for(var i=0;i<rosterData.length;i++){
    var r=rosterData[i];
    rosterById[r.id]=r;
    if(r.uid)rosterByUid[r.uid.toUpperCase()]=r;
  }
}
var activeSlotsLocal = [-1,-1,-1,-1];

var raceRunning=false, raceStartPerf=0, timerH=null, countdownH=null;
var raceStarted=false, timerFrozenMs=0;
var voiceEnabled = localStorage.getItem('voice')!=='0';
var announceMode = localStorage.getItem('announce')||'lap_laptime';
var speechRate   = parseFloat(localStorage.getItem('srate')||'1.1');
var lapMode      = localStorage.getItem('lapMode')||'holeshot';
var cooldownMs   = parseInt(localStorage.getItem('cooldownMs')||'3000');
var sdLogMode    = localStorage.getItem('sdLogMode')||'always';
function sdLogModeInt(m){return m==='off'?2:(m==='rotate'?1:0);}

var scanResults  = {};
var editingRosterId = null;
var sdPresent = false;
var sdFileList = [];
var sdDownloadBuf = [], sdDownloadPath = '';
var calibSaveTimers = {};
var scanAutoRefreshH = null;

function toast(msg, dur) {
  dur=dur||2000;
  var t=document.getElementById('statusMsg');
  if(!t)return;
  t.textContent=msg;
  var isErr=msg.includes('⚠')||msg.includes('エラー')||msg.includes('失敗');
  if(isErr)t.classList.add('err'); else t.classList.remove('err');
  t.classList.add('show');
  clearTimeout(t._hideTimer);
  t._hideTimer=setTimeout(()=>t.classList.remove('show'),dur);
}

function pad(n,w){return String(n).padStart(w,'0');}
function fmt(ms){if(!ms)return'--:--.---';var m=Math.floor(ms/60000),s=Math.floor((ms%60000)/1000),f=ms%1000;return m+':'+pad(s,2)+'.'+pad(f,3);}
function fmtTimer(ms){var h=Math.floor(ms/3600000),m=Math.floor((ms%3600000)/60000),s=Math.floor((ms%60000)/1000),cs=Math.floor((ms%1000)/10);if(h>0)return pad(h,2)+':'+pad(m,2)+':'+pad(s,2)+'.'+pad(cs,2);return pad(m,2)+':'+pad(s,2)+'.'+pad(cs,2);}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function dbPct(db){return Math.max(0,Math.min(100,((db-(-120))/((-40)-(-120)))*100));}
function fmtDelta(ms){return(ms>0?'+':'')+(ms/1000).toFixed(3)+'s';}

function switchTab(tab){
  document.querySelectorAll('.tab-pane').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
  document.getElementById('pane-'+tab).classList.add('active');
  document.querySelectorAll('.tab-btn')[['race','config','calib','sd'].indexOf(tab)].classList.add('active');
  var sdActive=tab==='config'||tab==='sd';
  fetch('/api/sd/poll',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enable:sdActive})}).catch(()=>{});
  if(tab==='calib'){
    setTimeout(()=>{
      slots.forEach(p=>{
        var c=charts[p.id];if(!c)return;
        var newW=c.cv.offsetWidth||300;
        if(newW!==c.cv.width){c.cv.width=newW;}
        drawChart(p.id);
      });
    },50);
  }
  if(tab==='sd') refreshSdFiles();
  if(tab==='config'){
    if(!scanAutoRefreshH) scanAutoRefreshH=setInterval(()=>{
      scanRefresh();
      if(editingRosterId===null) renderRoster();
    },5000);
  } else {
    if(scanAutoRefreshH){clearInterval(scanAutoRefreshH);scanAutoRefreshH=null;}
  }
}
