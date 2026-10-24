#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Adafruit_SHT4x.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include "../config.h"
#include <ESPmDNS.h>

#define DEVICE_ID 3
#define SDA_PIN 4
#define SCL_PIN 5
#define DEBUG_SERIAL false
#define WIFI_TIMEOUT_MS 7000
#define MAX_BATCH 50
const char *QUEUE_FILE="/pending.csv";
IPAddress baseAddress;
String baseUrl;

bool resolveBase() {
  // mDNS querying needs an active Wi-Fi STA connection.
  String mdnsName = String("sensor-") + String(DEVICE_ID);
  if (MDNS.begin(mdnsName.c_str())) {
    baseAddress = MDNS.queryHost(BASE_HOST, 2500);
    MDNS.end();
  }
  if (baseAddress == IPAddress(0,0,0,0) && strlen(BASE_FALLBACK_IP)) {
    baseAddress.fromString(BASE_FALLBACK_IP);
  }
  if (baseAddress == IPAddress(0,0,0,0)) return false;
  baseUrl = String("http://") + baseAddress.toString() + ":" + String(BASE_PORT);
  return true;
}

#if DEBUG_SERIAL
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) do{}while(0)
#endif

Adafruit_SHT4x sht;
Preferences prefs;
RTC_DATA_ATTR uint32_t clockMagic=0;
RTC_DATA_ATTR uint32_t lastSyncedEpoch=0;

uint32_t getEpoch(){ time_t now=time(nullptr); return now>1700000000?(uint32_t)now:0; }
void sleepUntilNext(){
  uint32_t awake=millis();
  uint32_t target=SAMPLE_SECONDS*1000UL;
  uint32_t ms=awake<target?target-awake:100;
  DBG("awake %lu; sleeping %lu ms\n",(unsigned long)awake,(unsigned long)ms);
  #if DEBUG_SERIAL
  Serial.flush();
  #endif
  WiFi.disconnect(true);WiFi.mode(WIFI_OFF);Wire.end();
  esp_sleep_enable_timer_wakeup((uint64_t)ms*1000ULL);
  esp_deep_sleep_start();
}

// Each measurement is appended to a persistent queue BEFORE networking.
// Format: sequence,unix_seconds,temp_c,rh_percent
bool recordMeasurement(uint32_t seq, uint32_t ts, float temp, float hum){
  File f=LittleFS.open(QUEUE_FILE,"a"); if(!f)return false;
  String row=String(seq)+","+String(ts)+","+String(temp,2)+","+String(hum,2)+"\n";
  bool ok=f.print(row)==row.length();f.flush();f.close();return ok;
}

// Delete only records covered by an acknowledged upload.
bool discardThrough(uint32_t ack){
  File in=LittleFS.open(QUEUE_FILE,"r");if(!in)return false;
  File out=LittleFS.open("/queue.tmp","w");if(!out){in.close();return false;}
  bool ok=true;
  while(in.available()){
    String row=in.readStringUntil('\n'); if(!row.length())continue;
    uint32_t seq=(uint32_t)strtoul(row.c_str(),nullptr,10);
    if(seq>ack && !out.println(row)){ok=false;break;}
  }
  out.flush();out.close();in.close();
  if(!ok){LittleFS.remove("/queue.tmp");return false;}
  LittleFS.remove("/queue.bak");
  if(!LittleFS.rename(QUEUE_FILE,"/queue.bak"))return false;
  if(!LittleFS.rename("/queue.tmp",QUEUE_FILE)){
    LittleFS.rename("/queue.bak",QUEUE_FILE);return false;
  }
  LittleFS.remove("/queue.bak");return true;
}

bool fetchTime(){
  HTTPClient http;
  http.setTimeout(2500);
  http.begin(baseUrl+"/api/time");
  int code=http.GET();
  String text=code==200?http.getString():"";
  http.end();
  uint32_t ts=(uint32_t)strtoul(text.c_str(),nullptr,10);
  if(code!=200||ts<1700000000)return false;
  struct timeval tv={};tv.tv_sec=ts;tv.tv_usec=0;
  settimeofday(&tv,nullptr);
  clockMagic=0x1234ABCD;lastSyncedEpoch=ts;
  return true;
}

