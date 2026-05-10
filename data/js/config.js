'use strict';

function isOnline(uid, now){
  if(!uid)return false;
  var sr=scanResults[uid.toUpperCase()]||scanResults[uid];
  if(!sr)return false;
  return sr.receivedAt && (now - sr.receivedAt < 30000);
}

function renderRoster(){
  Object.keys(scanResults).forEach(mac => {
    var macId = mac.replace(/:/g, '');
    var el = document.getElementById('scanName-' + macId);
    if (el && el.value.trim()) scanResults[mac].inputName = el.value;
  });

  var filter=(document.getElementById('searchInput').value||'').toLowerCase().trim();
  var list=document.getElementById('rosterList');
  var badge=document.getElementById('rosterCountBadge');
  badge.textContent='('+rosterData.length+'/50)';

  var filtered=rosterData.filter(function(r){
    if(!filter)return true;
    return r.name.toLowerCase().includes(filter)||(r.uid||'').toLowerCase().includes(filter);
  });

  var now=Date.now();
  filtered.sort(function(a,b){
    var aOn=isOnline(a.uid,now),bOn=isOnline(b.uid,now);
    if(aOn&&!bOn)return -1;if(!aOn&&bOn)return 1;return a.id-b.id;
  });

  if(!filtered.length){
    list.innerHTML='<div style="padding:20px;text-align:center;color:var(--muted);font-size:13px">'+(rosterData.length?'該当なし':'パイロット未登録')+'</div>';
    return;
  }

  list.innerHTML='';
  filtered.forEach(function(r){
    var isEditing=(editingRosterId===r.id);
    var item=document.createElement('div');item.className='roster-item';item.id='ri-'+r.id;
    var onlineMark=isOnline(r.uid,now)?'<span class="online-badge">📶 オンライン</span>':'';
    var activeBadge='';
    if(r.activeSlot>=0)activeBadge='<span class="active-badge '+PCLS[r.activeSlot]+'">Ch'+(r.activeSlot+1)+'</span>';

    if(isEditing){
      item.innerHTML=
        '<div style="flex:1;min-width:160px;display:flex;flex-direction:column;gap:4px">'
          +'<input type="text" id="editName" value="'+esc(r.name)+'" maxlength="20" placeholder="パイロット名" autocomplete="off" style="background:var(--bg);border:1px solid var(--accent);color:var(--tx);border-radius:5px;padding:3px 7px;font-size:12px;width:100%">'
          +'<input type="text" id="editYomi" value="'+esc(r.yomi||'')+'" maxlength="20" placeholder="読み方（よみかた）" autocomplete="off" style="background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:5px;padding:3px 7px;font-size:11px;width:100%">'
        +'</div>'
        +'<input type="text" id="editMac" value="'+esc(r.uid||'')+'" maxlength="17" placeholder="AA:BB:CC:DD:EE:FF" style="width:140px;font-family:monospace;background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:5px;padding:3px 7px;font-size:12px" autocomplete="off">'
        +'<div class="roster-actions">'
          +'<button onclick="submitEdit('+r.id+')" class="btn-success" style="font-size:11px;padding:3px 9px">保存</button>'
          +'<button onclick="cancelEdit()" class="btn-secondary" style="font-size:11px;padding:3px 9px">×</button>'
        +'</div>';
    } else {
      var chOpts='<option value="-1"'+(r.activeSlot<0?' selected':'')+'>未割当</option>';
      for(var ch=0;ch<N;ch++){
        chOpts+='<option value="'+ch+'"'+(r.activeSlot===ch?' selected':'')+'>Ch'+(ch+1)+'</option>';
      }
      var yomiLine=r.yomi?'<span class="roster-yomi">'+esc(r.yomi)+'</span>':'';
      item.innerHTML=
        '<div style="flex:1;min-width:80px">'
          +'<span class="roster-name">'+esc(r.name)+yomiLine+'</span>'
        +'</div>'
        +'<span class="roster-mac">'+(r.uid||'<span style="color:var(--err)">MAC未設定</span>')+'</span>'
        +onlineMark+activeBadge
        +'<div class="roster-actions">'
          +'<select class="ch-select" data-rid="'+r.id+'" onchange="onChSelectChange(this)">'
            +chOpts
          +'</select>'
          +'<button onclick="startEdit('+r.id+')" class="btn-secondary" style="font-size:11px;padding:3px 8px">編集</button>'
          +'<button onclick="deleteRosterPilot('+r.id+')" class="btn-danger" style="font-size:11px;padding:3px 8px">削除</button>'
        +'</div>';
    }
    list.appendChild(item);
  });
}

