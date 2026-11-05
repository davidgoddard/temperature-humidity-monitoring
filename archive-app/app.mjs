import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { archive,loadArchives } from './archive.mjs';
const here=path.dirname(fileURLToPath(import.meta.url));
const [command='help',...args]=process.argv.slice(2);
const options={base:'http://temp.local',directory:path.join(here,'archives'),port:8080};
try{
 for(let i=0;i<args.length;i+=2){const key={'--base':'base','--dir':'directory','--port':'port'}[args[i]];if(!key||!args[i+1])throw Error('Unknown or incomplete option: '+args[i]);options[key]=args[i+1];}
 options.directory=path.resolve(options.directory);
 if(command==='archive'){const result=await archive(options);console.log(result.message);if(result.filename)console.log(result.filename);}
 else if(command==='serve'){
  const port=Number(options.port);if(!Number.isInteger(port)||port<1||port>65535)throw Error('Invalid port');
  const server=createServer(async(req,res)=>{
   try{
    if(req.method!=='GET'){res.writeHead(405);return res.end('GET only');}
    if(req.url==='/api/archives'){const data=await loadArchives(options.directory);res.writeHead(200,{'Content-Type':'application/json','Cache-Control':'no-store'});return res.end(JSON.stringify(data));}
    if(req.url==='/'||req.url==='/viewer.js'){const name=req.url==='/'?'index.html':'viewer.js';res.writeHead(200,{'Content-Type':name.endsWith('.html')?'text/html; charset=utf-8':'text/javascript; charset=utf-8','Cache-Control':'no-store'});return res.end(await readFile(path.join(here,name)));}
    res.writeHead(404);res.end('Not found');
   }catch(e){res.writeHead(500,{'Content-Type':'application/json'});res.end(JSON.stringify({error:e.message}));}
  });
  server.on('error',e=>{console.error(e.message);process.exitCode=1;});
  server.listen(port,'127.0.0.1',()=>console.log(`Historical dashboard: http://127.0.0.1:${port}\nArchives: ${options.directory}\nPress Ctrl+C to stop.`));
 }else if(command==='help'||command==='--help')console.log('Node 22+\nWeekly archive: node archive-app/app.mjs archive [--base http://temp.local] [--dir /path/to/archives]\nView all archives: node archive-app/app.mjs serve [--dir /path/to/archives] [--port 8080]');
 else throw Error('Unknown command. Use: archive, serve or help');
}catch(e){console.error(e.message);process.exitCode=1;}
