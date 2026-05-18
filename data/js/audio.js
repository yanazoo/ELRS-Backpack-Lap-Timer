'use strict';

var actx=null;
function ensureAudio(){
  if(!actx)actx=new(window.AudioContext||window.webkitAudioContext)();
  if(actx.state==='suspended')actx.resume();
  warmUpSpeech();
}
// Layered voice: fundamental + chorus-detuned twin + sub-octave (body) +
// soft upper harmonic (presence), routed through a lowpass for warmth and
// a click-free attack / smooth exponential release. Same signature as before
// so existing call sites get the deeper, richer tone automatically.
function beep(freq,dur,type,vol){
  if(!actx)return;
  type=type||'sine';
  vol=(vol===undefined?0.4:vol);
  var t=actx.currentTime;
  var g=actx.createGain();
  var lp=actx.createBiquadFilter();
  lp.type='lowpass';
  lp.frequency.value=Math.min(6000,freq*6+700);
  lp.Q.value=0.7;
  g.connect(lp);lp.connect(actx.destination);
  var atk=Math.min(0.012,dur*0.3);
  g.gain.setValueAtTime(0.0001,t);
  g.gain.exponentialRampToValueAtTime(vol,t+atk);
  g.gain.exponentialRampToValueAtTime(0.0008,t+dur);
  function layer(f,wave,lvl,detune){
    var o=actx.createOscillator(),og=actx.createGain();
    o.type=wave;o.frequency.value=f;
    if(detune)o.detune.value=detune;
    og.gain.value=lvl;
    o.connect(og);og.connect(g);
    o.start(t);o.stop(t+dur+0.03);
  }
  layer(freq,type,0.42,0);       // fundamental
  layer(freq,type,0.20,7);       // detuned twin -> thickness
  layer(freq/2,'sine',0.45,0);   // sub-octave -> depth/body
  layer(freq*2,'sine',0.10,0);   // soft harmonic -> presence
}
function beepSeq(notes){notes.forEach(n=>setTimeout(()=>beep(n[0],n[1],n[2]),n[3]||0));}

var sfx={
  // Leader tone — lower & warmer
  lap:  ()=>beepSeq([[600,.09,'triangle',0],[900,.13,'triangle',90]]),
  // Winner tone — fuller, deeper fanfare
  best: ()=>beepSeq([
    [600,.06,'triangle',0],  [900,.09,'triangle',60],
    [600,.06,'triangle',150],[900,.09,'triangle',210],
    [600,.06,'triangle',300],[900,.14,'triangle',360]
  ]),
  // Staging beeps + deeper start horn
  count: ()=>beepSeq([
    [392,.12,'triangle',0],
    [392,.12,'triangle',1000],
    [392,.12,'triangle',2000],
    [523,.8, 'triangle',3000]
  ]),
  enter: ()=>beep(523,.22,'triangle'),
  exit:  ()=>beep(740,.09,'triangle')
};

var speechQ=[],speechBusy=false,speechWarmedUp=false,lastSpeechStart=0;

// Resume paused speech (Chrome background tab) and recover from stuck state.
// Debounce: skip recovery for 1.5s after speak() to avoid double-firing while
// the TTS engine is still initialising (speaking may briefly read false).
setInterval(()=>{
  if(typeof speechSynthesis==='undefined')return;
  if(speechSynthesis.paused)speechSynthesis.resume();
  if(speechBusy&&!speechSynthesis.speaking&&Date.now()-lastSpeechStart>1500){
    speechBusy=false;nextSpeech();
  }
},1000);
var cachedJaVoice=null;

function getJaVoice(){
  if(cachedJaVoice)return cachedJaVoice;
  var voices=speechSynthesis.getVoices();
  cachedJaVoice=voices.find(v=>v.lang&&v.lang.startsWith('ja'))||null;
  return cachedJaVoice;
}
if(typeof speechSynthesis!=='undefined'&&'onvoiceschanged' in speechSynthesis){
  speechSynthesis.onvoiceschanged=function(){cachedJaVoice=null;};
}

function warmUpSpeech(){
  if(speechWarmedUp||typeof speechSynthesis==='undefined')return;
  speechWarmedUp=true;
  var u=new SpeechSynthesisUtterance('​');
  u.volume=0;
  speechSynthesis.speak(u);
}

function speak(text){
  if(!voiceEnabled||announceMode==='none'){sfx.lap();return;}
  if(announceMode==='beep'){sfx.lap();return;}
  if(speechQ.length<8)speechQ.push(text);
  if(!speechBusy)nextSpeech();
}
function nextSpeech(){
  if(!speechQ.length){speechBusy=false;return;}
  speechBusy=true;
  var text=speechQ.shift();
  var u=new SpeechSynthesisUtterance(text);
  u.lang='ja-JP';u.rate=speechRate;
  var jaVoice=getJaVoice();
  if(jaVoice)u.voice=jaVoice;
  // Generous timeout accounting for speech rate; safety net only
  var ms=Math.max(5000,text.length*500/Math.max(0.5,speechRate));
  var timeout=setTimeout(()=>{speechSynthesis.cancel();speechBusy=false;nextSpeech();},ms);
  u.onend=()=>{clearTimeout(timeout);setTimeout(nextSpeech,80);};
  u.onerror=()=>{clearTimeout(timeout);speechBusy=false;nextSpeech();};
  lastSpeechStart=Date.now();
  speechSynthesis.speak(u);
}
function getSpokenName(p){return (p.yomi&&p.yomi!=='')?p.yomi:p.name;}
function buildSpeech(p,lapCount,lapMs){
  var spokenName=getSpokenName(p);
  var s=Math.floor(lapMs/1000),ms=Math.floor((lapMs%1000)/100);
  var m=Math.floor(s/60);s=s%60;
  var tStr=m>0?m+'分'+s+'秒'+ms:s+'秒'+ms;
  if(announceMode==='lap_laptime'){var lapLabel=lapMode==='immediate'?lapCount+'周':(lapCount===1?'ホールショット':(lapCount-1)+'周');return spokenName+'、'+lapLabel+'、'+tStr;}
  return spokenName+'、'+tStr;
}
function testVoice(){
  ensureAudio();sfx.lap();
  if(voiceEnabled&&announceMode!=='beep'&&announceMode!=='none'){
    var u=new SpeechSynthesisUtterance('テスト、1分23秒4');u.lang='ja-JP';u.rate=speechRate;
    var voices=speechSynthesis.getVoices();
    var jaVoice=voices.find(v=>v.lang&&v.lang.startsWith('ja'));
    if(jaVoice)u.voice=jaVoice;
    speechSynthesis.speak(u);
  }
}
function toggleVoice(){
  ensureAudio();
  voiceEnabled=!voiceEnabled;
  localStorage.setItem('voice',voiceEnabled?'1':'0');
  refreshVoiceBtns();
}
function refreshVoiceBtns(){
  ['voiceToggle','voiceToggle2'].forEach(id=>{
    var el=document.getElementById(id);if(!el)return;
    el.checked=voiceEnabled;
  });
}
