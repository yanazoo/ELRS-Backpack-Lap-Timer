'use strict';

var charts={};
var chartDirty=[false,false,false,false];

// Runs at 60fps via rAF; draws only when calib tab is active and data changed
(function chartLoop(){
  requestAnimationFrame(chartLoop);
  var calibActive=document.getElementById('pane-calib').classList.contains('active');
  for(var i=0;i<N;i++){
    if(chartDirty[i]&&charts[i]){
      if(calibActive)drawChart(i);
      chartDirty[i]=false;
    }
  }
})();

function buildCalibCards(){
  var g=document.getElementById('calibGrid');g.innerHTML='';
  slots.forEach(function(p){
    var d=document.createElement('div');d.className='calib-card';
    d.innerHTML=
      '<div class="calib-header '+PCLS[p.id]+'">'
        +'<span id="calName'+p.id+'">'+esc(p.name==='---'?'Ch'+(p.id+1):p.name)+'</span>'
        +'<span class="rssi-live-badge">RSSI: <strong id="calRssi'+p.id+'">---</strong> dBm</span>'
      +'</div>'
      +'<canvas id="calCanvas'+p.id+'"></canvas>'
      +'<div class="calib-sliders">'
        +'<div class="config-row"><label>Enter（入）</label>'
          +'<div class="slider-num">'
            +'<input type="range" id="calEnter'+p.id+'" min="-100" max="-10" step="1" value="'+p.enterRssi+'" oninput="syncCalib('+p.id+',\'enter\')">'
            +'<input type="number" id="calEnterN'+p.id+'" min="-100" max="-10" step="1" value="'+p.enterRssi+'" onchange="syncCalibN('+p.id+',\'enter\')">'
            +'<span class="unit">dBm</span>'
          +'</div></div>'
        +'<div class="config-row"><label>Exit（出）</label>'
          +'<div class="slider-num">'
            +'<input type="range" id="calExit'+p.id+'" min="-100" max="-10" step="1" value="'+p.exitRssi+'" oninput="syncCalib('+p.id+',\'exit\')">'
            +'<input type="number" id="calExitN'+p.id+'" min="-100" max="-10" step="1" value="'+p.exitRssi+'" onchange="syncCalibN('+p.id+',\'exit\')">'
            +'<span class="unit">dBm</span>'
          +'</div></div>'
      +'</div>';
    g.appendChild(d);
    p.calRssiEl=null;
    setTimeout(()=>initChart(p.id),0);
  });
}

function initChart(id){
  var cv=document.getElementById('calCanvas'+id);if(!cv)return;
  cv.width = cv.offsetWidth || cv.parentElement.offsetWidth || 300;
  cv.height = 130;
  var col=getComputedStyle(document.documentElement).getPropertyValue('--p'+id).trim();
  charts[id]={cv,ctx:cv.getContext('2d'),data:new Float32Array(200).fill(-120),cross:new Uint8Array(200),col:col};
  drawChart(id);
}

function pushChart(id,rssi,crossing){
  var c=charts[id];if(!c)return;
  c.data.copyWithin(0,1);c.data[199]=rssi;c.cross.copyWithin(0,1);c.cross[199]=crossing?1:0;
  chartDirty[id]=true;
}

function calcChartRange(id){
  var c=charts[id];var p=slots[id];
  var hasData=false;var dMin=0;var dMax=-200;
  for(var i=0;i<200;i++){
    if(c.data[i]>-119){
      if(!hasData||c.data[i]<dMin)dMin=c.data[i];
      if(!hasData||c.data[i]>dMax)dMax=c.data[i];
      hasData=true;
    }
  }
  var allVals=hasData?[dMin,dMax,p.enterRssi,p.exitRssi]:[p.enterRssi,p.exitRssi];
  var yMin=Math.min.apply(null,allVals)-5;
  var yMax=Math.max.apply(null,allVals)+5;
  if(yMax-yMin<20){var mid=(yMax+yMin)/2;yMin=mid-10;yMax=mid+10;}
  return {yMin,yMax};
}

