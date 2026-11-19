#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include "../config.h"
#include <ESPmDNS.h>
#include "dashboard_page.h"

// Base station must have a filesystem partition large enough for the desired history.
// Log CSV: global_id,node_id,sequence,unix_seconds,temp_c,rh_percent
WebServer server(80);
Preferences prefs;
uint32_t nextId = 1;
uint32_t highSeq[256] = {0};
const char *LOGFILE = "/measurements.csv";
String datasetId;
const size_t MAX_LABEL_BYTES = 48;

// Fixed RAM budget: sparse file offsets plus a small pagination continuation cache.
struct HistoryPoint { uint32_t id, offset; };
HistoryPoint historyIndex[128], continuations[8];
size_t indexCount=0, continuationSlot=0;
uint32_t indexedRecords=0, indexStride=1;
struct LatestReading { uint32_t id, seq, ts; float temp, rh; };
LatestReading latestReadings[256] = {};

// Buffered, bounded reads avoid one filesystem operation per character and
// Stream's timeout on an incomplete final line. Oversized/torn lines are skipped.
class LogReader {
  File &file;
  uint8_t buffer[512];
  size_t used=0, availableBytes=0;
  uint32_t offset;
public:
  LogReader(File &f):file(f),offset(f.position()){}
  uint32_t position() const { return offset; }
  bool line(char *out, size_t capacity) {
    size_t length=0; bool overflow=false;
    for (;;) {
      if(used==availableBytes){availableBytes=file.read(buffer,sizeof(buffer));used=0;if(!availableBytes)return false;}
      char c=buffer[used++];offset++;
      if(c=='\n'){out[overflow?0:length]=0;return true;}
      if(length+1<capacity)out[length++]=c;else overflow=true;
    }
  }
};
bool parseRecord(const char *line, unsigned long &id, unsigned &node,
                 unsigned long &seq, unsigned long &ts, float &t, float &h) {
  return sscanf(line,"%lu,%u,%lu,%lu,%f,%f",&id,&node,&seq,&ts,&t,&h)==6 &&
    id>0 && node>0 && node<256 && isfinite(t) && isfinite(h);
}
void indexRecord(uint32_t id, uint32_t offset) {
  if(indexedRecords % indexStride == 0){
    if(indexCount==128){
      for(size_t i=0;i<64;i++)historyIndex[i]=historyIndex[i*2];
      indexCount=64;indexStride*=2;
    }
    if(indexedRecords % indexStride == 0)historyIndex[indexCount++]={id,offset};
  }
  indexedRecords++;
}
void rebuildHistoryIndex() {
  indexCount=0;indexedRecords=0;indexStride=1;
  memset(continuations,0,sizeof(continuations));
  File f=LittleFS.open(LOGFILE,"r");if(!f)return;
  LogReader reader(f);char line[128];
  for(;;){uint32_t offset=reader.position();if(!reader.line(line,sizeof(line)))break;
    unsigned long id,seq,ts;unsigned node;float t,h;
    if(parseRecord(line,id,node,seq,ts,t,h))indexRecord(id,offset);
  }
}

