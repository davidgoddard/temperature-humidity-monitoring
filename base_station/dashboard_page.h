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
.card{padding:20px 22px 22px;border:1px solid var(--sensor-border);border-left:6px solid var(--sensor-color);border-radius:14px;background:var(--sensor-bg);min-height:168px;box-shadow:0 2px 5px rgba(0,0,0,.025)}
.card .sensor-name{color:var(--sensor-color);font-weight:750;font-size:15px;display:flex;align-items:center;gap:9px;margin-bottom:13px}
.card .sensor-dot,.legend-dot{display:inline-block;width:12px;height:12px;border-radius:50%;background:var(--sensor-color);flex:none}
.card .sensor-number{font-size:32px;line-height:1.14;font-weight:750;letter-spacing:-.7px;margin-bottom:8px;color:#203042}
.card .sensor-rh{font-size:16px;margin-bottom:8px}.card .sensor-time{font-size:12px;color:#586d82}
.legend{display:flex;gap:12px;flex-wrap:wrap;padding:13px 2px 3px}
.legend-item{display:inline-flex;gap:9px;align-items:center;padding:7px 12px;border-radius:20px;border:1px solid var(--sensor-border);background:var(--sensor-bg);font-size:13px;font-weight:650;color:var(--sensor-color)}
small{color:#65778b}
@media(max-width:640px){body{padding:17px 12px 36px}section{padding:18px 15px;margin:17px 0}.cards{grid-template-columns:1fr;gap:13px;margin-top:20px}.card{min-height:0}#chart{height:340px}.controls{gap:12px}}
</style></head>
<body><h1>Wi-Fi Temperature Dashboard</h1><p>Reads ESP32 base station history over local Wi-Fi, combines it with readings retained in this browser, and archives non-overlapping CSV snapshots.</p>
<section><div class="controls"><label>Base URL <input id="base" value="http://temp.local"></label><button class="primary" id="refresh">Fetch readings</button><span id="status">Not connected</span></div><p><small>Dashboard and API are served by the base at http://temp.local. Refresh retrieves available measurements over Wi-Fi.</small></p></section>
<section><div class="controls"><label>Show <select id="metric"><option value="temp">Temperature (°C)</option><option value="rh">Humidity (%)</option></select></label><label>Period <select id="period"><option value="1">1 hour</option><option value="6" selected>6 hours</option><option value="24">24 hours</option><option value="168">7 days</option><option value="720">30 days</option><option value="0">All</option></select></label><span id="count"></span></div><div id="cards" class="cards"></div><div class="graph-area"><canvas id="chart"></canvas><div id="legend" class="legend"></div></div></section>
<section><h3>Archive and export</h3><p>Choose an archive folder in Chrome or Edge, then save the outstanding readings. The base deletes acknowledged readings <strong>only after</strong> the browser has written the archive file successfully.</p><div class="controls"><button id="folder">Choose archive folder</button><button id="archive" class="primary">Save snapshot + acknowledge</button><button id="download">Download CSV (no deletion)</button><span id="archiveStatus"></span></div><p><small>Important: direct folder writing is unavailable at ordinary http://temp.local because it is not a secure browser context. Use Download CSV and choose its destination in the browser, or serve this file from localhost/HTTPS to enable Choose archive folder and automatic base acknowledgement.</small></p></section>
<section><h3>Reset base station readings</h3><p>This permanently deletes all readings held by the base station and clears this browser's stored sensor history. Previously downloaded CSV archives are not deleted. Sensors will continue uploading new readings.</p><div class="controls"><button id="reset" class="danger">Reset all readings…</button><span id="resetStatus"></span></div></section>
<script>
'use strict';
const $=s=>document.getElementById(s);const palette=['#2563eb','#db2777','#0f766e','#9a6700','#7c3aed','#b45309','#0e7490','#be185d'];
const sensorColor=id=>palette[((Number(id)-1)%palette.length+palette.length)%palette.length];
const sensorStyle=id=>{let color=sensorColor(id);return `--sensor-color:${color};--sensor-bg:${color}0d;--sensor-border:${color}44;`;};
let db, all=[], folder=null, busy=false;
const datasetKey='esp-sensor-dataset-id';
async function clearBrowserReadings() {
  const tx=db.transaction('samples','readwrite');
  tx.objectStore('samples').clear();
  await txDone(tx);
  all=[];
  localStorage.removeItem('esp-archive-last-id');
  localStorage.removeItem('esp-archive-pending-id');
  draw();
}
async function checkDataset() {
  const response=await fetch(url('/api/meta'),{cache:'no-store'});
  if(!response.ok)throw Error('Dataset check: HTTP '+response.status);
  const data=await response.json();
  if(typeof data.dataset!=='string'||!data.dataset)throw Error('Missing dataset identifier');
  const old=localStorage.getItem(datasetKey);
  if(old&&old!==data.dataset){
    await clearBrowserReadings();
    message('Base station was reset; old browser readings cleared');
  }
  localStorage.setItem(datasetKey,data.dataset);
}
async function resetAll() {
  if(busy){$('resetStatus').textContent='Wait for the current download to finish';return;}
  if(!confirm('Permanently delete all measurements on the base AND this browser? This cannot be undone.'))return;
  const phrase=prompt('Type RESET to confirm removal of all readings:');
  if(phrase!=='RESET')return;
  busy=true;$('reset').disabled=true;$('refresh').disabled=true;
  try{
    const response=await fetch(url('/api/reset'),{method:'POST',headers:{'Content-Type':'text/plain'},body:'RESET ALL READINGS'});
    if(!response.ok)throw Error('Base reset HTTP '+response.status+': '+await response.text());
    const result=await response.json();
    await clearBrowserReadings();
    localStorage.setItem(datasetKey,result.dataset);
    $('resetStatus').textContent='Base and browser readings cleared';
    message('Connected · empty history');
  }catch(e){
    $('resetStatus').textContent='Reset incomplete: '+e.message;
    // If the base succeeded but local clearing failed, dataset check on refresh
    // will detect the generation change.
  }finally{busy=false;$('reset').disabled=false;$('refresh').disabled=false;}
}
const url=path=>$('base').value.replace(/\/$/,'')+path;
function message(s){$('status').textContent=s;}
const openDb=()=>new Promise((resolve,reject)=>{let r=indexedDB.open('esp-sensor-history-v1',1);r.onupgradeneeded=()=>r.result.createObjectStore('samples',{keyPath:'id'});r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);});
function txDone(tx){return new Promise((resolve,reject)=>{tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);tx.onabort=()=>reject(tx.error);});}
async function persist(rows){if(!rows.length)return;let tx=db.transaction('samples','readwrite');let store=tx.objectStore('samples');for(const r of rows)store.put(r);await txDone(tx);}
function readLocal(){return new Promise((resolve,reject)=>{let req=db.transaction('samples').objectStore('samples').getAll();req.onsuccess=()=>resolve(req.result);req.onerror=()=>reject(req.error);});}
const getArchId=()=>Number(localStorage.getItem('esp-archive-last-id')||0);
async function fetchAll(){if(busy)return;busy=true;$('refresh').disabled=true;try{
await checkDataset();
let after=0,total=0;
for(let page=0;page<10000;page++){
let response=await fetch(url('/api/history?after='+after),{cache:'no-store'});if(!response.ok)throw Error('HTTP '+response.status);
let json=await response.json();if(!Array.isArray(json.rows)||!json.rows.length)break;
let rows=json.rows.map(r=>({id:+r[0],node:+r[1],seq:+r[2],ts:+r[3],temp:+r[4],rh:+r[5]}));
await persist(rows);after=rows[rows.length-1].id;total+=rows.length;
if(rows.length<100)break;
}
all=await readLocal();all.sort((a,b)=>a.id-b.id);message('Connected · '+total+' fetched · '+all.length+' in browser history');draw();
}catch(e){message('Fetch error: '+e.message);}finally{busy=false;$('refresh').disabled=false;}}
function download(blob,name){let a=document.createElement('a'),p=URL.createObjectURL(blob);a.href=p;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(p),2000);}
function csv(rows){return 'record_id,node_id,sequence,unix_seconds,temperature_c,humidity_percent\n'+rows.map(r=>[r.id,r.node,r.seq,r.ts,r.temp,r.rh].join(',')).join('\n')+'\n';}
function today(){return new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)+'Z';}
async function chooseFolder(){try{if(!window.showDirectoryPicker)throw Error('Folder API not available: use desktop Chrome or Edge via localhost/HTTPS');folder=await window.showDirectoryPicker({id:'esp-sensor-archives',mode:'readwrite'});$('archiveStatus').textContent='Folder selected: '+folder.name;}catch(e){$('archiveStatus').textContent=e.message;}}
async function archive(){
if(!folder){$('archiveStatus').textContent='Choose a folder first';return;}
try{
// If previous file write completed but acknowledgment was lost, retry ONLY
// that acknowledgment. Never produce another overlapping archive file.
const pendingAck=Number(localStorage.getItem('esp-archive-pending-id')||0);
if(pendingAck){
 const r=await fetch(url('/api/archive/ack'),{method:'POST',headers:{'Content-Type':'text/plain'},body:String(pendingAck)});
 if(!r.ok)throw Error('Archive was saved; base ACK still failed. Retry to acknowledge it.');
 localStorage.setItem('esp-archive-last-id',String(pendingAck));
 localStorage.removeItem('esp-archive-pending-id');
 $('archiveStatus').textContent='Previous saved archive acknowledged through ID '+pendingAck;
 return;
}
await fetchAll();
const last=getArchId(),pending=all.filter(r=>r.id>last);
if(!pending.length){$('archiveStatus').textContent='No unarchived readings';return;}
const upto=pending[pending.length-1].id, name='sensors_'+today()+'_ids_'+pending[0].id+'-'+upto+'.csv';
const file=await folder.getFileHandle(name,{create:true});const writer=await file.createWritable();await writer.write(csv(pending));await writer.close();
const saved=await file.getFile();if(saved.size<70)throw Error('Archive verification failed');
localStorage.setItem('esp-archive-pending-id',String(upto));
const response=await fetch(url('/api/archive/ack'),{method:'POST',headers:{'Content-Type':'text/plain'},body:String(upto)});
if(!response.ok)throw Error('Archive saved, but base ACK failed. Choose the same folder and retry ACK.');
localStorage.setItem('esp-archive-last-id',String(upto));
localStorage.removeItem('esp-archive-pending-id');
$('archiveStatus').textContent='Saved '+name+' ('+pending.length+' rows), base acknowledged.';
}catch(e){$('archiveStatus').textContent='Archive error: '+e.message;}}

