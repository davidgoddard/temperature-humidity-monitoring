#pragma once
#include <pgmspace.h>
static const char DASHBOARD_PAGE[] PROGMEM = R"ESPHTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Wi-Fi temperature dashboard</title>
<style>
:root{font:15px system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#203042;background:#f4f7fb}
*{box-sizing:border-box}
body{max-width:1250px;margin:0 auto;padding:32px 26px 55px;line-height:1.5}
h1{font-size:27px;margin:0 0 8px;letter-spacing:-.5px}
p{margin:9px 0 17px;color:#596a7f}
section{background:#fff;border:1px solid #dce5ef;border-radius:16px;padding:25px 27px;margin:24px 0;box-shadow:0 2px 12px rgba(39,69,110,.035)}
section h3{margin:0 0 12px}
button,input,select{font:inherit;padding:11px 13px;border:1px solid #cad6e4;border-radius:10px;background:#fff;color:#203042}
button{cursor:pointer}button:hover{border-color:#8195b0}button.primary{background:#1758b4;color:#fff;border-color:#1758b4}button.primary:hover{background:#0e468f}
button.danger{background:#fff;color:#a42332;border-color:#edb6bc}button.danger:hover{background:#fff1f2;border-color:#cc3948}
#resetStatus{font-size:13px;color:#596a7f}
.controls{display:flex;align-items:center;column-gap:18px;row-gap:14px;flex-wrap:wrap}
.controls label{display:inline-flex;align-items:center;gap:9px;white-space:nowrap}
input{width:180px}#status{color:#4e647b}#count{color:#53677d;font-size:14px}
.graph-area{margin-top:26px;padding-top:16px;border-top:1px solid #e9eef5}
#chart{width:100%;height:400px;display:block}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(225px,1fr));gap:20px;margin:26px 0 8px}
.card{cursor:pointer;text-align:left;font:inherit;color:inherit;padding:20px 22px 22px;border:1px solid var(--sensor-border);border-left:6px solid var(--sensor-color);border-radius:14px;background:var(--sensor-bg);min-height:168px;box-shadow:0 2px 5px rgba(0,0,0,.025)}
.card[aria-pressed="true"]{outline:3px solid var(--sensor-color);outline-offset:2px}
.card:focus-visible{outline:3px solid #203042;outline-offset:3px}
.card .sensor-name{color:var(--sensor-color);font-weight:750;font-size:15px;display:flex;align-items:center;gap:9px;margin-bottom:13px}
.card .sensor-dot,.legend-dot{display:inline-block;width:12px;height:12px;border-radius:50%;background:var(--sensor-color);flex:none}
.card .sensor-number{font-size:32px;line-height:1.14;font-weight:750;letter-spacing:-.7px;margin-bottom:8px;color:#203042}
.card .sensor-rh{font-size:16px;margin-bottom:8px}.card .sensor-time{font-size:12px;color:#586d82}
.legend{display:flex;gap:12px;flex-wrap:wrap;padding:13px 2px 3px}
.legend-item{display:inline-flex;gap:9px;align-items:center;padding:7px 12px;border-radius:20px;border:1px solid var(--sensor-border);background:var(--sensor-bg);font-size:13px;font-weight:650;color:var(--sensor-color)}
small{color:#65778b}
@media(max-width:640px){body{padding:17px 12px 36px}section{padding:18px 15px;margin:17px 0}.cards{grid-template-columns:1fr;gap:13px;margin-top:20px}.card{min-height:0}#chart{height:340px}.controls{gap:12px}}
</style></head>
<body><h1>Wi-Fi Temperature Dashboard</h1><p>Shows readings still retained on the base station. Use the Node archive app to save and browse long-term history.</p>
<section><div class="controls"><label>Base URL <input id="base" value="http://temp.local"></label><button class="primary" id="refresh">Fetch readings</button><span id="status" role="status" aria-live="polite">Not connected</span></div><p><small>Dashboard and API are served by the base at http://temp.local. Refresh retrieves available measurements over Wi-Fi.</small></p></section>
<section><div class="controls"><label>Show <select id="metric"><option value="temp">Temperature (°C)</option><option value="rh">Humidity (%)</option></select></label><label>Period <select id="period"><option value="1">1 hour</option><option value="6" selected>6 hours</option><option value="24">24 hours</option><option value="168">7 days</option><option value="720">30 days</option><option value="0">All</option></select></label><label>Overdue after (seconds) <input id="overdue" type="number" min="120" value="360"></label><span id="count"></span></div><div id="cards" class="cards"></div><div class="graph-area"><canvas id="chart"></canvas><div id="legend" class="legend"></div></div></section>
<section><h3>Archive and export</h3><p>Choose an archive folder in Chrome or Edge, then save the outstanding readings. The base deletes acknowledged readings <strong>only after</strong> the browser has written the archive file successfully.</p><div class="controls"><button id="folder">Choose archive folder</button><button id="archive" class="primary">Save snapshot + acknowledge</button><button id="download">Download CSV (no deletion)</button><span id="archiveStatus"></span></div><p><small>Important: direct folder writing is unavailable at ordinary http://temp.local because it is not a secure browser context. Use Download CSV and choose its destination in the browser, or serve this file from localhost/HTTPS to enable Choose archive folder and automatic base acknowledgement.</small></p></section>
<section><h3>Reset base station readings</h3><p>This permanently deletes all readings held by the base station and clears this browser's stored sensor history. Previously downloaded CSV archives are not deleted. Sensors will continue uploading new readings.</p><div class="controls"><button id="reset" class="danger">Reset all readings…</button><span id="resetStatus"></span></div></section>
<script>
'use strict';
const $=s=>document.getElementById(s);const palette=['#2563eb','#db2777','#0f766e','#9a6700','#7c3aed','#b45309','#0e7490','#be185d'];
const sensorColor=id=>palette[((Number(id)-1)%palette.length+palette.length)%palette.length];
const sensorStyle=id=>{let color=sensorColor(id);return `--sensor-color:${color};--sensor-bg:${color}0d;--sensor-border:${color}44;`;};
let db, all=[], latest=[], folder=null, busy=false, initialized=false;
let persistedAfter=0, sessionAfter=0, activeDataset=null, legacyDataset=null, storageWarning='', datasetNotice='';
const memoryDatasets=new Map();
function stored(key){try{return localStorage.getItem(key);}catch{return null;}}
function remember(key,value){try{localStorage.setItem(key,value);}catch{}}
function memoryOnly(error){if(db)db.close();db=null;storageWarning='Readings are not stored persistently: '+(error?.message||error);}
const maxId=rows=>rows.reduce((n,r)=>Math.max(n,r.id),0);

const datasetKey='esp-sensor-dataset-id';
const archiveKey=suffix=>'esp-archive-'+activeDataset+'-'+suffix;
async function clearBrowserReadings() {
  if(db){const tx=db.transaction('samples','readwrite');const done=txDone(tx);tx.objectStore('samples').clear();await done;}
  all=[];latest=[];persistedAfter=sessionAfter=0;
  localStorage.removeItem(archiveKey('last-id'));localStorage.removeItem(archiveKey('pending-id'));draw();
}
async function checkDataset() {
  const data=await request('/api/meta');
  if(typeof data.dataset!=='string'||!data.dataset)throw Error('Missing dataset identifier');
  if(activeDataset===data.dataset){await pruneArchived(data);return;}
  const old=activeDataset||stored(datasetKey);
  // The original database belongs to its original dataset permanently. Never
  // overwrite it when another base/reset starts using record IDs from one.
  const legacy=legacyDataset;
  // With no reliable ownership marker, retain the legacy database untouched.
  // A separate dataset database prevents mixing reused record IDs.
  if(activeDataset)memoryDatasets.set(activeDataset,{all,sessionAfter});
  if(db)db.close();db=null;
  activeDataset=data.dataset;latest=[];
  const cached=memoryDatasets.get(activeDataset);
  all=cached?.all||[];sessionAfter=cached?.sessionAfter||0;persistedAfter=0;
  try{
    db=await openDb(activeDataset===legacy?'esp-sensor-history-v1':'esp-sensor-history-'+activeDataset);
    all=await readLocal();all.sort((a,b)=>a.id-b.id);persistedAfter=sessionAfter=maxId(all);storageWarning='';
  }catch(e){memoryOnly(e);}
  // Migrate the old archive markers only when their dataset is known to match.
  if(old===activeDataset){for(const suffix of ['last-id','pending-id']){
    const key='esp-archive-'+suffix,value=stored(key);
    if(value){try{if(!stored(archiveKey(suffix)))localStorage.setItem(archiveKey(suffix),value);localStorage.removeItem(key);}catch{}}
  }}
  if(old&&old!==activeDataset){
    datasetNotice='Dataset changed; previous browser history retained separately. ';
    // Archive cursors belong to one dataset and must never acknowledge another.
    // Dataset-specific archive markers keep old acknowledgments isolated.
  }
  remember(datasetKey,activeDataset);await pruneArchived(data);draw();
}
async function pruneArchived(meta){
 if(!Number.isSafeInteger(meta.retained_after)||meta.retained_after<0)return;
 const floor=meta.retained_after;
 latest=latest.filter(r=>r.id>floor);
 if(!all.some(r=>r.id<=floor))return;
 all=all.filter(r=>r.id>floor);
 if(db){try{const tx=db.transaction('samples','readwrite'),done=txDone(tx),store=tx.objectStore('samples');store.clear();for(const r of all)store.put(r);await done;}catch(e){memoryOnly(e);}}
 draw();
}
async function resetAll() {
  if(busy||!initialized){$('resetStatus').textContent='Wait for the current download to finish';return;}
  if(!confirm('Permanently delete all measurements on the base AND this browser? This cannot be undone.'))return;
  const phrase=prompt('Type RESET to confirm removal of all readings:');
  if(phrase!=='RESET')return;
  busy=true;$('reset').disabled=true;$('refresh').disabled=true;
  try{
    const response=await fetch(url('/api/reset'),{method:'POST',headers:{'Content-Type':'text/plain'},body:'RESET ALL READINGS'});
    if(!response.ok)throw Error('Base reset HTTP '+response.status+': '+await response.text());
    const result=await response.json();
    await clearBrowserReadings();
    activeDataset=null;await checkDataset();
    $('resetStatus').textContent='Base and browser readings cleared';
    message('Connected · empty history');
  }catch(e){
    $('resetStatus').textContent='Reset incomplete: '+e.message;
    // If the base succeeded but local clearing failed, dataset check on refresh
    // will detect the generation change.
  }finally{busy=false;$('reset').disabled=false;$('refresh').disabled=false;}
}
const url=path=>$('base').value.replace(/\/$/,'')+path;
function message(s){$('status').textContent=datasetNotice+s+(storageWarning?' — '+storageWarning:'');}
function openDb(name='esp-sensor-history-v1'){return new Promise((resolve,reject)=>{
  let settled=false,r;
  const fail=e=>{if(!settled){settled=true;clearTimeout(timer);reject(e);}};
  const timer=setTimeout(()=>fail(Error('Database open timed out')),4000);
  try{r=indexedDB.open(name,2);}catch(e){fail(e);return;}
  r.onupgradeneeded=()=>{if(!r.result.objectStoreNames.contains('samples'))r.result.createObjectStore('samples',{keyPath:'id'});if(!r.result.objectStoreNames.contains('metadata'))r.result.createObjectStore('metadata');};
  r.onblocked=()=>fail(Error('Database blocked by another tab'));
  r.onerror=()=>fail(r.error||Error('Database open failed'));
  r.onsuccess=()=>{if(settled){r.result.close();return;}settled=true;clearTimeout(timer);
    r.result.onversionchange=()=>{memoryOnly(Error('Database changed in another tab'));message('Reconnect to resume persistent storage');};resolve(r.result);};
});}
function txDone(tx){return new Promise((resolve,reject)=>{tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error||Error('Storage transaction failed'));tx.onabort=()=>reject(tx.error||Error('Storage transaction aborted'));});}
async function persist(rows){if(!rows.length||!db)return;const tx=db.transaction('samples','readwrite'),done=txDone(tx);const store=tx.objectStore('samples');for(const r of rows)store.put(r);await done;persistedAfter=Math.max(persistedAfter,maxId(rows));}
async function legacyOwner(){
 const tx=db.transaction('metadata','readwrite'),done=txDone(tx),store=tx.objectStore('metadata'),req=store.get('dataset');
 const owner=stored('esp-sensor-legacy-dataset')||stored(datasetKey)||'unassigned';
 req.onsuccess=()=>{if(!req.result)store.put(owner,'dataset');};
 await done;return req.result||owner;
}
async function readLocal(){const tx=db.transaction('samples'),done=txDone(tx);const req=tx.objectStore('samples').getAll();await done;return req.result;}
const getArchId=()=>Number(localStorage.getItem(archiveKey('last-id'))||0);
async function request(path){const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),20000);try{
 const response=await fetch(url(path),{cache:'no-store',signal:controller.signal});if(!response.ok)throw Error('HTTP '+response.status);return await response.json();
 }catch(e){if(e.name==='AbortError')throw Error('Request timed out');throw e;}finally{clearTimeout(timer);}}
function decodeRows(json){if(json.dataset&&json.dataset!==activeDataset)throw Error('Dataset changed during download; fetch again');if(!Array.isArray(json.rows))throw Error('Invalid readings response');return json.rows.map(r=>{
 if(!Array.isArray(r)||r.length!==6||!r.every(v=>typeof v==='number'&&Number.isFinite(v))||!Number.isSafeInteger(r[0])||r[0]<=0||!Number.isInteger(r[1])||r[1]<1||r[1]>255)throw Error('Invalid sensor reading');
 return {id:r[0],node:r[1],seq:r[2],ts:r[3],temp:r[4],rh:r[5]};});}
async function fetchAll(){if(busy||!initialized)return false;busy=true;$('refresh').disabled=true;$('base').disabled=true;let total=0;
try{
message('Connecting');await checkDataset();
// Latest readings are deliberately separate from the history cursor: persisting
// them here would skip the historical records between the cache and the latest.
try{latest=decodeRows(await request('/api/latest'));draw();message('Connected — '+latest.length+' sensors reporting');}
catch(e){if(e.message!=='HTTP 404')message('Latest readings unavailable: '+e.message+'; fetching history');}
let after=db?persistedAfter:sessionAfter;
for(;;){
const json=await request('/api/history?after='+after),rows=decodeRows(json);
if(!rows.length)break;
rows.sort((a,b)=>a.id-b.id);
if(rows.some((r,i)=>r.id<=after||(i&&r.id===rows[i-1].id)))throw Error('History page did not advance');
try{await persist(rows);}catch(e){memoryOnly(e);}
all.push(...rows);after=maxId(rows);sessionAfter=after;total+=rows.length;
message('Fetching historical readings — '+total+' downloaded');draw();
await new Promise(resolve=>setTimeout(resolve,0));
// Do not infer completion from batch size: the server also caps response bytes.
}
await checkDataset();
message('Up to date — '+all.length+' readings '+(db?'stored locally':'in memory'));draw();return true;
}catch(e){message('Fetch failed — '+e.message+' ('+total+' downloaded)');return false;}
finally{busy=false;$('refresh').disabled=false;$('base').disabled=false;}}
function download(blob,name){let a=document.createElement('a'),p=URL.createObjectURL(blob);a.href=p;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(p),2000);}
function csv(rows){return 'record_id,node_id,sequence,unix_seconds,temperature_c,humidity_percent\n'+rows.map(r=>[r.id,r.node,r.seq,r.ts,r.temp,r.rh].join(',')).join('\n')+'\n';}
function today(){return new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)+'Z';}
async function chooseFolder(){try{if(!window.showDirectoryPicker)throw Error('Folder API not available: use desktop Chrome or Edge via localhost/HTTPS');folder=await window.showDirectoryPicker({id:'esp-sensor-archives',mode:'readwrite'});$('archiveStatus').textContent='Folder selected: '+folder.name;}catch(e){$('archiveStatus').textContent=e.message;}}
async function archive(){
if(busy){$('archiveStatus').textContent='Wait for the current download to finish';return;}
if(!folder){$('archiveStatus').textContent='Choose a folder first';return;}
let archiveBusy=false;
try{
if(!await fetchAll())throw Error('Fetch must complete before archiving');
busy=true;archiveBusy=true;$('refresh').disabled=true;$('base').disabled=true;
// If previous file write completed but acknowledgment was lost, retry ONLY
// that acknowledgment. Never produce another overlapping archive file.
const pendingAck=Number(localStorage.getItem(archiveKey('pending-id'))||0);
if(pendingAck){
 const r=await fetch(url('/api/archive/ack?dataset='+encodeURIComponent(activeDataset)),{method:'POST',headers:{'Content-Type':'text/plain'},body:String(pendingAck)});
 if(!r.ok)throw Error('Archive was saved; base ACK still failed. Retry to acknowledge it.');
 localStorage.setItem(archiveKey('last-id'),String(pendingAck));
 localStorage.removeItem(archiveKey('pending-id'));
 await checkDataset();$('archiveStatus').textContent='Previous saved archive acknowledged through ID '+pendingAck;
 return;
}
const last=getArchId(),pending=all.filter(r=>r.id>last);
if(!pending.length){$('archiveStatus').textContent='No unarchived readings';return;}
const upto=pending[pending.length-1].id, name='sensors_'+today()+'_ids_'+pending[0].id+'-'+upto+'.csv';
const file=await folder.getFileHandle(name,{create:true});const writer=await file.createWritable();await writer.write(csv(pending));await writer.close();
const saved=await file.getFile();if(saved.size<70)throw Error('Archive verification failed');
localStorage.setItem(archiveKey('pending-id'),String(upto));
const response=await fetch(url('/api/archive/ack?dataset='+encodeURIComponent(activeDataset)),{method:'POST',headers:{'Content-Type':'text/plain'},body:String(upto)});
if(!response.ok)throw Error('Archive saved, but base ACK failed. Choose the same folder and retry ACK.');
localStorage.setItem(archiveKey('last-id'),String(upto));
localStorage.removeItem(archiveKey('pending-id'));
await checkDataset();$('archiveStatus').textContent='Saved '+name+' ('+pending.length+' rows), base acknowledged.';
}catch(e){$('archiveStatus').textContent='Archive error: '+e.message;}finally{if(archiveBusy){busy=false;$('refresh').disabled=false;$('base').disabled=false;}}}

let selectedSensor=null;
function toggleSensor(id){selectedSensor=selectedSensor===id?null:id;draw();}
$('cards').onclick=event=>{const card=event.target.closest('[data-sensor]');if(card)toggleSensor(Number(card.dataset.sensor));};

function draw(){const sel=$('metric').value,hours=+$('period').value,cutoff=hours?Date.now()/1000-hours*3600:0;let rows=all.filter(r=>r.ts>cutoff&&r.ts>0&&(selectedSensor===null||r.node===selectedSensor));$('count').textContent=rows.length+' readings in displayed period';
const last=new Map();for(const r of [...all,...latest])if(!last.has(r.node)||last.get(r.node).id<r.id)last.set(r.node,r);$('cards').innerHTML=[...last].sort((a,b)=>a[0]-b[0]).map(([id,r])=>`<button type="button" class="card" data-sensor="${id}" aria-pressed="${selectedSensor===id}" title="${selectedSensor===id?'Show all sensors':'Show only Sensor '+id}" style="${sensorStyle(id)}"><div class="sensor-name"><span class="sensor-dot"></span>Sensor ${id}</div><div class="sensor-number">${r.temp.toFixed(2)} °C</div><div class="sensor-rh">${r.rh.toFixed(1)} % RH</div><div class="sensor-time">${r.ts>0?new Date(r.ts*1000).toLocaleString():'Measurement time unknown'}<br>${r.ts>0?Math.max(0,Math.floor(Date.now()/1000-r.ts))+' seconds since measurement — '+(Date.now()/1000-r.ts>Math.max(120,+$('overdue').value||360)?'Overdue':'Recently seen'):'Age unknown'}</div></button>`).join('');
let canvas=$('chart'),rect=canvas.getBoundingClientRect(),w=Math.max(320,rect.width),h=Math.max(300,rect.height),dpr=devicePixelRatio||1;canvas.width=w*dpr;canvas.height=h*dpr;let c=canvas.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);let L=65,R=17,T=22,B=40,cw=w-L-R,ch=h-T-B;
if(!rows.length){$('legend').innerHTML='';c.fillStyle='#708090';c.font='14px system-ui';c.fillText('No readings for this period',L+25,T+80);return;}
let low=rows.reduce((v,r)=>Math.min(v,r[sel]),Infinity),high=rows.reduce((v,r)=>Math.max(v,r[sel]),-Infinity),minSpan=sel==='temp'?.2:2,range=Math.max(minSpan,high-low);low=(low+high-range)/2;high=low+range;low-=range*.10;high+=range*.10;
let minX=rows.reduce((v,r)=>Math.min(v,r.ts),Infinity),maxX=rows.reduce((v,r)=>Math.max(v,r.ts),-Infinity);if(maxX-minX<60){minX-=30;maxX+=30;}if(hours){maxX=Math.max(maxX,Date.now()/1000);minX=Math.max(minX,maxX-hours*3600);}
const X=x=>L+(x-minX)/(maxX-minX)*cw,Y=y=>T+(high-y)/(high-low)*ch;
c.font='12px system-ui';c.textAlign='right';c.fillStyle='#53667b';c.strokeStyle='#e3e9f1';for(let i=0;i<=5;i++){let y=T+i*ch/5, val=high-i*(high-low)/5;c.beginPath();c.moveTo(L,y);c.lineTo(w-R,y);c.stroke();c.fillText(val.toFixed((high-low)<.6?2:1),L-8,y+4);}
c.textAlign='center';for(let i=0;i<=5;i++){let ts=minX+i*(maxX-minX)/5,label=new Date(ts*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});c.fillText(label,X(ts),h-12);}
let ids=[...new Set(rows.map(r=>r.node))].sort((a,b)=>a-b);for(let id of ids){let pts=rows.filter(r=>r.node===id).sort((a,b)=>a.ts-b.ts);c.strokeStyle=sensorColor(id);c.lineWidth=2;c.beginPath();pts.forEach((r,i)=>i?c.lineTo(X(r.ts),Y(r[sel])):c.moveTo(X(r.ts),Y(r[sel])));c.stroke();pts.forEach(r=>{c.beginPath();c.arc(X(r.ts),Y(r[sel]),2.2,0,Math.PI*2);c.fillStyle=c.strokeStyle;c.fill();});}
$('legend').innerHTML=ids.map(id=>`<span class="legend-item" style="${sensorStyle(id)}"><span class="legend-dot"></span>Sensor ${id}</span>`).join('');
}
$('reset').onclick=resetAll;$('refresh').onclick=fetchAll;$('folder').onclick=chooseFolder;$('archive').onclick=archive;$('download').onclick=()=>download(new Blob([csv(all)],{type:'text/csv'}),'sensors_full_'+today()+'.csv');$('metric').onchange=draw;$('period').onchange=draw;$('overdue').onchange=draw;window.onresize=draw;
async function initialize(){
 $('refresh').disabled=true;
 const old=stored(datasetKey);
 try{db=await openDb();legacyDataset=await legacyOwner();if(old&&old!==legacyDataset){db.close();db=await openDb('esp-sensor-history-'+old);}all=await readLocal();all.sort((a,b)=>a.id-b.id);persistedAfter=sessionAfter=maxId(all);}catch(e){memoryOnly(e);}
 all=[];latest=[];persistedAfter=sessionAfter=0;draw();initialized=true;await fetchAll();setInterval(fetchAll,30000);setInterval(draw,15000);
}
initialize();
</script></body></html>
)ESPHTML";
