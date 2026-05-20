'use strict';

var actx=null,audioUnlocked=false;
function ensureAudio(){
  try{
    if(!actx)actx=new(window.AudioContext||window.webkitAudioContext)();
    if(actx.state==='suspended')actx.resume();
    // Android Chrome only fully unlocks WebAudio when an actual node has
    // produced output inside a user gesture; resume() alone is not enough.
    if(!audioUnlocked){
      var o=actx.createOscillator(),g=actx.createGain();
      g.gain.value=0.00001;o.connect(g);g.connect(actx.destination);
      var t=actx.currentTime;o.start(t);o.stop(t+0.03);
      audioUnlocked=true;
    }
  }catch(e){}
}

// Unlock audio + speech on the very first user interaction anywhere on the
// page. Race crossing/lap sounds and TTS fire from WebSocket events (no
// gesture), so without this Android stays muted until a specific button is
// pressed. Also recover the context after backgrounding (screen lock).
(function(){
  function unlock(){ensureAudio();}
  ['pointerdown','touchstart','mousedown','keydown','click'].forEach(function(ev){
    document.addEventListener(ev,unlock,{capture:true,passive:true});
  });
  document.addEventListener('visibilitychange',function(){
    if(!document.hidden&&actx&&actx.state==='suspended')actx.resume();
  });
})();
// Layered voice: fundamental + chorus-detuned twin + sub-octave (body) +
// soft upper harmonic (presence), routed through a lowpass for warmth and
// a click-free attack / smooth exponential release. Same signature as before
// so existing call sites get the deeper, richer tone automatically.
function beep(freq,dur,type,vol){
  if(!actx)return;
  if(actx.state==='suspended'){try{actx.resume();}catch(e){}}
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

var speechQ=[],speechBusy=false,speechWarmedUp=false,speechWarming=false,lastSpeechStart=0;

// iOS Safari often fails to fire onend / reports speaking inconsistently
// and rapid speak() calls can drop the second utterance. Poll frequently
// and advance the queue whenever the engine is no longer speaking.
setInterval(()=>{
  if(typeof speechSynthesis==='undefined')return;
  if(speechSynthesis.paused)speechSynthesis.resume();
  if(speechBusy&&!speechSynthesis.speaking&&Date.now()-lastSpeechStart>800){
    speechBusy=false;nextSpeech();
  }
},250);
// iOS keep-alive: nudge a long-running utterance so the engine doesn't
// silently stall (well-known iOS Safari TTS bug).
setInterval(()=>{
  if(typeof speechSynthesis==='undefined')return;
  if(speechSynthesis.speaking&&Date.now()-lastSpeechStart>8000){
    try{speechSynthesis.pause();speechSynthesis.resume();}catch(e){}
  }
},5000);
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

// Silent TTS warm-up. Called explicitly from the Start button (an
// unambiguous user gesture) so unlock is tied to "press Start to begin".
// On iOS Safari a silent primer may not be enough; if that happens the
// user can fall back to the audible voice toggle (toggleVoice).
function warmUpSpeech(){
  if(speechWarmedUp||speechWarming||typeof speechSynthesis==='undefined')return;
  try{
    if(speechSynthesis.paused)speechSynthesis.resume();
    speechWarming=true;
    var u=new SpeechSynthesisUtterance('​');
    u.volume=0;u.lang='ja-JP';u.rate=speechRate;
    u.onstart=()=>{speechWarmedUp=true;speechWarming=false;};
    u.onend=()=>{speechWarmedUp=true;speechWarming=false;};
    u.onerror=()=>{speechWarming=false;};
    speechSynthesis.speak(u);
  }catch(e){speechWarming=false;}
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
  // Soft timeout = expected duration + grace. On iOS, onend often never
  // fires, so we advance the queue once the engine reports it's no longer
  // speaking. Hard timeout = forced cancel + advance as a last resort.
  var rate=Math.max(0.5,speechRate);
  var estMs=Math.max(900,text.length*200/rate);
  var hardMs=Math.max(5000,estMs*2);
  var done=false;
  function advance(cancel){
    if(done)return;done=true;
    clearTimeout(softT);clearTimeout(hardT);
    if(cancel){try{speechSynthesis.cancel();}catch(e){}}
    speechBusy=false;
    setTimeout(nextSpeech,60);
  }
  var softT=setTimeout(()=>{if(!speechSynthesis.speaking)advance(false);},estMs+400);
  var hardT=setTimeout(()=>advance(true),hardMs);
  u.onend=()=>advance(false);
  u.onerror=()=>advance(false);
  lastSpeechStart=Date.now();
  try{
    if(speechSynthesis.paused)speechSynthesis.resume();
    speechSynthesis.speak(u);
  }catch(e){advance(false);}
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
  // Audible confirmation on enable doubles as the iOS Safari TTS unlock
  // (matches the proven PhobosLT_4ch pattern). Cancel anything pending
  // when disabling.
  if(typeof speechSynthesis==='undefined')return;
  if(voiceEnabled){
    try{
      var u=new SpeechSynthesisUtterance('音声オン');
      u.lang='ja-JP';u.rate=speechRate;u.volume=0.5;
      var jaVoice=getJaVoice();if(jaVoice)u.voice=jaVoice;
      speechSynthesis.speak(u);
      speechWarmedUp=true;
    }catch(e){}
  }else{
    try{speechSynthesis.cancel();}catch(e){}
    speechQ=[];speechBusy=false;
  }
}
function refreshVoiceBtns(){
  ['voiceToggle','voiceToggle2'].forEach(id=>{
    var el=document.getElementById(id);if(!el)return;
    el.checked=voiceEnabled;
  });
}