async function onChSelectChange(sel){
  var rid=parseInt(sel.dataset.rid);
  var newCh=parseInt(sel.value);
  var newSlots=activeSlotsLocal.slice();
  for(var i=0;i<N;i++) if(newSlots[i]===rid) newSlots[i]=-1;
  if(newCh>=0){
    for(var i=0;i<N;i++) if(newSlots[i]!==rid&&newSlots[i]===newSlots[newCh])newSlots[i]=-1;
    newSlots[newCh]=rid;
  }
  try{
    var r=await fetch('/api/active',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slots:newSlots})});
    if(r.ok){activeSlotsLocal=newSlots;applyActiveToSlots();buildRaceCards();buildCalibCards();await loadRoster();toast('✅ チャンネル割当を保存しました');}
    else toast('⚠️ 保存エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function showAddForm(){document.getElementById('addForm').style.display='block';document.getElementById('addName').focus();}
function hideAddForm(){
  document.getElementById('addForm').style.display='none';
  document.getElementById('addName').value='';
  document.getElementById('addYomi').value='';
  document.getElementById('addMac').value='';
}

async function submitAdd(){
  var name=document.getElementById('addName').value.trim();
  var yomi=document.getElementById('addYomi').value.trim();
  var mac=document.getElementById('addMac').value.trim().toUpperCase();
  if(!name){toast('⚠️ 名前を入力してください');return;}
  var validMac=/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(mac);
  if(mac&&!validMac){toast('⚠️ MAC形式: AA:BB:CC:DD:EE:FF');return;}
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,yomi,uid:validMac?mac:''})});
    if(r.ok){await loadRoster();hideAddForm();toast('✓ '+name+' を登録しました');}
    else toast('⚠️ 登録エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function startEdit(id){editingRosterId=id;renderRoster();setTimeout(()=>{var el=document.getElementById('editName');if(el)el.focus();},0);}
function cancelEdit(){editingRosterId=null;renderRoster();}

async function submitEdit(id){
  var name=document.getElementById('editName').value.trim();
  var yomi=document.getElementById('editYomi').value.trim();
  var mac=document.getElementById('editMac').value.trim().toUpperCase();
  if(!name){toast('⚠️ 名前を入力してください');return;}
  var validMac=/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(mac);
  if(mac&&!validMac){toast('⚠️ MAC形式: AA:BB:CC:DD:EE:FF');return;}
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,name,yomi,uid:validMac?mac:''})});
    if(r.ok){editingRosterId=null;await loadRoster();applyActiveToSlots();buildRaceCards();toast('✓ 更新しました');}
    else toast('⚠️ 更新エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

async function deleteRosterPilot(id){
  var r=rosterData.find(x=>x.id===id);
  if(!confirm((r?r.name:'このパイロット')+'を削除しますか？'))return;
  try{
    var res=await fetch('/api/pilots/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});
    if(res.ok){await loadRoster();applyActiveToSlots();buildRaceCards();buildCalibCards();toast('削除しました');}
    else toast('⚠️ 削除エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function updateScanList(){
  var el=document.getElementById('scanList');if(!el)return;
  var macs=Object.keys(scanResults);
  if(!macs.length){el.innerHTML='<span style="color:var(--muted);font-size:12px">スキャン待機中... 機体の電源を入れてください</span>';return;}
  el.innerHTML=macs.map(function(mac){
    var s=scanResults[mac];
    var done=s.assignedRosterId!==undefined&&s.assignedRosterId>=0;
    var macId=mac.replace(/:/g,'');
    var savedName=s.inputName||s.pilotName||'';
    return '<div id="scan-'+macId+'" style="background:var(--sf2);border:1px solid var(--bd);border-radius:8px;padding:10px 12px;margin-bottom:8px">'
      +'<div style="display:flex;align-items:center;gap:8px;margin-bottom:8px">'
      +  '<span style="font-family:monospace;font-size:13px;color:var(--accent);flex:1">'+mac+'</span>'
      +  '<span style="color:var(--muted);font-size:11px">'+s.rssi+' dBm</span>'
      +  (done?'<span style="color:var(--ok);font-size:11px;font-weight:700">✓ 登録済み</span>':'')
      +'</div>'
      +'<div style="display:flex;gap:6px;align-items:center">'
      +  '<input type="text" id="scanName-'+macId+'" placeholder="パイロット名" maxlength="20" value="'+esc(savedName)+'" autocomplete="off"'
      +    ' style="flex:1;background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:6px;padding:5px 8px;font-size:13px"'
      +    (done?' disabled':'')+' onkeydown="if(event.key===\'Enter\')registerScanPilot(\''+mac+'\')">'
      +  '<button id="scanBtn-'+macId+'" onclick="registerScanPilot(\''+mac+'\')" class="btn-success" style="font-size:12px;padding:5px 12px;white-space:nowrap"'+(done?' disabled':'')+'>パイロット情報に追加</button>'
      +'</div>'
      +'</div>';
  }).join('');
}

async function registerScanPilot(mac){
  var macId=mac.replace(/:/g,'');
  var nameEl=document.getElementById('scanName-'+macId);
  if(!nameEl)return;
  var name=nameEl.value.trim();
  if(!name){toast('⚠️ 名前を入力してください');return;}
  try{
    var r=await fetch('/api/pilots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,yomi:'',uid:mac})});
    if(!r.ok){toast('⚠️ 登録エラー');return;}
    var body=await r.json();
    scanResults[mac].assignedRosterId=body.id;
    scanResults[mac].pilotName=name;
    scanResults[mac].inputName='';
    await loadRoster();updateScanList();
    toast('✓ '+name+' をパイロット情報に追加しました');
  }catch(e){toast('⚠️ 接続エラー');}
}

function clearScan(){scanResults={};updateScanList();fetch('/api/scan/clear',{method:'POST'}).catch(()=>{});}

async function sdBackup(){
  try{
    var r=await fetch('/api/sd/pilots/backup',{method:'POST'});
    if(r.ok)toast('✅ SDカードにバックアップしました');
    else toast('⚠️ バックアップエラー: '+(await r.text()));
  }catch(e){toast('⚠️ 接続エラー');}
}
async function sdRestore(){
  if(!confirm('SDカードからパイロット情報を復元します。現在のデータは上書きされます。よろしいですか？'))return;
  try{
    var r=await fetch('/api/sd/pilots/restore',{method:'POST'});
    if(r.ok)toast('📤 復元要求を送信しました。完了まで少々お待ちください…',4000);
    else toast('⚠️ 復元エラー');
  }catch(e){toast('⚠️ 接続エラー');}
}

function saveGlobalConfig(){
  announceMode=document.getElementById('announceMode').value;
  speechRate=parseFloat(document.getElementById('speechRateN').value)||1.1;
  localStorage.setItem('announce',announceMode);
  localStorage.setItem('srate',String(speechRate));
}

function updateSdSection(present){
  sdPresent=present;
  var sec=document.getElementById('sdSection');
  if(sec)sec.style.display=present?'block':'none';
  var st=document.getElementById('sdTabStatus');
  if(st)st.innerHTML=present
    ?'<p style="color:var(--ok);font-size:12px">✅ SDカード検出済み — レース中はダウンロードしないでください</p>'
    :'<p style="color:var(--err);font-size:12px">⚠ SDカードが見つかりません</p>';
  if(!present){
    var wrap=document.getElementById('sdFileListWrap');
    if(wrap)wrap.innerHTML='<p style="color:var(--muted);font-size:12px">SDカードが挿入されていません</p>';
  }
}
