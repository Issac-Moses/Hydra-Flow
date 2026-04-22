/* ═══════════════════════════════════════════════════
   TankSync IoT Motor Control Node (v6.0 - STATIC IP)
   - Multi-Schedule support
   - Heartbeat tracking (using motor_ping toggle)
   - 30-min local safety timer
   - STATIC IP CONFIGURATION (192.168.1.184)
 ═══════════════════════════════════════════════════ */
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESPDateTime.h>

/* ═════════ STATIC IP CONFIG ═════════ */
IPAddress local_IP(192, 168, 1, 184);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // Optional
IPAddress secondaryDNS(8, 8, 4, 4); // Optional

/* ═════════ CONFIGURATION ═════════ */
const char* ssid     = "IssacMoses";
const char* password = "moses@1234";

const char* supabaseUrl = "https://ulyeoukxyaadfvekdmwc.supabase.co";
const char* anonKey     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVseWVvdWt4eWFhZGZ2ZWtkbXdjIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM5ODk1NTcsImV4cCI6MjA4OTU2NTU1N30.w_37uATvP3SFiWPUvUtzPaGTs4oYQQok1uW7RXe5luE";

/* ═════════ PINS ═════════ */
const int relayPin = D1; // Active HIGH for relay

/* ═════════ SETTINGS ═════════ */
const int           SAFETY_MINS    = 30;    // Hardware-level safety timer
const unsigned long POLL_INTERVAL  = 5000;  // Poll DB every 5s
const unsigned long SYNC_INTERVAL  = 10000; // Sync local status every 10s
const unsigned long RETRY_INTERVAL = 10000; // WiFi retry
const unsigned long WIFI_RETRY_MS  = 15000;

/* ═════════ GLOBAL STATE ═════════ */
bool motorStatus      = false;
bool manualOverride   = false;
bool isOnline         = false;
bool motor_ping       = false; // Heartbeat toggle
String systemMode     = "OFFLINE";
unsigned long motorStartedAt = 0; // ms

// Multi-Schedule Data
struct MotorSchedule {
  String onTime; // "HH:MM"
  bool   enabled;
};
MotorSchedule schedules[10];
int scheduleCount = 0;
bool ntpSynced    = false;

/* ═════════ WIFI ═════════ */
unsigned long startTime = 0;
bool connecting         = false;

ESP8266WebServer server(80);

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    connecting = false;
    return;
  }

  if (!connecting) {
    WiFi.disconnect(true);
    delay(100);
    
    // Config Static IP
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("[WiFi] Static IP Failed to configure");
    }

    WiFi.mode(WIFI_STA);
    WiFi.setOutputPower(20.5);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(ssid, password);
    connecting = true;
    startTime  = millis();
    Serial.println("[WiFi] Connecting (Static IP: 192.168.1.184)...");
  }

  if (millis() - startTime > WIFI_RETRY_MS) {
    Serial.println("[WiFi] Retry...");
    connecting = false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! Current IP: ");
    Serial.println(WiFi.localIP());
  }
}

// ═══════════════════════════════════════════════
//  SUPABASE – PATCH (sync local → DB)
// ═══════════════════════════════════════════════
void syncToSupabase() {
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

  motor_ping = !motor_ping; // Toggle heartbeat bit
  String payload = "{\"motor_status\":"  + String(motorStatus ? "true" : "false") +
                   ",\"mode\":\""        + systemMode + "\"" +
                   ",\"motor_ping\":"    + String(motor_ping ? "true" : "false") + "}";

  int code = http.sendRequest("PATCH", payload);
  Serial.printf("[Supabase] PATCH → %d | Motor: %s\n", code, motorStatus ? "ON" : "OFF");
  http.end();

  if (code > 0) {
    isOnline   = true;
    systemMode = "ONLINE";
  }
}

// ═══════════════════════════════════════════════
//  SUPABASE – GET (poll DB → local)
// ═══════════════════════════════════════════════
void pollSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(supabaseUrl) + "/rest/v1/motor_system?id=eq.1&select=motor_status";
  http.begin(client, url);
  http.setTimeout(5000);
  http.addHeader("apikey",        anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, body);

    if (!err && doc.size() > 0) {
      bool remoteStatus = doc[0]["motor_status"].as<bool>();

      static bool lastRemoteStatus = false;
      if (remoteStatus != lastRemoteStatus) {
         lastRemoteStatus = remoteStatus;
         setMotor(remoteStatus, remoteStatus ? "manual_trigger" : "tank_full_or_manual");
      }
      isOnline = true;
      systemMode = "ONLINE";
    }
  } else {
    isOnline = false;
    systemMode = "OFFLINE";
  }
  http.end();
}

// ═══════════════════════════════════════════════
//  FETCH SCHEDULES
// ═══════════════════════════════════════════════
void fetchSchedule() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(supabaseUrl) + "/rest/v1/motor_schedules?select=on_time,enabled";
  http.begin(client, url);
  http.addHeader("apikey", anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);

    if (!err) {
      scheduleCount = 0;
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject obj : arr) {
        if (scheduleCount < 10) {
          schedules[scheduleCount].onTime  = obj["on_time"].as<String>();
          schedules[scheduleCount].enabled = obj["enabled"].as<bool>();
          scheduleCount++;
        }
      }
      Serial.printf("[Schedules] Loaded %d timers\n", scheduleCount);
    }
  }
  http.end();
}