function drawChart(id){
  var c=charts[id];if(!c)return;
  var cv=c.cv,ctx=c.ctx,w=cv.width,h=cv.height,p=slots[id];
  var range=calcChartRange(id);
  var yMin=range.yMin,yMax=range.yMax,rng=yMax-yMin;
  var toY=function(v){return h-((Math.max(yMin,Math.min(yMax,v))-yMin)/rng)*h;};
  var dx=w/200;
  var col=c.col||(c.col=getComputedStyle(document.documentElement).getPropertyValue('--p'+id).trim());
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,w,h);
  ctx.fillStyle=col+'22';
  for(var i=0;i<200;i++)if(c.cross[i])ctx.fillRect(i*dx,0,dx+1,h);
  ctx.strokeStyle='#1e2530';ctx.lineWidth=1;ctx.setLineDash([]);
  var tickStart=Math.ceil(yMin/10)*10;
  for(var db=tickStart;db<=yMax;db+=10){
    var ty=toY(db);
    ctx.beginPath();ctx.moveTo(0,ty);ctx.lineTo(w,ty);ctx.stroke();
    ctx.fillStyle='#444';ctx.font='9px monospace';ctx.fillText(db+'dB',2,ty-2);
  }
  ctx.setLineDash([4,4]);ctx.lineWidth=1;
  ctx.strokeStyle='#3fb95088';ctx.beginPath();ctx.moveTo(0,toY(p.enterRssi));ctx.lineTo(w,toY(p.enterRssi));ctx.stroke();
  ctx.strokeStyle='#f8514988';ctx.beginPath();ctx.moveTo(0,toY(p.exitRssi));ctx.lineTo(w,toY(p.exitRssi));ctx.stroke();
  ctx.setLineDash([]);
  ctx.beginPath();
  for(var j=0;j<200;j++){var x=j*dx,y=toY(c.data[j]);j===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}
  ctx.lineTo(w,h);ctx.lineTo(0,h);ctx.closePath();
  ctx.fillStyle=col+'28';ctx.fill();
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  for(var j=0;j<200;j++){var x=j*dx,y=toY(c.data[j]);j===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}
  ctx.stroke();
}

function cap(s){return s.charAt(0).toUpperCase()+s.slice(1);}

function syncCalib(id,which){
  var v=parseInt(document.getElementById('cal'+cap(which)+id).value);
  document.getElementById('cal'+cap(which)+'N'+id).value=v;
  slots[id][which+'Rssi']=v;drawChart(id);
  scheduleCalibSave(id);
}
function syncCalibN(id,which){
  var el=document.getElementById('cal'+cap(which)+'N'+id);
  var v=Math.max(-100,Math.min(-10,parseInt(el.value)||(-80)));
  el.value=v;document.getElementById('cal'+cap(which)+id).value=v;
  slots[id][which+'Rssi']=v;drawChart(id);
  scheduleCalibSave(id);
}

function scheduleCalibSave(slotId){
  if(calibSaveTimers[slotId])clearTimeout(calibSaveTimers[slotId]);
  calibSaveTimers[slotId]=setTimeout(()=>saveCalibConfig(slotId),800);
}

async function saveCalibConfig(slotId){
  var enter=parseInt(document.getElementById('calEnterN'+slotId).value);
  var exit_=parseInt(document.getElementById('calExitN'+slotId).value);
  if(enter<=exit_){toast('⚠️ Enter > Exit にしてください');return;}
  var ri=activeSlotsLocal[slotId];
  if(ri<0){return;}
  slots[slotId].enterRssi=enter;slots[slotId].exitRssi=exit_;
  try{
    var r=await fetch('/api/calib',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:ri,enter,exit:exit_})});
    if(r.ok){
      var rd=rosterById[ri];if(rd){rd.enter=enter;rd.exit=exit_;}
      // Unconditionally reflect the new thresholds in the pilot-info UI.
      renderRoster();
    }else toast('⚠️ 保存エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function syncCalibSliders(id,enter,exit_){
  var eEl=document.getElementById('calEnter'+id),eN=document.getElementById('calEnterN'+id);
  var xEl=document.getElementById('calExit'+id),xN=document.getElementById('calExitN'+id);
  if(eEl)eEl.value=enter;if(eN)eN.value=enter;if(xEl)xEl.value=exit_;if(xN)xN.value=exit_;
}
