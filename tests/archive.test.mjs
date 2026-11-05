import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile, mkdir } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { archive,loadArchives,csv } from '../archive-app/archive.mjs';
const row=id=>[id,1,id,1800000000+id,20,50];
async function setup(t){const directory=await mkdtemp(path.join(os.tmpdir(),'sensor-archive-'));t.after(()=>rm(directory,{recursive:true,force:true}));return directory;}
function base({rows=[row(1),row(2),row(3)],ackFails=false,change=false,malformed=false,old=false,onAck}={}){
 const state={rows,acks:[],metaCalls:0};
 state.fetchImpl=async(input,options)=>{const u=new URL(input);let data;
  if(u.pathname==='/api/meta'){state.metaCalls++;data={dataset:change&&state.metaCalls>1?'b':'a',archive_protocol:old?1:2,latest_id:3,retained_after:state.rows.length?state.rows[0][0]-1:3};}
  else if(u.pathname==='/api/history')data={dataset:'a',rows:malformed?[[1,1,1,0,'bad',40]]:state.rows.filter(r=>r[0]>Number(u.searchParams.get('after'))).slice(0,2)};
  else {assert.equal(u.searchParams.get('dataset'),'a');state.acks.push(Number(options.body));await onAck?.();if(ackFails)return {ok:false,status:503,text:async()=> 'failure'};state.rows=state.rows.filter(r=>r[0]>Number(options.body));return {ok:true,text:async()=> 'OK'};}
  return {ok:true,json:async()=>data};
 };return state;
}
test('archive is readable before acknowledgement; subsequent empty run is harmless',async t=>{
 const directory=await setup(t),b=base({onAck:async()=>{assert.equal((await loadArchives(directory)).rows.length,3);}});
 const result=await archive({directory,fetchImpl:b.fetchImpl});assert.equal(result.count,3);assert.equal(b.rows.length,0);assert.equal((await archive({directory,fetchImpl:b.fetchImpl})).count,0);assert.deepEqual(b.acks,[3]);
});
test('failed acknowledgement keeps file; retry archives overlap without duplicate readings',async t=>{
 const directory=await setup(t),b=base({ackFails:true});await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}),/Archive saved/);assert.equal((await loadArchives(directory)).rows.length,3);
 await archive({directory,fetchImpl:base().fetchImpl});const loaded=await loadArchives(directory);assert.equal(loaded.files,2);assert.equal(loaded.rows.length,3);
});
test('dataset change refuses acknowledgement',async t=>{const directory=await setup(t),b=base({change:true});await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}),/reset or archived/);assert.equal(b.acks.length,0);});
test('malformed readings and old firmware refuse acknowledgement',async t=>{const directory=await setup(t);for(const options of [{malformed:true},{old:true}]){const b=base(options);await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}));assert.equal(b.acks.length,0);}});
test('truncated snapshot refuses acknowledgement',async t=>{const directory=await setup(t),b=base({rows:[row(1)]});await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}),/ended before/);assert.equal(b.acks.length,0);});
test('new uploads after snapshot are not acknowledged',async t=>{const directory=await setup(t),b=base({rows:[row(1),row(2),row(3),row(4)]});await archive({directory,fetchImpl:b.fetchImpl});assert.deepEqual(b.rows,[row(4)]);});
test('lock prevents concurrent runs',async t=>{const directory=await setup(t);await writeFile(path.join(directory,'.archive.lock'),'123');const b=base();await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}),/Another archive run/);assert.equal(b.metaCalls,0);});
test('write failure never acknowledges',async t=>{const directory=await setup(t);await writeFile(path.join(directory,'a'),'blocks dataset directory');const b=base();await assert.rejects(archive({directory,fetchImpl:b.fetchImpl}));assert.equal(b.acks.length,0);});
test('datasets isolate reused IDs and conflicting files fail visibly',async t=>{const directory=await setup(t);for(const dataset of ['a','b']){await mkdir(path.join(directory,dataset));await writeFile(path.join(directory,dataset,'one.csv'),csv([row(1)]));}assert.equal((await loadArchives(directory)).rows.length,2);const changed=row(1);changed[4]=22;await writeFile(path.join(directory,'a','two.csv'),csv([changed]));await assert.rejects(loadArchives(directory),/Conflicting/);});
