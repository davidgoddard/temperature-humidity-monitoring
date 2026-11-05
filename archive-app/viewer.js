'use strict';
const $=id=>document.getElementById(id),palette=['#2563eb','#db2777','#0f766e','#9a6700','#7c3aed','#b45309'];
let data=[],selected=[];
const color=node=>palette[(node-1)%palette.length];
function options(id,values,label){const el=$(id),old=el.value;el.replaceChildren(new Option(label,''),...values.map(v=>new Option(v,v)));el.value=values.map(String).includes(old)?old:'';}
async function reload(){
 $('reload').disabled=true;
 try{const response=await fetch('/api/archives');const json=await response.json();if(!response.ok)throw Error(json.error||'Could not load archives');data=json.rows;$('files').textContent=json.files;options('sensor',[...new Set(data.map(r=>r.values[1]))].sort((a,b)=>a-b),'All sensors');options('dataset',[...new Set(data.map(r=>r.dataset))].sort(),'All datasets');$('status').textContent=data.length?`Loaded ${data.length.toLocaleString()} unique readings. Overlapping archives are combined automatically.`:'No archives yet. Run the weekly archive command, then reload here.';draw();}
 catch(e){$('status').textContent='Could not load archives: '+e.message;}
 finally{$('reload').disabled=false;}
}
function draw(){
 const start=$('from').value?new Date($('from').value).getTime()/1000:-Infinity,end=$('to').value?new Date($('to').value).getTime()/1000:Infinity;
 const invalid=start>end;$('from').setCustomValidity(invalid?'Start must be before end':'');
 selected=data.filter(r=>(!$('sensor').value||r.values[1]===Number($('sensor').value))&&(!$('dataset').value||r.dataset===$('dataset').value)&&(r.values[3]>0?r.values[3]>=start&&r.values[3]<=end:start===-Infinity&&end===Infinity));
 if(invalid)selected=[];
 $('count').textContent=selected.length.toLocaleString();$('sensors').textContent=new Set(selected.map(r=>r.values[1])).size;
 const timed=selected.filter(r=>r.values[3]>0),unknown=selected.length-timed.length;
 $('coverage').textContent=invalid?'Choose a start time before the end time.':timed.length?`${new Date(timed[0].values[3]*1000).toLocaleString()} — ${new Date(timed.at(-1).values[3]*1000).toLocaleString()}. ${unknown?unknown+' readings have unknown times and are omitted from the graph.':''}`:'No timestamped readings in this period.';
 const canvas=$('chart'),rect=canvas.getBoundingClientRect(),w=rect.width,h=rect.height,dpr=window.devicePixelRatio||1;canvas.width=w*dpr;canvas.height=h*dpr;
 const ctx=canvas.getContext('2d');ctx.scale(dpr,dpr);ctx.clearRect(0,0,w,h);$('legend').replaceChildren();
 if(timed.length){
  const metric=Number($('metric').value),L=55,R=20,T=20,B=65,cw=w-L-R,ch=h-T-B;
  let min=Infinity,max=-Infinity;for(const r of timed){min=Math.min(min,r.values[metric]);max=Math.max(max,r.values[metric]);}const pad=Math.max((max-min)*.1,metric===4?.2:1);min-=pad;max+=pad;
  let first=timed[0].values[3],last=timed.at(-1).values[3];if(first===last){first-=30;last+=30;}
  const X=t=>L+(t-first)/(last-first)*cw,Y=v=>T+(max-v)/(max-min)*ch;
  ctx.font='12px system-ui';ctx.strokeStyle='#e3e9f1';ctx.fillStyle='#596a7f';ctx.textAlign='right';
  for(let i=0;i<=5;i++){const y=T+ch*i/5;ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(w-R,y);ctx.stroke();ctx.fillText((max-(max-min)*i/5).toFixed(1),L-8,y+4);}
  ctx.textAlign='center';for(let i=0;i<4;i++){const t=first+(last-first)*i/3,d=new Date(t*1000),x=Math.max(45,Math.min(w-45,X(t)));ctx.fillText(d.toLocaleDateString(undefined,{day:'numeric',month:'short',year:'2-digit'}),x,h-32);ctx.fillText(d.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit'}),x,h-14);}
  const groups=new Map();for(const r of timed){const key=r.dataset+':'+r.values[1];if(!groups.has(key))groups.set(key,[]);groups.get(key).push(r.values);}
  for(const points of groups.values()){
   ctx.strokeStyle=color(points[0][1]);ctx.fillStyle=ctx.strokeStyle;ctx.lineWidth=1.5;ctx.beginPath();
   points.forEach((r,i)=>{if(!i||r[3]-points[i-1][3]>3600)ctx.moveTo(X(r[3]),Y(r[metric]));else ctx.lineTo(X(r[3]),Y(r[metric]));});ctx.stroke();
   if(points.length<500)for(const r of points){ctx.beginPath();ctx.arc(X(r[3]),Y(r[metric]),2,0,Math.PI*2);ctx.fill();}
  }
  for(const node of [...new Set(timed.map(r=>r.values[1]))].sort((a,b)=>a-b)){const label=document.createElement('span'),dot=document.createElement('span');dot.className='swatch';dot.style.background=color(node);label.append(dot,`Sensor ${node}`);$('legend').append(label);}
 }
 $('readings').replaceChildren();for(const r of selected.slice(-100).reverse()){const tr=document.createElement('tr');for(const text of [r.values[3]?new Date(r.values[3]*1000).toLocaleString():'Unknown',r.values[1],r.values[4].toFixed(2)+' °C',r.values[5].toFixed(1)+' %',r.dataset]){const td=document.createElement('td');td.textContent=text;tr.append(td);}$('readings').append(tr);}
 $('tableInfo').textContent=`Showing the most recent ${Math.min(100,selected.length)} of ${selected.length.toLocaleString()} selected readings. Export includes all selected readings.`;$('export').disabled=!selected.length;
}
$('export').onclick=()=>{const content='dataset,record_id,node_id,sequence,unix_seconds,temperature_c,humidity_percent\n'+selected.map(r=>[r.dataset,...r.values].join(',')).join('\n')+'\n',url=URL.createObjectURL(new Blob([content],{type:'text/csv'})),a=document.createElement('a');a.href=url;a.download='sensor-history.csv';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
for(const id of ['from','to','sensor','dataset','metric'])$(id).onchange=draw;
$('all').onclick=()=>{$('from').value='';$('to').value='';draw();};$('reload').onclick=reload;window.onresize=draw;reload();