// ═══════════════════════════════════════════════
//  MOTOR CONTROL LOGIC
// ═══════════════════════════════════════════════
void setMotor(bool status, String reason) {
  if (motorStatus == status) return;

  motorStatus = status;
  digitalWrite(relayPin, motorStatus ? HIGH : LOW);
  
  if (status) motorStartedAt = millis();
  else        motorStartedAt = 0;

  Serial.printf("[Motor] Event: %s | Reason: %s\n", status ? "ON" : "OFF", reason.c_str());
  syncToSupabase();
}

void checkSafetyTimer() {
  if (motorStatus && motorStartedAt > 0) {
    if (millis() - motorStartedAt > (unsigned long)SAFETY_MINS * 60 * 1000) {
      setMotor(false, "safety_timer_30min");
    }
  }
}

void checkSchedules() {
  if (!ntpSynced || motorStatus) return; // Only start if OFF and time is synced

  String currentTime = DateTime.format("%H:%M");
  static String lastRunMinute = "";

  if (currentTime == lastRunMinute) return;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].enabled && schedules[i].onTime == currentTime) {
      setMotor(true, "schedule");
      lastRunMinute = currentTime;
      break;
    }
  }
}

// ═══════════════════════════════════════════════
//  LOCAL WEB SERVER
// ═══════════════════════════════════════════════
void handleStatus() {
  String json = "{\"motor\":" + String(motorStatus ? "true" : "false") + 
                 ",\"mode\":\"" + systemMode + "\"" +
                 ",\"uptime\":" + String(millis() / 1000) + 
                 ",\"ntp\":" + String(ntpSynced ? "true" : "false") + 
                 ",\"time\":\"" + DateTime.toString() + "\"}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleOn() {
  setMotor(true, "local_web");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"status\":\"success\",\"motor\":true}");
}

void handleOff() {
  setMotor(false, "local_web");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"status\":\"success\",\"motor\":false}");
}

void handleRoot() {
  String html = "<html><head><title>TankSync Local</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center; padding:20px; background:#1a1a2e; color:#fff;} ";
  html += ".card{background:#16213e; padding:30px; border-radius:15px; box-shadow:0 10px 20px rgba(0,0,0,0.3); display:inline-block; min-width:300px;} ";
  html += ".status{font-size:28px; font-weight:bold; color:#00e5ff; margin-bottom:20px;} ";
  html += ".btn{padding:15px 30px; font-size:18px; font-weight:bold; cursor:pointer; border:none; border-radius:8px; margin:10px; transition:0.3s;} ";
  html += ".btn-on{background:#00e676; color:#000;} .btn-off{background:#ff5252; color:#fff;} ";
  html += ".btn:hover{opacity:0.8; transform:scale(1.05);} p{color:#8b8b8b; margin:5px 0;} </style></head><body>";
  
  html += "<div class='card'><h1>TankSync Motor Node</h1>";
  html += "<div class='status'>" + String(motorStatus ? "● MOTOR RUNNING" : "○ MOTOR STOPPED") + "</div>";
  html += "<div class='ctrl'>";
  html += "<a href='/on'><button class='btn btn-on'>TURN ON</button></a>";
  html += "<a href='/off'><button class='btn btn-off'>TURN OFF</button></a>";
  html += "</div><hr style='border:0; border-top:1px solid #1f4068; margin:20px 0;'>";
  html += "<p>Mode: <b>" + systemMode + "</b></p>";
  html += "<p>Schedules: <b>" + String(scheduleCount) + " loaded</b></p>";
  html += "<p>Time: " + DateTime.toString() + " (" + String(ntpSynced ? "SYNCED" : "UNSYNCED") + ")</p>";
  html += "<p>Safety Timer: 30 min (Hardcoded)</p>";
  html += "</div></body></html>";
  
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  connectWiFi();
  
  DateTime.setServer("pool.ntp.org");
  DateTime.setTimeZone("IST-5:30");
  DateTime.begin(15000);

  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/status", handleStatus);
  server.begin();
  
  fetchSchedule();
  Serial.println("[System] Motor Node Initialized with Static IP.");
}

void loop() {
  server.handleClient();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpSynced) {
      ntpSynced = DateTime.isTimeValid();
    }

    unsigned long now = millis();
    static unsigned long lastPoll = 0;
    static unsigned long lastSync = 0;
    static unsigned long lastFetch = 0;

    if (now - lastPoll > POLL_INTERVAL) {
      pollSupabase();
      lastPoll = now;
    }
    
    if (now - lastSync > SYNC_INTERVAL) {
      syncToSupabase();
      lastSync = now;
    }

    if (now - lastFetch > 300000) { // Refresh schedules every 5m
      fetchSchedule();
      lastFetch = now;
    }

    checkSchedules();
  }

  checkSafetyTimer();
  delay(100);
}
