/* ═══════════════════════════════════════════════════
   TankSync IoT AC Control Node (v1.0)
   - AC On/Off control via Relay (D1)
   - Multi-Schedule support (ON and OFF times)
   - Heartbeat tracking (using ac_ping toggle)
   - STATIC IP CONFIGURATION (192.168.1.185)
   - Real-time Supabase integration
 ═══════════════════════════════════════════════════ */
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESPDateTime.h>

/* ═════════ STATIC IP CONFIG ═════════ */
IPAddress local_IP(192, 168, 1, 185); // Unique IP for AC node
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

/* ═════════ CONFIGURATION ═════════ */
const char* ssid     = "IssacMoses";
const char* password = "moses@1234";

const char* supabaseUrl = "https://ulyeoukxyaadfvekdmwc.supabase.co";
const char* anonKey     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InVseWVvdWt4eWFhZGZ2ZWtkbXdjIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM5ODk1NTcsImV4cCI6MjA4OTU2NTU1N30.w_37uATvP3SFiWPUvUtzPaGTs4oYQQok1uW7RXe5luE";

/* ═════════ PINS ═════════ */
const int relayPin = D1; // Connected to Relay via IRLZ44N

/* ═════════ SETTINGS ═════════ */
const unsigned long POLL_INTERVAL  = 5000;
const unsigned long SYNC_INTERVAL  = 15000;
const unsigned long WIFI_RETRY_MS  = 15000;

/* ═════════ GLOBAL STATE ═════════ */
bool acStatus         = false;
bool isOnline         = false;
bool ac_ping          = false;
String systemMode     = "OFFLINE";
bool lastRemoteStatus = false;
bool pendingSync      = false;

// Multi-Schedule Data
struct ACSchedule {
  String onTime;
  String offTime;
  bool   enabled;
};
ACSchedule schedules[10];
int scheduleCount = 0;
bool ntpSynced    = false;

/* ═════════ WIFI ═════════ */
unsigned long wifiStartTime = 0;
bool connecting             = false;

ESP8266WebServer server(80);

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (connecting) {
      Serial.println("\n[WiFi] Connected!");
      Serial.print("IP Address: "); Serial.println(WiFi.localIP());
      Serial.print("Gateway:    "); Serial.println(WiFi.gatewayIP());
      Serial.print("DNS:        "); Serial.println(WiFi.dnsIP());
      connecting = false;
    }
    return;
  }

  if (!connecting) {
    WiFi.disconnect(true);
    delay(100);

    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("[WiFi] Static IP Failed to configure");
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    connecting    = true;
    wifiStartTime = millis();
    Serial.println("[WiFi] Connecting (Static IP: 192.168.1.185)...");
  }

  if (millis() - wifiStartTime > WIFI_RETRY_MS) {
    Serial.println("[WiFi] Connection Timeout, Retrying...");
    connecting = false;
  }
}

// ═══════════════════════════════════════════════
//  SUPABASE – PATCH (sync local → DB)
// ═══════════════════════════════════════════════
void syncToSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024); // Optimization: Reduce memory usage for SSL
  
  HTTPClient http;

  String url = String(supabaseUrl) + "/rest/v1/ac_system?id=eq.1";
  http.begin(client, url);
  http.setTimeout(8000); // Increased timeout
  http.addHeader("apikey",        anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Prefer",        "return=minimal");

  ac_ping = !ac_ping;
  String payload = "{\"ac_status\":" + String(acStatus ? "true" : "false") +
                   ",\"mode\":\""    + systemMode + "\"" +
                   ",\"ac_ping\":"   + String(ac_ping ? "true" : "false") + "}";

  int code = http.sendRequest("PATCH", payload);
  if (code > 0) {
    Serial.printf("[Supabase] PATCH OK → %d | AC: %s | Ping: %d\n", 
                  code, acStatus ? "ON" : "OFF", ac_ping);
    isOnline = true;
    systemMode = "ONLINE";
  } else {
    Serial.printf("[Supabase] PATCH FAIL → %d (%s)\n", code, http.errorToString(code).c_str());
    isOnline = false;
    systemMode = "OFFLINE";
  }
  http.end();
}

