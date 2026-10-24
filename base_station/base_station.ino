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

// Reset sensor readings only; firmware, shared Wi-Fi configuration and
// dashboard remain. Do not call LittleFS.format().
bool resetReadings() {
  // Remove any pending/backup compaction files as well as the live log.
  if (LittleFS.exists(LOGFILE) && !LittleFS.remove(LOGFILE)) return false;
  if (LittleFS.exists("/compact.tmp") && !LittleFS.remove("/compact.tmp")) return false;
  if (LittleFS.exists("/compact.bak") && !LittleFS.remove("/compact.bak")) return false;

  // A failed metadata reset must not be reported as success.
  if (!prefs.clear()) return false;
  memset(highSeq, 0, sizeof(highSeq));
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
  if (f) while (f.available()) {
    String line = f.readStringUntil('\n');
    unsigned long id, seq, ts; unsigned node; float t, h;
    if (sscanf(line.c_str(), "%lu,%u,%lu,%lu,%f,%f", &id, &node, &seq, &ts, &t, &h) == 6 && node < 256) {
      if (id >= nextId) nextId = id + 1;
      if (seq > highSeq[node]) highSeq[node] = seq;
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
    server.send(200, "application/json", "{\"dataset\":\"" + datasetId + "\"}");
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
        if(f.print(record)!=record.length()){ok=false;break;}
        f.flush();
        // Monotonic IDs and dedup are persisted before acknowledging.
        char key[12];snprintf(key,sizeof(key),"seq%d",node);
        if(prefs.putUInt(key,seq)!=4 || prefs.putUInt("nextid",nextId+1)!=4){ok=false;break;}
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
    String result="{\"latest_id\":"+String(nextId-1)+",\"rows\":[";
    int count=0;
    if(f) {
      while(f.available()){
        String line=f.readStringUntil('\n');
        uint32_t id=(uint32_t)strtoul(line.c_str(),nullptr,10);
        if(id<=after)continue;
        line.trim(); if(!line.length())continue;
        if(count)result+=",";
        result+="[";line.replace(",",",");result+=line+"]";
        if(++count>=100 || result.length()>12000)break;
      }
      f.close();
    }
    result+="]}";
    server.send(200,"application/json",result);
  });

  // Called ONLY after successful archive-file write by the dashboard.
  // Deletes archived prefix by rewriting data > upto.
  server.on("/api/archive/ack",HTTP_POST,[](){
    cors();
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
    respond(200,"OK");
  });
  server.on("/api/meta",HTTP_OPTIONS,options);
  server.on("/api/reset",HTTP_OPTIONS,options);
  server.on("/api/time",HTTP_OPTIONS,options);
  server.on("/api/readings",HTTP_OPTIONS,options);
  server.on("/api/history",HTTP_OPTIONS,options);
  server.on("/api/archive/ack",HTTP_OPTIONS,options);
  server.on("/",HTTP_GET,[](){server.send_P(200,"text/html; charset=utf-8",DASHBOARD_PAGE);});
  server.on("/dashboard.html",HTTP_GET,[](){server.send_P(200,"text/html; charset=utf-8",DASHBOARD_PAGE);});
  server.begin();
  MDNS.addService("http", "tcp", BASE_PORT);
}
void loop(){server.handleClient();}
