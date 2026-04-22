/* ═══════════════════════════════════════════════════
   TankSync IoT Motor Control Node (v5.3 - STABLE FIX)
   - Multi-Schedule support
   - Heartbeat tracking (using motor_ping toggle)
   - 30-min local safety timer
   - FIX: motorLockUntil guard (prevents echo-OFF)
   - FIX: pendingSync flag (eliminates double PATCH)
   - FIX: pollSupabase reads water_level for smart decisions
═══════════════════════════════════════════════════ */
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESPDateTime.h>

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
const unsigned long SYNC_INTERVAL  = 15000; // Sync local status every 15s
const unsigned long WIFI_RETRY_MS  = 15000;

// ── STABILITY FIX: Lock polling after any motor event ──
const unsigned long MOTOR_LOCK_MS  = 20000; // 20s — prevents echo-OFF after motor event

/* ═════════ GLOBAL STATE ═════════ */
bool motorStatus      = false;
bool manualOverride   = false;
bool isOnline         = false;
bool motor_ping       = false;
String systemMode     = "OFFLINE";
unsigned long motorStartedAt  = 0;
bool lastRemoteStatus         = false;

// ── STABILITY FIX: Pending sync & lock ──
bool          pendingSync    = false;  // set true when motor state changes
unsigned long motorLockUntil = 0;     // poll is skipped until this millis()

// Multi-Schedule Data
struct MotorSchedule {
  String onTime; // "HH:MM"
  bool   enabled;
};
MotorSchedule schedules[10];
int scheduleCount = 0;
bool ntpSynced    = false;

/* ═════════ WIFI ═════════ */
unsigned long wifiStartTime = 0;
bool connecting             = false;

ESP8266WebServer server(80);

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    connecting = false;
    return;
  }

  if (!connecting) {
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setOutputPower(20.5);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(ssid, password);
    connecting    = true;
    wifiStartTime = millis();
    Serial.println("[WiFi] Connecting...");
  }

  if (millis() - wifiStartTime > WIFI_RETRY_MS) {
    Serial.println("[WiFi] Retry...");
    connecting = false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! IP: ");
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

  motor_ping = !motor_ping;
  String payload = "{\"motor_status\":"  + String(motorStatus ? "true" : "false") +
                   ",\"mode\":\""        + systemMode + "\"" +
                   ",\"motor_ping\":"    + String(motor_ping ? "true" : "false") + "}";

  int code = http.sendRequest("PATCH", payload);
  if (code > 0) {
    Serial.printf("[Supabase] PATCH OK → %d | Motor: %s | Ping: %d\n",
                  code, motorStatus ? "ON" : "OFF", motor_ping);
    isOnline   = true;
    systemMode = "ONLINE";
  } else {
    Serial.printf("[Supabase] PATCH FAIL → %d (%s)\n", code, http.errorToString(code).c_str());
    isOnline   = false;
    systemMode = "OFFLINE";
  }
  http.end();
}

