// Litter Tracker - PIR INSIDE + MAINTENANCE MODE (plugged-in version)
// Placement: PIR INSIDE the box, not outside. Only sees cat inside + human hands during cleanup.
// Maintenance mode pauses logging during cleanup, so human is never counted as a visit.
// Trigger maintenance: 1) Physical button, 2) Dashboard button -> server -> ESP32 polls
// Auto-exit after 10 min so you can't forget it on.

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* SERVER_URL = "http://192.168.1.XX:8000";
const char* DEVICE_ID = "litter-01";

#define PIR_PIN 27
#define BTN_PIN 33   // maintenance button -> GND, use internal pullup. Press = toggle maintenance
#define LED_PIN 2    // built-in LED: OFF=idle, SOLID=cat inside, BLINKING=maintenance

// Tunables - PIR INSIDE logic (not entrance pair logic)
int MIN_VISIT_SEC = 8;
int END_AFTER_NO_MOTION_SEC = 30; // cat sits still -> PIR goes LOW, wait this long before closing
int MAX_VISIT_SEC = 900;
int MAINTENANCE_MINUTES = 10;
int DEBOUNCE_MS = 300;

bool inVisit = false;
unsigned long visitStartMs = 0;
String visitStartISO = "";
unsigned long lastMotionMs = 0;
bool maintenance = false;
unsigned long maintenanceUntilMs = 0;
unsigned long lastBtnMs = 0;
bool lastBtnState = HIGH;
unsigned long lastServerCheck = 0;
unsigned long lastLedBlink = 0;
bool ledState = false;

String isoNow() {
  struct tm t; if(!getLocalTime(&t)) return "";
  char buf[30]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

bool postVisit(String startISO, String endISO, float durSec) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.begin(String(SERVER_URL) + "/api/visit");
  http.addHeader("Content-Type","application/json");
  StaticJsonDocument<256> doc;
  doc["device_id"]=DEVICE_ID;
  doc["start_ts"]=startISO;
  doc["end_ts"]=endISO;
  doc["duration_sec"]=durSec;
  String body; serializeJson(doc, body);
  int code = http.POST(body);
  Serial.printf("POST visit %d dur=%.0fs\n", code, durSec);
  http.end();
  return code>=200 && code<300;
}

bool checkServerMaintenance() {
  // Dashboard can also toggle maintenance: GET /api/maintenance
  if (WiFi.status() != WL_CONNECTED) return maintenance;
  HTTPClient http; http.begin(String(SERVER_URL) + "/api/maintenance");
  int code = http.GET();
  if (code==200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    deserializeJson(doc, payload);
    bool serverMaint = doc["active"] | false;
    if (serverMaint && !maintenance) {
      Serial.println("Maintenance ON via dashboard");
      maintenance = true;
      maintenanceUntilMs = millis() + (unsigned long)MAINTENANCE_MINUTES*60*1000;
      if (inVisit) { Serial.println("Cancelling in-progress visit - was maintenance"); inVisit=false; }
    }
    if (!serverMaint && maintenance && millis() > maintenanceUntilMs) {
      // already expired, will clear below
    }
  }
  http.end();
  return maintenance;
}

void setMaintenance(bool on, const char* why) {
  maintenance = on;
  if (on) {
    maintenanceUntilMs = millis() + (unsigned long)MAINTENANCE_MINUTES*60*1000;
    if (inVisit) { Serial.println("Maintenance started - discarding in-progress visit"); inVisit=false; }
    Serial.printf("MAINTENANCE ON (%s) for %d min - logging paused\n", why, MAINTENANCE_MINUTES);
    // Tell server
    if (WiFi.status()==WL_CONNECTED) {
      HTTPClient http; http.begin(String(SERVER_URL) + "/api/maintenance");
      http.addHeader("Content-Type","application/json");
      StaticJsonDocument<128> doc; doc["active"]=true; doc["minutes"]=MAINTENANCE_MINUTES; doc["source"]="esp32_button";
      String b; serializeJson(doc,b); http.POST(b); http.end();
    }
  } else {
    Serial.println("MAINTENANCE OFF - logging resumed");
    if (WiFi.status()==WL_CONNECTED) {
      HTTPClient http; http.begin(String(SERVER_URL) + "/api/maintenance");
      http.addHeader("Content-Type","application/json");
      StaticJsonDocument<128> doc; doc["active"]=false;
      String b; serializeJson(doc,b); http.POST(b); http.end();
    }
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");
  configTime(-21600, 3600, "pool.ntp.org");
  Serial.println("PIR warming up 30s - stay out of box");
  delay(30000);
  Serial.println("Ready - PIR INSIDE, IDLE");
}

void loop(){
  unsigned long now = millis();

  // --- Button: toggle maintenance ---
  bool btn = digitalRead(BTN_PIN);
  if (btn==LOW && lastBtnState==HIGH && now-lastBtnMs > 500) {
    lastBtnMs = now;
    setMaintenance(!maintenance, "button");
    delay(300); // debounce, avoid double-toggle
  }
  lastBtnState = btn;

  // --- Auto-exit maintenance after timeout ---
  if (maintenance && now > maintenanceUntilMs) {
    setMaintenance(false, "timeout");
  }

  // --- Poll server for dashboard maintenance every 5s ---
  if (now - lastServerCheck > 5000) {
    lastServerCheck = now;
    checkServerMaintenance();
  }

  // --- LED ---
  if (maintenance) {
    if (now-lastLedBlink>400){ ledState=!ledState; digitalWrite(LED_PIN, ledState); lastLedBlink=now; }
  } else if (inVisit) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  // --- PIR logic, paused during maintenance ---
  if (maintenance) {
    // Ignore all PIR while cleaning. Do not start, do not extend.
    delay(100);
    return;
  }

  int pir = digitalRead(PIR_PIN);
  if (pir==HIGH) {
    lastMotionMs = now;
    if (!inVisit) {
      inVisit = true;
      visitStartMs = now;
      visitStartISO = isoNow();
      Serial.println("VISIT START - motion INSIDE box");
    }
  }

  if (inVisit) {
    unsigned long noMotionFor = now - lastMotionMs;
    unsigned long dur = (now - visitStartMs)/1000;
    if (noMotionFor > (unsigned long)END_AFTER_NO_MOTION_SEC*1000) {
      Serial.printf("VISIT END dur=%lus no-motion %lus\n", dur, noMotionFor/1000);
      if (dur >= MIN_VISIT_SEC && dur <= MAX_VISIT_SEC) postVisit(visitStartISO, isoNow(), dur);
      else Serial.println("Ignored - too short/long");
      inVisit = false;
    }
    if (dur > MAX_VISIT_SEC) { postVisit(visitStartISO, isoNow(), MAX_VISIT_SEC); inVisit=false; }
  }
  delay(100);
}