String newDatasetId() {
  char s[25];
  snprintf(s, sizeof(s), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  return String(s);
}

void loadDatasetId() {
  datasetId = prefs.getString("dataset", "");
  if (!datasetId.length()) {
    datasetId = newDatasetId();
    prefs.putString("dataset", datasetId);
  }
}

void labelKey(unsigned node, char *key, size_t capacity) {
  snprintf(key, capacity, "label%u", node);
}

String jsonString(const String &value) {
  String escaped = "\"";
  escaped.reserve(value.length() + 8);
  for (size_t i=0; i<value.length(); i++) {
    uint8_t c=value[i];
    if(c=='\"' || c=='\\'){escaped+='\\';escaped+=(char)c;}
    else if(c=='\n')escaped+="\\n";
    else if(c=='\r')escaped+="\\r";
    else if(c=='\t')escaped+="\\t";
    else escaped+=(char)c;
  }
  escaped+='\"';
  return escaped;
}

// Reset sensor readings only; firmware, shared Wi-Fi configuration and
// dashboard remain. Do not call LittleFS.format().
bool resetReadings() {
  // Remove any pending/backup compaction files as well as the live log.
  if (LittleFS.exists(LOGFILE) && !LittleFS.remove(LOGFILE)) return false;
  if (LittleFS.exists("/compact.tmp") && !LittleFS.remove("/compact.tmp")) return false;
  if (LittleFS.exists("/compact.bak") && !LittleFS.remove("/compact.bak")) return false;

  // Labels are configuration, not readings, and deliberately survive reset.
  for (int n=1; n<256; n++) {
    char key[12]; snprintf(key,sizeof(key),"seq%d",n);
    if (prefs.isKey(key) && !prefs.remove(key)) return false;
  }
  if (prefs.isKey("nextid") && !prefs.remove("nextid")) return false;
  memset(highSeq, 0, sizeof(highSeq));
  memset(latestReadings,0,sizeof(latestReadings));
  indexCount=0;indexedRecords=0;indexStride=1;memset(continuations,0,sizeof(continuations));
  nextId = 1;
  datasetId = newDatasetId();
  return prefs.putString("dataset", datasetId) == datasetId.length();
}


bool epochValid() { return time(nullptr) > 1700000000; }
void respond(int code, const String &s) { server.send(code, "text/plain", s); }
void cors() { server.sendHeader("Access-Control-Allow-Origin", "*"); server.sendHeader("Access-Control-Allow-Headers", "Content-Type"); server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS"); }
void options() { cors(); server.send(204); }

void recoverLog() {
  File f = LittleFS.open(LOGFILE, "r");
  if(f){
    LogReader reader(f);char line[128];
    for(;;){
      uint32_t offset=reader.position();if(!reader.line(line,sizeof(line)))break;
      unsigned long id,seq,ts;unsigned node;float t,h;
      if(parseRecord(line,id,node,seq,ts,t,h)){
        indexRecord(id,offset);
        if(id>=nextId)nextId=id+1;
        if(seq>highSeq[node])highSeq[node]=seq;
        if(id>latestReadings[node].id)latestReadings[node]={(uint32_t)id,(uint32_t)seq,(uint32_t)ts,t,h};
      }
    }
  }
  if (f) f.close();
  // Preserve deduplication across archive compaction.
  for (int n=1; n<256; n++) {
    char key[12]; snprintf(key,sizeof(key),"seq%d",n);
    uint32_t stored = prefs.getUInt(key,0);
    if (stored > highSeq[n]) highSeq[n] = stored;
  }
  uint32_t storedId = prefs.getUInt("nextid",1);
  if (storedId > nextId) nextId = storedId;
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(false)) { Serial.println("LittleFS mount error"); while (true) delay(1000); }
  prefs.begin("base", false);
  if (!LittleFS.exists(LOGFILE) && LittleFS.exists("/compact.bak"))
    LittleFS.rename("/compact.bak", LOGFILE);
  if (LittleFS.exists("/compact.tmp")) LittleFS.remove("/compact.tmp");
  recoverLog();
  loadDatasetId();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.printf("\nBase address: http://%s\n", WiFi.localIP().toString().c_str());
  if (MDNS.begin(BASE_HOST)) {
    Serial.printf("mDNS available: http://%s.local\n", BASE_HOST);
  } else {
    Serial.println("mDNS initialization FAILED");
  }
  configTime(0,0,"pool.ntp.org","time.google.com");

  server.on("/api/meta", HTTP_GET, []() {
    cors();
    server.send(200, "application/json", "{\"dataset\":\"" + datasetId + "\",\"archive_protocol\":2,\"latest_id\":" + String(nextId-1) + ",\"retained_after\":" + String(indexCount?historyIndex[0].id-1:nextId-1) + "}");
  });

  // Destructive operation. Require the exact confirmation phrase in body.
  // Intended for use on a trusted LAN only: this is not authentication.
  server.on("/api/reset", HTTP_POST, []() {
    cors();
    if (server.arg("plain") != "RESET ALL READINGS") {
      respond(400, "Confirmation phrase required");
      return;
    }
    if (!resetReadings()) {
      respond(500, "Reset failed; check serial diagnostics");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"dataset\":\"" + datasetId + "\"}");
  });

  server.on("/api/time", HTTP_GET, []() {
    cors();
    if (!epochValid()) { respond(503,"clock not synchronised"); return; }
    server.send(200,"text/plain",String((uint32_t)time(nullptr)));
  });

  // Labels live in NVS on the base and are independent of retained readings.
  server.on("/api/labels", HTTP_GET, []() {
    cors();server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"application/json","");server.sendContent("{\"labels\":[");
    bool comma=false;
    for(unsigned node=1;node<256;node++){
      char key[12];labelKey(node,key,sizeof(key));
      String label=prefs.getString(key,"");if(!label.length())continue;
      server.sendContent(String(comma?",":"")+"["+String(node)+","+jsonString(label)+"]");comma=true;
    }
    server.sendContent("]}");server.sendContent("");
  });

  server.on("/api/labels", HTTP_POST, []() {
    cors();
    char *end=nullptr;unsigned long parsed=strtoul(server.arg("node").c_str(),&end,10);
    if(!end || *end || parsed<1 || parsed>255){respond(400,"invalid sensor id");return;}
    String label=server.arg("plain");label.trim();
    if(label.length()>MAX_LABEL_BYTES){respond(400,"label must be at most 48 bytes");return;}
    for(size_t i=0;i<label.length();i++)if((uint8_t)label[i]<32 || (uint8_t)label[i]==127){respond(400,"label contains control characters");return;}
    char key[12];labelKey((unsigned)parsed,key,sizeof(key));
    bool ok=label.length()?prefs.putString(key,label)==label.length():(!prefs.isKey(key)||prefs.remove(key));
    if(!ok){respond(500,"label could not be stored");return;}
    server.send(200,"application/json","{\"ok\":true,\"node\":"+String(parsed)+",\"label\":"+jsonString(label)+"}");
  });

  // Sensor POST format: one CSV line per reading: node,seq,unix_seconds,temp,humidity
  // One sensor per upload; ACK contains highest durable accepted sequence.
  server.on("/api/readings", HTTP_POST, []() {
    cors();
    String body=server.arg("plain");
    if (!body.length() || body.length()>20000) { respond(413,"batch too large or empty"); return; }
    File f=LittleFS.open(LOGFILE,"a");
    if (!f) { respond(507,"storage unavailable"); return; }
    size_t pos=0; uint32_t ack=0; int nodeExpected=-1; bool ok=true;
    while (pos<body.length()) {
      int e=body.indexOf('\n',pos); if(e<0)e=body.length();
      String line=body.substring(pos,e); line.trim(); pos=e+1;
      if (!line.length())continue;
      unsigned node; unsigned long seq,ts; float t,h; char tail;
      if (sscanf(line.c_str(),"%u,%lu,%lu,%f,%f%c",&node,&seq,&ts,&t,&h,&tail)!=5 || node==0 || node>255 || seq==0 || !isfinite(t) || !isfinite(h)) {ok=false;break;}
      if(nodeExpected<0)nodeExpected=node;
      if(nodeExpected!=(int)node){ok=false;break;}
      if (seq>highSeq[node]) {
        String record=String(nextId)+","+String(node)+","+String(seq)+","+String(ts)+","+String(t,2)+","+String(h,2)+"\n";
        uint32_t recordOffset=f.position();
        if(f.print(record)!=record.length()){ok=false;break;}
        f.flush();
        // Monotonic IDs and dedup are persisted before acknowledging.
        char key[12];snprintf(key,sizeof(key),"seq%d",node);
        if(prefs.putUInt(key,seq)!=4 || prefs.putUInt("nextid",nextId+1)!=4){ok=false;break;}
        indexRecord(nextId,recordOffset);
        latestReadings[node]={nextId,(uint32_t)seq,(uint32_t)ts,t,h};
        highSeq[node]=seq; nextId++;
      }
      ack=seq;
    }
    f.close();
    if(!ok || !ack){respond(500,"failed; retry batch");return;}
    respond(200,"ACK "+String(ack));
  });

  // JSON: {"latest_id":123,"rows":[[id,node,seq,ts,temp,rh],...]}
  // Reader fetches incrementally; 'after' is an exclusive global ID.
  server.on("/api/history",HTTP_GET,[](){
    cors();
    uint32_t after=(uint32_t)strtoul(server.arg("after").c_str(),nullptr,10);
    File f=LittleFS.open(LOGFILE,"r");
    String result="{\"dataset\":\""+datasetId+"\",\"latest_id\":"+String(nextId-1)+",\"rows\":[";
    result.reserve(12500);
    int count=0;
    if(f && after<nextId-1) {
      uint32_t offset=0;
      for(size_t i=0;i<indexCount;i++)if(historyIndex[i].id<=after)offset=historyIndex[i].offset;
      for(const auto &point:continuations)if(point.id==after && point.offset>offset)offset=point.offset;
      if(!f.seek(offset)){f.close();respond(500,"history seek failed");return;}
      LogReader reader(f);char line[128];uint32_t last=after;
      while(reader.line(line,sizeof(line))){
        unsigned long id,seq,ts;unsigned node;float t,h;
        if(!parseRecord(line,id,node,seq,ts,t,h)||id<=after)continue;
        if(count)result+=",";
        result+="[";result+=line;result+="]";last=id;
        if(++count>=100 || result.length()>12000)break;
      }
      continuations[continuationSlot++ % 8]={last,reader.position()};
      f.close();
    }
    result+="]}";
    server.send(200,"application/json",result);
  });

  // Cached at boot and on uploads; bounded by the protocol's 255 sensor IDs.
  server.on("/api/latest",HTTP_GET,[](){
    cors();server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"application/json","");server.sendContent("{\"dataset\":\""+datasetId+"\",\"rows\":[");
    bool comma=false;
    for(unsigned node=1;node<256;node++){
      const auto &r=latestReadings[node];if(!r.id)continue;
      char row[256];
      snprintf(row,sizeof(row),"%s[%lu,%u,%lu,%lu,%.2f,%.2f]",comma?",":"",
        (unsigned long)r.id,node,(unsigned long)r.seq,(unsigned long)r.ts,r.temp,r.rh);
      server.sendContent(row);comma=true;
    }
    server.sendContent("]}");server.sendContent("");
  });

  // Called ONLY after a verified archive-file write. Dataset guards against reset races.
  // Deletes archived prefix by rewriting data > upto.
  server.on("/api/archive/ack",HTTP_POST,[](){
    cors();
    if(server.arg("dataset") != datasetId) {respond(409,"dataset changed or missing; fetch again");return;}
    uint32_t upto=(uint32_t)strtoul(server.arg("plain").c_str(),nullptr,10);
    if(!upto || upto>=nextId) {respond(400,"invalid archive id");return;}
    File in=LittleFS.open(LOGFILE,"r");
    if(!in){respond(200,"OK");return;}
    File out=LittleFS.open("/compact.tmp","w");
    if(!out){in.close();respond(507,"no room for compaction");return;}
    while(in.available()){
      String line=in.readStringUntil('\n');
      uint32_t id=(uint32_t)strtoul(line.c_str(),nullptr,10);
      if(id>upto && line.length() && !out.println(line)) { out.close(); in.close(); LittleFS.remove("/compact.tmp"); respond(507,"compaction write failed"); return; }
    }
    out.flush();out.close();in.close();
    // Retain a fallback copy until the replacement is installed.
    LittleFS.remove("/compact.bak");
    if (!LittleFS.rename(LOGFILE,"/compact.bak")) { respond(500,"backup rename failed"); return; }
    if (!LittleFS.rename("/compact.tmp",LOGFILE)) { LittleFS.rename("/compact.bak",LOGFILE); respond(500,"compaction rename failed"); return; }
    LittleFS.remove("/compact.bak");
    rebuildHistoryIndex();
    for(unsigned node=1;node<256;node++)if(latestReadings[node].id<=upto)latestReadings[node]={};
    respond(200,"OK");
  });
  server.on("/api/meta",HTTP_OPTIONS,options);
  server.on("/api/reset",HTTP_OPTIONS,options);
  server.on("/api/time",HTTP_OPTIONS,options);
  server.on("/api/readings",HTTP_OPTIONS,options);
  server.on("/api/history",HTTP_OPTIONS,options);
  server.on("/api/latest",HTTP_OPTIONS,options);
  server.on("/api/labels",HTTP_OPTIONS,options);
  server.on("/api/archive/ack",HTTP_OPTIONS,options);
  server.on("/",HTTP_GET,[](){server.send_P(200,"text/html; charset=utf-8",DASHBOARD_PAGE);});
  server.on("/dashboard.html",HTTP_GET,[](){server.send_P(200,"text/html; charset=utf-8",DASHBOARD_PAGE);});
  server.begin();
  MDNS.addService("http", "tcp", BASE_PORT);
}
void loop(){server.handleClient();}