// ═══════════════════════════════════════════════
//  SUPABASE – GET (poll DB → local)
//  FIX: reads water_level too, respects motorLockUntil
// ═══════════════════════════════════════════════
void pollSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Always poll to check water_level (needed for tank-full safety)
  String url = String(supabaseUrl) +
               "/rest/v1/motor_system?id=eq.1&select=motor_status,water_level";
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
      int  remoteLevel  = doc[0]["water_level"].as<int>();

      isOnline   = true;
      systemMode = "ONLINE";

      // ── SAFETY: Tank full — ALWAYS force OFF, lock is bypassed ──
      // This ensures tank-full auto-off works even within the 20s motor lock window
      if (remoteLevel == 100 && motorStatus) {
        Serial.println("[Poll] Tank FULL — Force motor OFF (lock bypassed)");
        lastRemoteStatus = false;
        setMotor(false, "tank_full_poll");
        http.end();
        return;
      }

      // ── LOCK CHECK: Skip non-safety status changes during lock period ──
      if (motorStatus && millis() < motorLockUntil) {
        Serial.println("[Poll] Status change ignored — motor lock active");
        http.end();
        return;
      }

      // ── NORMAL: Act on DB motor_status changes ──
      if (remoteStatus != lastRemoteStatus) {
        Serial.printf("[Poll] DB change: %s → %s\n",
                      lastRemoteStatus ? "ON" : "OFF",
                      remoteStatus     ? "ON" : "OFF");
        lastRemoteStatus = remoteStatus;
        setMotor(remoteStatus, remoteStatus ? "remote_on" : "remote_off");
      }
    }
  } else {
    isOnline   = false;
    systemMode = "OFFLINE";
    Serial.printf("[Poll] FAIL → %d\n", code);
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
//  FIX: No longer calls syncToSupabase() directly.
//       Sets pendingSync=true and motorLockUntil.
// ═══════════════════════════════════════════════
void setMotor(bool status, String reason) {
  if (motorStatus == status) return;

  motorStatus = status;
  digitalWrite(relayPin, motorStatus ? HIGH : LOW);

  if (status) motorStartedAt = millis();
  else        motorStartedAt = 0;

  lastRemoteStatus = status;

  Serial.printf("[Motor] %s | Reason: %s\n", status ? "ON" : "OFF", reason.c_str());

  // ── FIX: Schedule sync without blocking, set lock to prevent echo-OFF ──
  pendingSync    = true;
  motorLockUntil = millis() + MOTOR_LOCK_MS;
}

void checkSafetyTimer() {
  if (motorStatus && motorStartedAt > 0) {
    if (millis() - motorStartedAt > (unsigned long)SAFETY_MINS * 60 * 1000) {
      Serial.println("[Safety] 30-min timer expired → Motor OFF");
      setMotor(false, "safety_timer_30min");
    }
  }
}

void checkSchedules() {
  if (!ntpSynced || motorStatus) return;

  String currentTime  = DateTime.format("%H:%M");
  static String lastRunMinute = "";

  if (currentTime == lastRunMinute) return;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].enabled && schedules[i].onTime == currentTime) {
      Serial.printf("[Schedule] Triggered at %s\n", currentTime.c_str());
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
  String json = "{\"motor\":"    + String(motorStatus ? "true" : "false") +
                ",\"mode\":\""  + systemMode + "\"" +
                ",\"uptime\":"  + String(millis() / 1000) +
                ",\"ntp\":"     + String(ntpSynced ? "true" : "false") +
                ",\"time\":\""  + DateTime.toString() + "\""
                ",\"lock\":"    + String(millis() < motorLockUntil ? "true" : "false") + "}";
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
  String lockStatus = (millis() < motorLockUntil)
    ? "<span style='color:#f1c40f'>LOCKED (" + String((motorLockUntil - millis()) / 1000) + "s)</span>"
    : "<span style='color:#2ecc71'>UNLOCKED</span>";

  String html = "<html><head><title>TankSync Local</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
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
  html += "<p>Poll Lock: " + lockStatus + "</p>";
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

  server.on("/",       handleRoot);
  server.on("/on",     handleOn);
  server.on("/off",    handleOff);
  server.on("/status", handleStatus);
  server.begin();

  fetchSchedule();
  Serial.println("[System] Motor Node v5.3 Ready (Stability Fix Active)");
}

void loop() {
  server.handleClient();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpSynced) {
      ntpSynced = DateTime.isTimeValid();
    }

    unsigned long now = millis();
    static unsigned long lastPoll  = 0;
    static unsigned long lastSync  = 0;
    static unsigned long lastFetch = 0;

    // ── Priority: handle pending sync immediately after motor event (500ms debounce) ──
    if (pendingSync && (now >= motorLockUntil - MOTOR_LOCK_MS + 500)) {
      syncToSupabase();
      pendingSync = false;
      lastSync    = now; // Reset 15s timer to prevent double-sync
    }

    // ── Poll DB for remote commands ──
    if (now - lastPoll > POLL_INTERVAL) {
      pollSupabase();
      lastPoll = now;
    }

    // ── Periodic heartbeat sync ──
    if (now - lastSync > SYNC_INTERVAL) {
      syncToSupabase();
      lastSync = now;
    }

    // ── Refresh schedule list every 5 minutes ──
    if (now - lastFetch > 300000) {
      fetchSchedule();
      lastFetch = now;
    }

    checkSchedules();
  }

  checkSafetyTimer();
  delay(100);
}
