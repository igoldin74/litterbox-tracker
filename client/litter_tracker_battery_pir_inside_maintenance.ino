// Litter Tracker - BATTERY + PIR INSIDE + MAINTENANCE MODE
// PIR INSIDE: only cat + cleanup hands trigger it. Outside motion is ignored by placement.
// Maintenance: press button to pause logging for 10 min during cleanup.
// Battery: ESP32 sleeps, wakes on PIR OR button.
//  - Button wake: toggle maintenance, save to RTC, go back to sleep
//  - PIR wake while maintenance ON: ignore, go back to sleep immediately
//  - PIR wake while maintenance OFF: stay awake until visit ends (30s no motion), POST once, sleep
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_sleep.h"

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* SERVER_URL = "http://192.168.1.XX:8000";
const char* DEVICE_ID = "litter-01";

#define PIR_PIN 27
#define BTN_PIN 33
#define LED_PIN 2
#define BAT_PIN 34

RTC_DATA_ATTR bool maintenance = false;
RTC_DATA_ATTR unsigned long maintenanceUntilEpoch = 0; // unix time, survives sleep
RTC_DATA_ATTR bool waitingForExit = false; // not used in inside mode, kept for clarity

int MIN_VISIT_SEC = 8;
int END_AFTER_NO_MOTION_SEC = 30;
int MAINTENANCE_MINUTES = 10;

String isoNow() {
  struct tm t; if(!getLocalTime(&t)) return "";
  char buf[30]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}
unsigned long epochNow() {
  struct tm t; if(!getLocalTime(&t)) return 0;
  return mktime(&t);
}
float readBattery() {
  int raw = analogRead(BAT_PIN);
  return raw / 4095.0 * 3.3 * 2.0;
}
bool postVisit(String startISO, String endISO, float durSec) {
  HTTPClient http; http.begin(String(SERVER_URL) + "/api/visit");
  http.addHeader("Content-Type","application/json");
  StaticJsonDocument<256> doc;
  doc["device_id"]=DEVICE_ID;
  doc["start_ts"]=startISO;
  doc["end_ts"]=endISO;
  doc["duration_sec"]=durSec;
  doc["battery_v"]=readBattery();
  String body; serializeJson(doc, body);
  int code = http.POST(body);
  Serial.printf("POST %d dur=%.0f batt=%.2fV\n", code, durSec, readBattery());
  http.end();
  return code>=200 && code<300;
}
void notifyMaintenance(bool on) {
  HTTPClient http; http.begin(String(SERVER_URL) + "/api/maintenance");
  http.addHeader("Content-Type","application/json");
  StaticJsonDocument<128> doc; doc["active"]=on; doc["minutes"]=MAINTENANCE_MINUTES; doc["source"]="esp32_button";
  String b; serializeJson(doc,b); http.POST(b); http.end();
}
void goToSleep() {
  Serial.println("Sleeping, wake on PIR or BTN...");
  Serial.flush();
  // Wake on PIR HIGH OR BTN LOW (button to GND)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, 1);
  esp_sleep_enable_ext1_wakeup(1ULL << BTN_PIN, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}

void setup(){
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause %d maintenance=%d\n", cause, maintenance);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries=0; while(WiFi.status()!=WL_CONNECTED && tries<20){ delay(500); tries++; }
  configTime(-21600, 3600, "pool.ntp.org");
  delay(1000);

  // Check if maintenance expired while sleeping
  if (maintenance && epochNow() > maintenanceUntilEpoch) {
    maintenance = false;
    Serial.println("Maintenance expired - logging resumed");
    notifyMaintenance(false);
  }

  // Button was pressed?
  if (cause == ESP_SLEEP_WAKEUP_EXT1 || digitalRead(BTN_PIN)==LOW) {
    // Toggle maintenance
    maintenance = !maintenance;
    if (maintenance) {
      maintenanceUntilEpoch = epochNow() + MAINTENANCE_MINUTES*60;
      Serial.printf("MAINTENANCE ON for %d min - PIR ignored\n", MAINTENANCE_MINUTES);
      digitalWrite(LED_PIN, HIGH);
      notifyMaintenance(true);
      delay(500); // avoid immediate re-trigger
    } else {
      Serial.println("MAINTENANCE OFF - logging resumed");
      notifyMaintenance(false);
    }
    goToSleep();
  }

  // PIR wake
  if (maintenance) {
    Serial.println("PIR fired during maintenance - IGNORED, back to sleep");
    goToSleep();
  }

  // Normal visit: stay awake until 30s no motion
  Serial.println("VISIT START - motion INSIDE");
  digitalWrite(LED_PIN, HIGH);
  String startISO = isoNow();
  unsigned long visitStart = millis();
  unsigned long lastMotion = millis();
  while (millis() - visitStart < 900000) { // 15 min cap
    if (digitalRead(PIR_PIN)==HIGH) lastMotion = millis();
    // Check button during visit - if pressed, cancel visit as maintenance
    if (digitalRead(BTN_PIN)==LOW) {
      Serial.println("Button pressed during visit - treating as maintenance, discarding visit");
      maintenance = true;
      maintenanceUntilEpoch = epochNow() + MAINTENANCE_MINUTES*60;
      notifyMaintenance(true);
      goToSleep();
    }
    if (millis() - lastMotion > (unsigned long)END_AFTER_NO_MOTION_SEC*1000) break;
    delay(200);
  }
  float dur = (millis()-visitStart)/1000.0;
  digitalWrite(LED_PIN, LOW);
  Serial.printf("VISIT END dur=%.0fs\n", dur);
  if (dur >= MIN_VISIT_SEC) postVisit(startISO, isoNow(), dur);
  goToSleep();
}
void loop(){}
