'use strict';

function refreshSdFiles(){
  if(!sdPresent){toast('SDカードがありません');return;}
  fetch('/api/sd/files/list',{method:'POST'}).catch(()=>{});
  toast('ファイル一覧を取得中...');
}

function downloadSdFile(path){
  if(raceRunning){toast('レース中はダウンロードできません');return;}
  sdDownloadPath=path;sdDownloadBuf=[];
  fetch('/api/sd/files/download',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({path:path})
  }).catch(()=>{});
  toast('ダウンロード中: '+path.replace(/^\//,''));
}

function deleteSdFile(path){
  if(!confirm(path.replace(/^\//,'')+' を削除しますか？'))return;
  fetch('/api/sd/files/delete',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({path:path})
  }).catch(()=>{});
}

function renderSdFileList(){
  var wrap=document.getElementById('sdFileListWrap');
  if(!wrap)return;
  if(!sdFileList.length){
    wrap.innerHTML='<p style="color:var(--muted);font-size:12px">ファイルがありません</p>';
    return;
  }
  var html='<div class="lap-table-wrap"><table class="lapTable"><thead><tr>'
    +'<th style="text-align:left">ファイル名</th><th>サイズ</th><th>操作</th>'
    +'</tr></thead><tbody>';
  sdFileList.forEach(function(f){
    var kb=(f.size/1024).toFixed(1);
    var path='/'+f.name;
    var escaped=path.replace(/'/g,"\\'");
    html+='<tr>'
      +'<td style="text-align:left">'+f.name+'</td>'
      +'<td>'+kb+' KB</td>'
      +'<td style="display:flex;gap:4px;justify-content:center">'
      +'<button onclick="downloadSdFile(\''+escaped+'\')" class="btn-secondary" style="padding:2px 8px;font-size:11px">⬇ DL</button>'
      +'<button onclick="deleteSdFile(\''+escaped+'\')" class="btn-danger" style="padding:2px 8px;font-size:11px">🗑</button>'
      +'</td></tr>';
  });
  html+='</tbody></table></div>';
  wrap.innerHTML=html;
}
