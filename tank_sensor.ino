#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

/* ═════════ CONFIGURATION ═════════ */
const char* ssid     = "IssacMoses";
const char* password = "moses@1234";

const char* supabaseUrl = "https://ulyeoukxyaadfvekdmwc.supabase.co";
const char* anonKey     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVseWVvdWt4eWFhZGZ2ZWtkbXdjIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM5ODk1NTcsImV4cCI6MjA4OTU2NTU1N30.w_37uATvP3SFiWPUvUtzPaGTs4oYQQok1uW7RXe5luE";

/* ═════════ PIN ═════════ */
const int floatSwitchPin = D2;

/* ═════════ SETTINGS ═════════ */
const unsigned long SEND_INTERVAL  = 5000;
const unsigned long RETRY_INTERVAL = 10000;

// 🔥 Stability tuning
const unsigned long STABLE_DELAY = 3000;   // 3 sec stable irukanum
const unsigned long CHANGE_LOCK  = 5000;   // anti flicker

/* ═════════ GLOBAL STATE ═════════ */
unsigned long lastSendTime = 0;
int  waterLevel     = 0;
int  lastSentLevel  = -1;
bool tankPing       = false;

/* Stability variables */
int stableState = HIGH;
int lastReading = HIGH;
unsigned long lastChangeTime = 0;
unsigned long lastAcceptedChange = 0;

/* ═════════ WIFI ═════════ */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Failed. Will retry.");
  }
}

/* ═════════ SENSOR (FIXED LOGIC 🔥) ═════════ */
void readSensor() {
  int reading = digitalRead(floatSwitchPin);

  // detect change
  if (reading != lastReading) {
    lastChangeTime = millis();
    lastReading = reading;
  }

  // check stability
  if ((millis() - lastChangeTime) > STABLE_DELAY) {
    if (stableState != reading) {

      // anti flicker
      if (millis() - lastAcceptedChange < CHANGE_LOCK) return;

      lastAcceptedChange = millis();
      stableState = reading;

      // 🔥 LOGIC FIXED HERE
      if (stableState == LOW) {
        waterLevel = 100;   // FULL ✅ (NC switch)
      } else {
        waterLevel = 0;     // EMPTY
      }

      Serial.printf("[Stable] Water Level = %d%%\n", waterLevel);
    }
  }
}

/* ═════════ SUPABASE ═════════ */
void updateSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(supabaseUrl) + "/rest/v1/motor_system?id=eq.1";
  http.begin(client, url);
  http.setTimeout(5000);
  http.addHeader("apikey",        anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Prefer",        "return=minimal");

  // heartbeat toggle
  tankPing = !tankPing;

  String payload = "{\"water_level\":" + String(waterLevel) +
                   ",\"tank_ping\":" + String(tankPing ? "true" : "false") + "}";

  int code = http.sendRequest("PATCH", payload);

  if (code > 0) {
    Serial.printf("[Tank] Sent: Level=%d%% (HTTP %d)\n", waterLevel, code);
    lastSentLevel = waterLevel;
  } else {
    Serial.printf("[Tank] Send failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

/* ═════════ SETUP ═════════ */
void setup() {
  Serial.begin(115200);

  // external pull-up (1K) use panrom
  pinMode(floatSwitchPin, INPUT);

  connectWiFi();
  Serial.println("[Tank] Stable Mode ON 🔥 (NC Switch Fixed)");
}

/* ═════════ LOOP ═════════ */
void loop() {

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > RETRY_INTERVAL) {
      connectWiFi();
      lastRetry = millis();
    }
  }

  unsigned long now = millis();

  // read sensor fast
  static unsigned long lastRead = 0;
  if (now - lastRead > 200) {
    readSensor();
    lastRead = now;
  }

  // send data
  bool levelChanged = (waterLevel != lastSentLevel);
  if (levelChanged || (now - lastSendTime > SEND_INTERVAL)) {
    updateSupabase();
    lastSendTime = now;
  }
}