import { mkdir, open, readFile, rename, readdir, unlink } from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
export const header='record_id,node_id,sequence,unix_seconds,temperature_c,humidity_percent';
export function validateRow(r){
 if(!Array.isArray(r)||r.length!==6||!r.every(Number.isFinite)||!r.slice(0,4).every(Number.isSafeInteger)||r[0]<1||r[1]<1||r[1]>255||r[2]<0||r[3]<0)throw Error('Invalid sensor reading');
 return r;
}
export const csv=rows=>header+'\n'+rows.map(r=>r.join(',')).join('\n')+'\n';
export function parseCSV(text){
 const lines=text.trim().split(/\r?\n/);if(lines.shift()!==header)throw Error('Unrecognised CSV header');
 return lines.filter(Boolean).map(line=>{const cells=line.split(',');if(cells.some(c=>!c.trim()))throw Error('Empty CSV value');return validateRow(cells.map(Number));});
}
async function durableFile(filename,contents){
 const temp=filename+'.'+randomUUID()+'.tmp';
 const handle=await open(temp,'wx');
 try{await handle.writeFile(contents);await handle.sync();}finally{await handle.close();}
 if(await readFile(temp,'utf8')!==contents)throw Error('Archive verification failed');
 await rename(temp,filename);
 const dir=await open(path.dirname(filename),'r');try{await dir.sync();}finally{await dir.close();}
 if(await readFile(filename,'utf8')!==contents)throw Error('Saved archive verification failed');
}
export async function archive({base='http://temp.local',directory,fetchImpl=fetch}){
 const request=async(endpoint,options={})=>{
  const response=await fetchImpl(new URL(endpoint,base),{...options,signal:AbortSignal.timeout(20000)});
  if(!response.ok)throw Error(`Base HTTP ${response.status}: ${await response.text()}`);
  return options.method?response.text():response.json();
 };
 await mkdir(directory,{recursive:true});
 const lockPath=path.join(directory,'.archive.lock');
 let lock;try{lock=await open(lockPath,'wx');}catch(e){if(e.code==='EEXIST')throw Error('Another archive run is active (or a crashed run left .archive.lock; see README).');throw e;}
 try{
  await lock.writeFile(String(process.pid));
  const meta=await request('/api/meta');
  if(meta.archive_protocol!==2)throw Error('Flash the updated base firmware before archiving; dataset-safe acknowledgement is required.');
  if(!/^[a-zA-Z0-9_-]{1,100}$/.test(meta.dataset)||!Number.isSafeInteger(meta.latest_id)||meta.latest_id<0||!Number.isSafeInteger(meta.retained_after)||meta.retained_after<0||meta.retained_after>meta.latest_id)throw Error('Invalid base metadata');
  const rows=[];let after=meta.retained_after;
  while(after<meta.latest_id){
   const page=await request('/api/history?after='+after);
   if(page.dataset!==meta.dataset||!Array.isArray(page.rows))throw Error('Dataset changed or invalid history');
   if(!page.rows.length)throw Error('History ended before the snapshot was complete; nothing acknowledged');
   for(const raw of page.rows){const r=validateRow(raw);if(r[0]<=after)throw Error('History did not advance');after=r[0];if(r[0]<=meta.latest_id)rows.push(r);}
  }
  if(!rows.length)return {count:0,message:'No readings to archive.'};
  const current=await request('/api/meta');
  if(current.dataset!==meta.dataset||current.retained_after!==meta.retained_after)throw Error('Base was reset or archived during download; retry');
  const folder=path.join(directory,meta.dataset);await mkdir(folder,{recursive:true});
  // Sync the parent as well so the dataset directory survives before clearing the base.
  const parent=await open(directory,'r');try{await parent.sync();}finally{await parent.close();}
  const filename=path.join(folder,`${new Date().toISOString().replace(/[:.]/g,'-')}_${rows[0][0]}-${rows.at(-1)[0]}_${randomUUID()}.csv`);
  await durableFile(filename,csv(rows));
  try{await request('/api/archive/ack?dataset='+encodeURIComponent(meta.dataset),{method:'POST',headers:{'Content-Type':'text/plain'},body:String(rows.at(-1)[0])});}
  catch(e){throw Error(`Archive saved at ${filename}, but acknowledgement failed. Rerun safely; overlapping files are deduplicated. ${e.message}`);}
  return {count:rows.length,filename,message:`Saved ${rows.length} readings and cleared them from the base.`};
 }finally{await lock.close();await unlink(lockPath);}
}
export async function loadArchives(directory){
 const rows=new Map();let files=0;
 await mkdir(directory,{recursive:true});
 for(const entry of await readdir(directory,{withFileTypes:true})){
  if(!entry.isDirectory()||!/^[a-zA-Z0-9_-]{1,100}$/.test(entry.name))continue;
  for(const file of await readdir(path.join(directory,entry.name),{withFileTypes:true})){
   if(!file.isFile()||!file.name.endsWith('.csv'))continue;
   const filename=path.join(directory,entry.name,file.name);
   let parsed;try{parsed=parseCSV(await readFile(filename,'utf8'));}catch(e){throw Error(`${filename}: ${e.message}`);}
   for(const r of parsed){const key=entry.name+':'+r[0],old=rows.get(key);if(old&&JSON.stringify(old.values)!==JSON.stringify(r))throw Error(`Conflicting readings in ${filename}, record ${r[0]}`);rows.set(key,{dataset:entry.name,values:r});}
   files++;
  }
 }
 return {files,rows:[...rows.values()].sort((a,b)=>a.values[3]-b.values[3]||a.values[0]-b.values[0])};
}
