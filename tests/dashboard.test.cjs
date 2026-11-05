const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync('dashboard.html','utf8').split('<script>')[1].split('</script>')[0].replace(/initialize\(\);\s*$/, '');
function database(seed=[]){
 const databases=new Map([['esp-sensor-history-v1',{samples:new Map(seed.map(r=>[r.id,r])),metadata:new Map()}]]);
 const state={databases,failWrite:false,unavailable:false};
 state.open=(name)=>{
  if(state.unavailable)throw Error('Storage disabled');
  const req={};setImmediate(()=>{
   let created=!databases.has(name);if(created)databases.set(name,{samples:new Map(),metadata:new Map()});
   const data=databases.get(name);
   req.result={close(){},objectStoreNames:{contains:k=>k in data},createObjectStore(k){data[k]=new Map();},transaction(name,mode){
    const working=new Map(data[name]);let pending=0,finished=false;
    const tx={error:null,objectStore(){return {getAll:()=>request(()=>[...working.values()]),get:k=>request(()=>working.get(k)),put(v,k){working.set(k??v.id,structuredClone(v));},clear(){working.clear();}};}};
    function finish(){setImmediate(()=>{if(finished||pending)return;finished=true;if(state.failWrite&&name==='samples'&&mode==='readwrite'){tx.error=Error('Quota exceeded');tx.onabort?.();}else{if(mode==='readwrite')data[name]=working;tx.oncomplete?.();}});}
    function request(fn){pending++;const r={};setImmediate(()=>{r.result=fn();r.onsuccess?.();pending--;finish();});return r;}
    finish();return tx;
   }};
   if(created)req.onupgradeneeded?.();req.onsuccess?.();
  });return req;
 };
 return state;
}
const row=id=>({id,node:1,seq:id,ts:1800000000,temp:20,rh:50});
const wire=r=>[r.id,r.node,r.seq,r.ts,r.temp,r.rh];
function harness({storage=database(),local=new Map(),history=[],dataset='a',latest=[],onHistory,realDraw=false}={}){
 const elements=new Map();const el=id=>{if(!elements.has(id))elements.set(id,{value:({base:'http://temp.local',period:'6',metric:'temp',overdue:'360'})[id]||'',textContent:'',disabled:false});return elements.get(id);};
 const calls=[];let draws=0;
 const ctx=vm.createContext({console,Map,Date,Promise,Error,AbortController,Number,Array,Math,Set,Blob,setTimeout,clearTimeout,setInterval(){},indexedDB:storage,
 localStorage:{getItem:k=>local.get(k)||null,setItem:(k,v)=>local.set(k,String(v)),removeItem:k=>local.delete(k)},document:{getElementById:el},window:{},
 fetch:async url=>{calls.push(url);let body;if(url.endsWith('/api/meta'))body={dataset};else if(url.endsWith('/api/latest'))body={rows:latest.map(wire)};else{const after=+url.split('after=')[1];if(onHistory){const result=await onHistory(after);if(result)return result;}body={rows:history.filter(r=>r.id>after).slice(0,2).map(wire)};}return {ok:true,json:async()=>body};},
 });vm.runInContext(source,ctx);ctx.recordDraw=()=>draws++;if(!realDraw)vm.runInContext('draw=()=>recordDraw()',ctx);
 return {ctx,calls,el,local,storage,get draws(){return draws;},run:code=>vm.runInContext(code,ctx),init:()=>vm.runInContext('initialize()',ctx)};
}
test('empty database backfills gaps, displays batches and polls incrementally',async()=>{
 const h=harness({history:[row(2),row(9),row(100)]});await h.init();assert.deepEqual(h.calls.filter(x=>x.includes('history')).map(x=>+x.split('=')[1]),[0,9,100]);assert.equal(h.run('persistedAfter'),100);assert.equal(h.run('all.length'),3);assert.ok(h.draws>=4);await h.run('fetchAll()');assert.ok(h.calls.filter(x=>x.includes('history')).at(-1).endsWith('after=100'));
});
test('legacy cache migrates without deletion; reload resumes persisted cursor',async()=>{
 const storage=database([row(2),row(9)]),local=new Map([['esp-sensor-dataset-id','a']]);
 let h=harness({storage,local,history:[row(2),row(9),row(100)]});await h.init();assert.ok(h.calls.find(x=>x.includes('history')).endsWith('after=9'));
 h=harness({storage,local,history:[row(2),row(9),row(100)]});await h.init();assert.ok(h.calls.find(x=>x.includes('history')).endsWith('after=100'));assert.equal(h.run('all.length'),3);
});
test('manual clicks and polling share a single busy guard',async()=>{
 let release;const h=harness({onHistory:()=>new Promise(r=>release=r)});const init=h.init();while(!release)await new Promise(setImmediate);
 assert.equal(await h.run('fetchAll()'),false);assert.equal(await h.run('fetchAll()'),false);assert.equal(h.calls.filter(x=>x.includes('history')).length,1);release({ok:true,json:async()=>({rows:[]})});await init;assert.equal(h.el('refresh').disabled,false);
});
test('HTTP errors are visible and retryable',async()=>{
 let fail=true;const h=harness({onHistory:()=>fail?{ok:false,status:500}:null});await h.init();assert.match(h.el('status').textContent,/HTTP 500/);assert.equal(h.run('busy'),false);fail=false;assert.equal(await h.run('fetchAll()'),true);
});
test('write failure does not advance durable cursor, memory data visible, reload retries',async()=>{
 const storage=database([row(2)]),local=new Map([['esp-sensor-dataset-id','a']]);storage.failWrite=true;
 let h=harness({storage,local,history:[row(2),row(9)]});await h.init();assert.equal(h.run('persistedAfter'),2);assert.equal(h.run('sessionAfter'),9);assert.equal(h.run('all.length'),2);assert.match(h.el('status').textContent,/not stored persistently/);
 storage.failWrite=false;h=harness({storage,local,history:[row(2),row(9)]});await h.init();assert.ok(h.calls.find(x=>x.includes('history')).endsWith('after=2'));
});
test('unavailable IndexedDB still displays latest and history',async()=>{
 const storage=database();storage.unavailable=true;const h=harness({storage,history:[row(4)],latest:[row(40)]});await h.init();assert.equal(h.run('latest[0].id'),40);assert.equal(h.run('all[0].id'),4);assert.match(h.el('status').textContent,/not stored persistently/);
});
test('latest appears before history and cannot skip backfill',async()=>{
 let release;const h=harness({latest:[row(999)],onHistory:()=>new Promise(r=>release=r)});const init=h.init();while(!release)await new Promise(setImmediate);assert.equal(h.run('latest[0].id'),999);assert.equal(h.run('persistedAfter'),0);assert.ok(h.calls.at(-1).endsWith('after=0'));release({ok:true,json:async()=>({rows:[]})});await init;
});
test('dataset change preserves original history and isolates reused IDs',async()=>{
 const storage=database([row(99)]),local=new Map([['esp-sensor-dataset-id','a']]);const h=harness({storage,local,dataset:'b',history:[row(1)]});await h.init();assert.equal(storage.databases.get('esp-sensor-history-v1').samples.get(99).id,99);assert.equal(h.run('all.length'),1);assert.equal(h.run('all[0].id'),1);assert.match(h.el('status').textContent,/previous browser history retained/);
});
test('nonadvancing history fails instead of looping',async()=>{
 const h=harness({onHistory:()=>({ok:true,json:async()=>({rows:[wire(row(5))]})})});await h.init();assert.match(h.el('status').textContent,/did not advance/);assert.equal(h.run('all.length'),1);
});
test('blocked database falls back and closes late success',async()=>{
 const h=harness();let closed=false;h.ctx.indexedDB={open(){const r={};setImmediate(()=>{r.onblocked();r.result={close(){closed=true;}};r.onsuccess();});return r;}};
 await assert.rejects(h.run('openDb()'),/blocked/);assert.equal(closed,true);
});
test('embedded HTML matches source',()=>{assert.equal(fs.readFileSync('base_station/dashboard_page.h','utf8').split('R"ESPHTML(')[1].split(')ESPHTML";')[0],fs.readFileSync('dashboard.html','utf8'));});
test('reload after an interrupted backfill resumes the last committed batch',async()=>{
 const storage=database(),local=new Map();let h=harness({storage,local,history:[row(3),row(8),row(21)],onHistory:after=>after===8?{ok:false,status:503}:null});await h.init();assert.equal(h.run('persistedAfter'),8);assert.match(h.el('status').textContent,/HTTP 503/);
 h=harness({storage,local,history:[row(3),row(8),row(21)]});await h.init();assert.ok(h.calls.find(x=>x.includes('history')).endsWith('after=8'));assert.equal(h.run('all.length'),3);
});
test('dataset changes mid-download cannot mix histories',async()=>{
 const h=harness({onHistory:()=>({ok:true,json:async()=>({dataset:'b',rows:[wire(row(1))]})})});await h.init();assert.equal(h.run('all.length'),0);assert.match(h.el('status').textContent,/Dataset changed during download/);
});
test('unknown legacy ownership preserves the original database',async()=>{
 const storage=database([row(99)]);const h=harness({storage,history:[row(1)]});await h.init();assert.equal(storage.databases.get('esp-sensor-history-v1').samples.size,1);assert.equal(h.run('all[0].id'),1);
});
test('localStorage unavailable still allows dataset-specific IndexedDB persistence',async()=>{
 const h=harness({history:[row(1)]});h.ctx.localStorage={getItem(){throw Error('disabled');},setItem(){throw Error('disabled');},removeItem(){throw Error('disabled');}};await h.init();assert.equal(h.run('persistedAfter'),1);assert.equal(h.run('all.length'),1);
});

