'use strict';

var actx=null;
function ensureAudio(){if(!actx)actx=new(window.AudioContext||window.webkitAudioContext)();if(actx.state==='suspended')actx.resume();}
function beep(freq,dur,type,vol){if(!actx)return;type=type||'sine';vol=vol===undefined?0.4:vol;var o=actx.createOscillator(),g=actx.createGain();o.connect(g);g.connect(actx.destination);o.frequency.value=freq;o.type=type;var t=actx.currentTime;g.gain.setValueAtTime(vol,t);g.gain.exponentialRampToValueAtTime(.001,t+dur);o.start(t);o.stop(t+dur);}
function beepSeq(notes){notes.forEach(n=>setTimeout(()=>beep(n[0],n[1],n[2]),n[3]||0));}

var sfx={
  // RotorHazard leader tone
  lap:  ()=>beepSeq([[1200,.075,'square',0],[1800,.1,'square',75]]),
  // RotorHazard winner tone
  best: ()=>beepSeq([
    [1200,.05,'square',0],  [1800,.075,'square',50],
    [1200,.05,'square',125],[1800,.075,'square',175],
    [1200,.05,'square',250],[1800,.1,  'square',300]
  ]),
  // RotorHazard staging + start
  count: ()=>beepSeq([
    [440,.1,'triangle',0],
    [440,.1,'triangle',1000],
    [440,.1,'triangle',2000],
    [880,.7,'triangle',3000]
  ]),
  enter: ()=>beep(880,.2,'sine'),
  exit:  ()=>beep(1100,.07,'sine')
};

var speechQ=[],speechBusy=false;
function speak(text){
  if(!voiceEnabled||announceMode==='none'){sfx.lap();return;}
  if(announceMode==='beep'){sfx.lap();return;}
  speechQ.push(text);if(!speechBusy)nextSpeech();
}
function nextSpeech(){
  if(!speechQ.length){speechBusy=false;return;}
  speechBusy=true;
  var u=new SpeechSynthesisUtterance(speechQ.shift());
  u.lang='ja-JP';u.rate=speechRate;
  var timeout=setTimeout(()=>{speechSynthesis.cancel();speechBusy=false;nextSpeech();},5000);
  u.onend=()=>{clearTimeout(timeout);setTimeout(nextSpeech,80);};
  u.onerror=()=>{clearTimeout(timeout);speechBusy=false;nextSpeech();};
  speechSynthesis.speak(u);
}
function getSpokenName(p){return (p.yomi&&p.yomi!=='')?p.yomi:p.name;}
function buildSpeech(p, lapCount, lapMs){
  var spokenName=getSpokenName(p);
  var s=Math.floor(lapMs/1000),ms=Math.floor((lapMs%1000)/100);
  var m=Math.floor(s/60);s=s%60;
  var tStr=m>0?m+'分'+s+'秒'+ms:s+'秒'+ms;
  if(announceMode==='lap_laptime') return spokenName+'、'+lapCount+'周、'+tStr;
  return spokenName+'、'+tStr;
}
function testVoice(){
  ensureAudio();sfx.lap();
  if(voiceEnabled&&announceMode!=='beep'&&announceMode!=='none'){
    var u=new SpeechSynthesisUtterance('テスト、1分23秒4');u.lang='ja-JP';u.rate=speechRate;speechSynthesis.speak(u);
  }
}
function toggleVoice(){voiceEnabled=!voiceEnabled;localStorage.setItem('voice',voiceEnabled?'1':'0');refreshVoiceBtns();}
function refreshVoiceBtns(){
  ['voiceBtn','voiceBtn2'].forEach(id=>{var b=document.getElementById(id);if(!b)return;
    b.textContent=voiceEnabled?'🔊 音声 ON':'🔇 音声 OFF';b.className='btn-voice'+(voiceEnabled?' on':'');});
}