void uploadQueue(uint32_t currentSeq, uint32_t sampleMillis){
  // The base can backfill records captured with no valid clock using the
  // known sampling period and an approximate freshly synchronised time.
  for(int batch=0;batch<200;batch++){
    File f=LittleFS.open(QUEUE_FILE,"r");if(!f)return;
    String payload;int count=0;uint32_t topSeq=0;
    uint32_t reference=getEpoch();
    while(f.available() && count<MAX_BATCH){
      String line=f.readStringUntil('\n');if(!line.length())continue;
      unsigned long seq,ts;float t,h;
      if(sscanf(line.c_str(),"%lu,%lu,%f,%f",&seq,&ts,&t,&h)!=4)continue;
      uint32_t timestamp=(uint32_t)ts;
      if(!timestamp && reference && seq<=currentSeq){
        uint64_t ago=(uint64_t)(currentSeq-seq)*SAMPLE_SECONDS;
        timestamp=reference>ago?(uint32_t)(reference-ago):0;
      }
      payload+=String(DEVICE_ID)+","+String(seq)+","+String(timestamp)+","+String(t,2)+","+String(h,2)+"\n";
      topSeq=seq;count++;
    }
    f.close();if(!count)return;
    HTTPClient http;http.setTimeout(5000);
    http.begin(baseUrl+"/api/readings");
    http.addHeader("Content-Type","text/plain");
    int code=http.POST(payload);
    String response=code==200?http.getString():"";
    http.end();
    if(code!=200||!response.startsWith("ACK "))return;
    uint32_t ack=(uint32_t)strtoul(response.substring(4).c_str(),nullptr,10);
    if(!ack||ack>topSeq)return;
    if(!discardThrough(ack))return;
    DBG("acknowledged through %lu\n",(unsigned long)ack);
  }
}

void setup(){
  #if DEBUG_SERIAL
  Serial.begin(115200);delay(1500);
  #endif
  // Mount existing filesystem first, preserving stored readings when healthy.
  bool fsReady = LittleFS.begin(false);
  if (!fsReady) {
    DBG("LittleFS mount failed. Attempting format/recovery...\n");
    // WARNING: This can erase all unsent measurements if the filesystem
    // was corrupted, rather than merely uninitialised.
    fsReady = LittleFS.begin(true);
    if (fsReady) {
      DBG("LittleFS formatted and mounted successfully.\n");
    }
  }
  if (!fsReady) {
    DBG("ERROR: LittleFS recovery failed; sleeping.\n");
    sleepUntilNext();
  }
  DBG("LittleFS ready: total=%u used=%u bytes\n",
      (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
  // Recover an interrupted queue compaction after power loss.
  if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists("/queue.bak")) {
    LittleFS.rename("/queue.bak", QUEUE_FILE);
  }
  if (LittleFS.exists(QUEUE_FILE) && LittleFS.exists("/queue.tmp")) {
    LittleFS.remove("/queue.tmp");
  }
  prefs.begin("sensor",false);
  // Permanent sequence ID avoids duplicates when a battery is replaced.
  uint32_t seq=prefs.getUInt("sequence",0)+1;
  // Persist a new sequence before logging this measurement.
  if(prefs.putUInt("sequence",seq)!=4)sleepUntilNext();
  Wire.begin(SDA_PIN,SCL_PIN);
  if(!sht.begin(&Wire))sleepUntilNext();
  sht.setHeater(SHT4X_NO_HEATER);
  sht.setPrecision(SHT4X_HIGH_PRECISION);
  sensors_event_t humidity,temp;
  if(!sht.getEvent(&humidity,&temp))sleepUntilNext();
  uint32_t sampleMillis=millis();
  uint32_t timestamp=getEpoch();
  if(!recordMeasurement(seq,timestamp,temp.temperature,humidity.relative_humidity))sleepUntilNext();
  DBG("Node %d seq %lu t %.2f h %.1f time %lu\n",DEVICE_ID,(unsigned long)seq,temp.temperature,humidity.relative_humidity,(unsigned long)timestamp);
  WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  uint32_t began=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-began<WIFI_TIMEOUT_MS)delay(40);
  if(WiFi.status()==WL_CONNECTED){
    if (resolveBase()) {
      fetchTime();
      uploadQueue(seq,sampleMillis);
    } else { DBG("Base mDNS resolution failed; queued readings retained\n"); }
  }
  sleepUntilNext();
}
void loop(){}