function draw(){const sel=$('metric').value,hours=+$('period').value,cutoff=hours?Date.now()/1000-hours*3600:0;let rows=all.filter(r=>r.ts>cutoff&&r.ts>0);$('count').textContent=rows.length+' readings in displayed period';
const last=new Map();for(const r of rows)last.set(r.node,r);$('cards').innerHTML=[...last].sort((a,b)=>a[0]-b[0]).map(([id,r])=>`<div class="card" style="${sensorStyle(id)}"><div class="sensor-name"><span class="sensor-dot"></span>Sensor ${id}</div><div class="sensor-number">${r.temp.toFixed(2)} °C</div><div class="sensor-rh">${r.rh.toFixed(1)} % RH</div><div class="sensor-time">${new Date(r.ts*1000).toLocaleString()}</div></div>`).join('');
let canvas=$('chart'),rect=canvas.getBoundingClientRect(),w=Math.max(320,rect.width),h=Math.max(300,rect.height),dpr=devicePixelRatio||1;canvas.width=w*dpr;canvas.height=h*dpr;let c=canvas.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);let L=65,R=17,T=22,B=40,cw=w-L-R,ch=h-T-B;
if(!rows.length){$('legend').innerHTML='';c.fillStyle='#708090';c.font='14px system-ui';c.fillText('No readings for this period',L+25,T+80);return;}
let a=rows.map(r=>r[sel]),low=Math.min(...a),high=Math.max(...a),minSpan=sel==='temp'?.2:2,range=Math.max(minSpan,high-low);low=(low+high-range)/2;high=low+range;low-=range*.10;high+=range*.10;
let minX=Math.min(...rows.map(r=>r.ts)),maxX=Math.max(...rows.map(r=>r.ts));if(maxX-minX<60){minX-=30;maxX+=30;}if(hours){maxX=Math.max(maxX,Date.now()/1000);minX=Math.max(minX,maxX-hours*3600);}
const X=x=>L+(x-minX)/(maxX-minX)*cw,Y=y=>T+(high-y)/(high-low)*ch;
c.font='12px system-ui';c.textAlign='right';c.fillStyle='#53667b';c.strokeStyle='#e3e9f1';for(let i=0;i<=5;i++){let y=T+i*ch/5, val=high-i*(high-low)/5;c.beginPath();c.moveTo(L,y);c.lineTo(w-R,y);c.stroke();c.fillText(val.toFixed((high-low)<.6?2:1),L-8,y+4);}
c.textAlign='center';for(let i=0;i<=5;i++){let ts=minX+i*(maxX-minX)/5,label=new Date(ts*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});c.fillText(label,X(ts),h-12);}
let ids=[...new Set(rows.map(r=>r.node))].sort((a,b)=>a-b);for(let id of ids){let pts=rows.filter(r=>r.node===id).sort((a,b)=>a.ts-b.ts);c.strokeStyle=sensorColor(id);c.lineWidth=2;c.beginPath();pts.forEach((r,i)=>i?c.lineTo(X(r.ts),Y(r[sel])):c.moveTo(X(r.ts),Y(r[sel])));c.stroke();pts.forEach(r=>{c.beginPath();c.arc(X(r.ts),Y(r[sel]),2.2,0,Math.PI*2);c.fillStyle=c.strokeStyle;c.fill();});}
$('legend').innerHTML=ids.map(id=>`<span class="legend-item" style="${sensorStyle(id)}"><span class="legend-dot"></span>Sensor ${id}</span>`).join('');
}
$('reset').onclick=resetAll;$('refresh').onclick=fetchAll;$('folder').onclick=chooseFolder;$('archive').onclick=archive;$('download').onclick=()=>download(new Blob([csv(all)],{type:'text/csv'}),'sensors_full_'+today()+'.csv');$('metric').onchange=draw;$('period').onchange=draw;window.onresize=draw;
(async()=>{try{db=await openDb();all=await readLocal();all.sort((a,b)=>a.id-b.id);draw();await fetchAll();setInterval(fetchAll,30000);}catch(e){message('Database: '+e.message);}})();
</script></body></html>
)ESPHTML";