// ═══════════════════════════════════════════════
//  SUPABASE – GET (poll DB → local)
// ═══════════════════════════════════════════════
void pollSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024); // Optimization: Reduce memory usage for SSL
  
  HTTPClient http;

  String url = String(supabaseUrl) + "/rest/v1/ac_system?id=eq.1&select=ac_status";
  http.begin(client, url);
  http.setTimeout(8000); // Increased timeout
  http.addHeader("apikey",        anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument doc(128);
    deserializeJson(doc, body);

    if (doc.size() > 0) {
      bool remoteStatus = doc[0]["ac_status"].as<bool>();
      if (remoteStatus != lastRemoteStatus) {
        Serial.printf("[Poll] DB Change: %s → %s\n", 
                      lastRemoteStatus ? "ON" : "OFF", 
                      remoteStatus ? "ON" : "OFF");
        lastRemoteStatus = remoteStatus;
        setAC(remoteStatus, "remote_command");
      }
    }
    isOnline = true;
    systemMode = "ONLINE";
  } else {
    Serial.printf("[Poll] FAIL → %d\n", code);
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

  String url = String(supabaseUrl) + "/rest/v1/ac_schedules?select=on_time,off_time,enabled";
  http.begin(client, url);
  http.addHeader("apikey", anonKey);
  http.addHeader("Authorization", "Bearer " + String(anonKey));

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, body);

    scheduleCount = 0;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
      if (scheduleCount < 10) {
        schedules[scheduleCount].onTime  = obj["on_time"].as<String>();
        schedules[scheduleCount].offTime = obj["off_time"].as<String>();
        schedules[scheduleCount].enabled = obj["enabled"].as<bool>();
        scheduleCount++;
      }
    }
    Serial.printf("[Schedules] Loaded %d timers\n", scheduleCount);
  }
  http.end();
}

// ═══════════════════════════════════════════════
//  AC CONTROL LOGIC
// ═══════════════════════════════════════════════
void setAC(bool status, String reason) {
  if (acStatus == status) return;

  acStatus = status;
  digitalWrite(relayPin, acStatus ? HIGH : LOW);
  lastRemoteStatus = status;

  Serial.printf("[AC] %s | Reason: %s\n", status ? "ON" : "OFF", reason.c_str());
  pendingSync = true;
}

void checkSchedules() {
  if (!ntpSynced) return;

  String currentTime = DateTime.format("%H:%M");
  static String lastRunMinute = "";

  if (currentTime == lastRunMinute) return;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].enabled) {
      // Check ON time
      if (schedules[i].onTime == currentTime && !acStatus) {
        setAC(true, "schedule_on");
        lastRunMinute = currentTime;
      }
      // Check OFF time
      else if (schedules[i].offTime == currentTime && acStatus) {
        setAC(false, "schedule_off");
        lastRunMinute = currentTime;
      }
    }
  }
}

// ═══════════════════════════════════════════════
//  LOCAL WEB SERVER
// ═══════════════════════════════════════════════
void handleRoot() {
  String html = "<html><head><title>AC Control Local</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center; padding:20px; background:#1a1a2e; color:#fff;} ";
  html += ".card{background:#16213e; padding:30px; border-radius:15px; display:inline-block; min-width:300px;} ";
  html += ".status{font-size:28px; font-weight:bold; color:#00e5ff; margin-bottom:20px;} ";
  html += ".btn{padding:15px 30px; font-size:18px; cursor:pointer; border-radius:8px; margin:10px; border:none;} ";
  html += ".btn-on{background:#00e676; color:#000;} .btn-off{background:#ff5252; color:#fff;} </style></head><body>";
  html += "<div class='card'><h1>AC Control Node</h1>";
  html += "<div class='status'>" + String(acStatus ? "● AC RUNNING" : "○ AC STOPPED") + "</div>";
  html += "<a href='/on'><button class='btn btn-on'>TURN ON</button></a>";
  html += "<a href='/off'><button class='btn btn-off'>TURN OFF</button></a>";
  html += "<p>Mode: <b>" + systemMode + "</b></p></div></body></html>";
  server.send(200, "text/html", html);
}

void handleOn() { setAC(true, "local_web"); server.sendHeader("Location", "/"); server.send(303); }
void handleOff() { setAC(false, "local_web"); server.sendHeader("Location", "/"); server.send(303); }

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
  server.begin();

  fetchSchedule();
  Serial.println("[System] AC Node Ready");
}

void loop() {
  server.handleClient();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpSynced) ntpSynced = DateTime.isTimeValid();

    unsigned long now = millis();
    static unsigned long lastPoll = 0;
    static unsigned long lastSync = 0;
    static unsigned long lastFetch = 0;

    if (pendingSync) {
      syncToSupabase();
      pendingSync = false;
      lastSync = now;
    }

    if (now - lastPoll > POLL_INTERVAL) {
      pollSupabase();
      lastPoll = now;
    }

    if (now - lastSync > SYNC_INTERVAL) {
      syncToSupabase();
      lastSync = now;
    }

    if (now - lastFetch > 300000) {
      fetchSchedule();
      lastFetch = now;
    }

    checkSchedules();
  }
  delay(100);
}