test('sensor cards isolate chart data and toggle back to all sensors',()=>{
 const h=harness({realDraw:true});
 const points=[];
 const canvasContext=new Proxy({arc(x,y){points.push([x,y]);}}, {get:(o,k)=>o[k]??(()=>{})});
 h.ctx.devicePixelRatio=1;
 h.el('chart').getBoundingClientRect=()=>({width:800,height:400});
 h.el('chart').getContext=()=>canvasContext;
 h.el('period').value='0';
 h.run('all=[{id:1,node:1,ts:1800000000,temp:20,rh:50},{id:2,node:2,ts:1800000060,temp:22,rh:60}];draw()');
 assert.equal(points.length,2);
 const click=id=>h.el('cards').onclick({target:{closest:()=>({dataset:{sensor:String(id)}})}});
 points.length=0;click(2);
 assert.equal(points.length,1);
 assert.match(h.el('legend').innerHTML,/Sensor 2/);
 assert.doesNotMatch(h.el('legend').innerHTML,/Sensor 1/);
 assert.match(h.el('cards').innerHTML,/data-sensor="2" aria-pressed="true"/);
 assert.match(h.el('count').textContent,/^1 readings/);
 h.el('metric').value='rh';h.el('metric').onchange();
 assert.equal(h.run('selectedSensor'),2);
 click(1);assert.equal(h.run('selectedSensor'),1);
 points.length=0;click(1);
 assert.equal(points.length,2);
 assert.equal(h.run('selectedSensor'),null);
 assert.match(h.el('legend').innerHTML,/Sensor 1/);
 assert.match(h.el('legend').innerHTML,/Sensor 2/);
});

test('archive metadata removes cached history and sensor cards and persists removal',async()=>{
 const storage=database([row(1),row(2),row(3)]),local=new Map([['esp-sensor-dataset-id','a']]);
 const h=harness({storage,local,history:[row(1),row(2),row(3)]});await h.init();
 h.run('latest=[{id:2,node:1}];');await h.run('pruneArchived({retained_after:2})');
 assert.equal(h.run('all.length'),1);assert.equal(h.run('all[0].id'),3);assert.equal(h.run('latest.length'),0);
 assert.deepEqual([...storage.databases.get('esp-sensor-history-v1').samples.keys()],[3]);
 await h.run('pruneArchived({retained_after:3})');assert.equal(h.run('all.length'),0);
});
