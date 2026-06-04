/*
 * Excavator Rental Timer - Wi-Fi Slave (ESP8266 / NodeMCU)
 *
 * Compatible with the same Master API as the ESP32 slave.
 * Connects to "ExcavatorMaster", auto-registers, handles commands.
 *
 * NodeMCU Pin Mapping:
 *   D1 (GPIO5)  -> Relay
 *   D2 (GPIO4)  -> Buzzer (active)
 *   D5 (GPIO14) -> Button (INPUT_PULLUP)
 *   D6 (GPIO12) -> TM1637 CLK
 *   D7 (GPIO13) -> TM1637 DIO
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <TM1637Display.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// ===== PINS (NodeMCU D-pins) =====
static const uint8_t RELAY_PIN   = 5;   // D1
static const uint8_t BUZZER_PIN  = 4;   // D2
static const uint8_t LED1_PIN    = 2;   // D4 (ESP-12 module LED, active LOW)
static const uint8_t LED2_PIN    = 16;  // D0 (NodeMCU PCB LED, active LOW)
static const uint8_t BUTTON_PIN  = 14;  // D5
static const uint8_t CLK_PIN     = 12;  // D6
static const uint8_t DIO_PIN     = 13;  // D7
static const bool RELAY_ACTIVE_HIGH = true;

// ===== TIMING =====
static const uint32_t REGISTRATION_RETRY_MS = 5000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 15000;
static const uint32_t WIFI_RETRY_MS         = 5000;
static const uint32_t FLASH_SAVE_INTERVAL_S = 30;
static const uint32_t BUTTON_DEBOUNCE_MS    = 300;
static const uint32_t HTTP_TIMEOUT_MS       = 2000;
static const uint32_t MAX_REMAINING         = 28800; // 8 hours
static const int      MAX_ADD_TIME_MINUTES  = 480;

// ===== GLOBALS =====
TM1637Display display(CLK_PIN, DIO_PIN);
ESP8266WebServer server(80);

bool colonState = false;

// LED indicators
uint32_t activityLedEndMs = 0;

uint8_t  TOY_NUMERIC_ID = 0;
String   TOY_ID = "EXC-00";
static const char* WIFI_SSID = "ExcavatorMaster";
static const char* WIFI_PASS = "12345678";

enum RentalState : uint8_t {
  STATE_LOCKED  = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED  = 2,
  STATE_ENDED   = 4,
  STATE_FAULT   = 5,
};

const char* stateName(RentalState v);
void changeState(RentalState next);
void setup();
void loop();

const char* stateName(RentalState v) {
  switch (v) {
    case STATE_LOCKED:  return "LOCKED";
    case STATE_RUNNING: return "RUNNING";
    case STATE_PAUSED:  return "PAUSED";
    case STATE_ENDED:   return "ENDED";
    case STATE_FAULT:   return "FAULT";
  }
  return "FAULT";
}

RentalState state        = STATE_LOCKED;
uint32_t    remaining    = 0;
uint32_t    totalPaid    = 0;
uint8_t     seq          = 0;
uint32_t    lastTickMs   = 0;
uint32_t    lastWifiChk  = 0;
uint32_t    lastBtnMs    = 0;
bool        isRegistered = false;
uint32_t    regRetryDelay = REGISTRATION_RETRY_MS;
int         failedHb     = 0;

// ── EEPROM helpers ───────────────────────────────────────────

static const uint32_t EEPROM_MAGIC = 0x45584331; // "EXC1"

void eepromSave() {
  EEPROM.begin(36);
  EEPROM.put(0, EEPROM_MAGIC);
  EEPROM.put(4, remaining);
  EEPROM.put(8, totalPaid);
  EEPROM.commit();
  Serial.printf("[FLASH] saved rem=%lu paid=%lu\n", remaining, totalPaid);
}

void eepromLoad() {
  EEPROM.begin(36);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == EEPROM_MAGIC) {
    EEPROM.get(4, remaining);
    EEPROM.get(8, totalPaid);
  } else {
    remaining = 0;
    totalPaid = 0;
    Serial.println("[FLASH] No saved state found (fresh EEPROM).");
  }
  EEPROM.end();
}

// ── display ──────────────────────────────────────────────────

void updateDisplay() {
  if (state == STATE_LOCKED || state == STATE_ENDED) {
    uint8_t dash[] = { 0x40, 0x40, 0x40, 0x40 };
    display.setSegments(dash);
  } else {
    uint32_t show = remaining > 5999 ? 5999 : remaining;
    int mins = show / 60;
    int secs = show % 60;
    int disp = (mins * 100) + secs;
    display.showNumberDecEx(disp, colonState ? 0x40 : 0, true);
  }
}

// ── relay / buzzer / LEDs ────────────────────────────────────

void applyRelay() {
  if (state == STATE_RUNNING && remaining > 0)
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? HIGH : LOW);
  else
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
}

void beep(int ms, int count = 1) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED2_PIN, LOW);   // buzzer LED ON with sound
    delay(ms);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED2_PIN, HIGH);  // buzzer LED OFF
    if (i < count - 1) delay(100);
  }
}

void updateLeds() {
  // LED1: relay state — steady ON when relay is active
  bool relayOn = (state == STATE_RUNNING && remaining > 0);
  digitalWrite(LED1_PIN, relayOn ? LOW : HIGH);

  // LED2: activity indicator — turn off when flash expires
  if (activityLedEndMs > 0 && millis() < activityLedEndMs) return;
  activityLedEndMs = 0;
  digitalWrite(LED2_PIN, HIGH);  // OFF
}

void activityFlash(int ms = 80) {
  activityLedEndMs = millis() + ms;
  digitalWrite(LED2_PIN, LOW);  // ON immediately
}

// ── state transition ─────────────────────────────────────────

void changeState(RentalState next) {
  if (state != next) { state = next; seq++; }
  lastTickMs = millis();
  colonState = true;
  applyRelay();
  updateDisplay();
}

void addTime(int seconds) {
  remaining += seconds;
  if (remaining > MAX_REMAINING) remaining = MAX_REMAINING;
  totalPaid += seconds;
}

// ── JSON state ───────────────────────────────────────────────

String buildJsonState() {
  JsonDocument doc;
  doc["id"]   = TOY_ID;
  doc["state"] = stateName(state);
  doc["time_left"]   = remaining;
  char t[6];
  snprintf(t, sizeof(t), "%02lu:%02lu", remaining / 60, remaining % 60);
  doc["display"]  = t;
  doc["battery"]   = "OK";
  doc["fault"] = 0;
  doc["seq"]   = seq;
  String out;
  serializeJson(doc, out);
  return out;
}

// ── CORS ─────────────────────────────────────────────────────

void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
void handleOptions() { cors(); server.send(204); }

// ── GET /api/state ───────────────────────────────────────────

void handleGetState() {
  cors();
  server.send(200, "application/json", buildJsonState());
}

// ── POST /api/command ────────────────────────────────────────

void handleCommand() {
  cors();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":0,\"code\":\"BAD_FORMAT\"}");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"ok\":0,\"code\":\"BAD_JSON\"}");
    return;
  }

  String cmd = doc["cmd"] | "";
  int    time = doc["time"] | 0;
  cmd.toUpperCase();
  Serial.printf("[API] cmd='%s' time=%d\n", cmd.c_str(), time);
  beep(50, 1);
  activityFlash(80);

  bool ok = false;
  const char* code = "OK";

  if (cmd == "ADD_TIME" && time > 0) {
    int maxLimit = MAX_ADD_TIME_MINUTES * 60;
    if (time > maxLimit) {
      Serial.printf("[COMMAND] ADD_TIME rejected. Max limit is %d minutes.\n", MAX_ADD_TIME_MINUTES);
      code = "EXCEEDS_LIMIT";
    } else {
      addTime(time);
      Serial.printf("[COMMAND] ADD_TIME: %d seconds. Total remaining: %lu\n", time, (unsigned long)remaining);
      if (state == STATE_LOCKED || state == STATE_ENDED)
        changeState(STATE_RUNNING);
      else
        changeState(state);
      eepromSave();
      ok = true;
    }
  } else if (cmd == "PAUSE") {
    if (remaining > 0 && state == STATE_RUNNING) {
      changeState(STATE_PAUSED);
      eepromSave();
      ok = true;
    } else {
      code = "BAD_STATE";
    }
  } else if (cmd == "RESUME") {
    if (remaining == 0) {
      code = "BAD_STATE";
    } else {
      changeState(STATE_RUNNING);
      eepromSave();
      ok = true;
    }
  } else if (cmd == "STOP") {
    remaining = 0;
    totalPaid = 0;
    changeState(STATE_LOCKED);
    eepromSave();
    ok = true;
  } else if (cmd == "REBOOT") {
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":1,\"code\":\"REBOOTING\"}");
    server.send(200, "application/json", resp);
    delay(500);
    ESP.restart();
    return;
  } else if (cmd == "IDENTIFY") {
    ok = true;
  } else {
    code = "UNKNOWN_COMMAND";
  }

  char resp[128];
  snprintf(resp, sizeof(resp), "{\"ok\":%d,\"code\":\"%s\",\"time_left\":%lu,\"state\":\"%s\"}",
           ok ? 1 : 0, code, remaining, stateName(state));
  server.send(200, "application/json", resp);

  if (cmd == "IDENTIFY") {
    for (int i = 0; i < 3; i++) {
      beep(100, 1);
      display.clear();
      delay(150);
      updateDisplay();
      delay(150);
    }
  }
}

// ── registration / heartbeat ─────────────────────────────────

bool tryRegister() {
  WiFiClient client;
  HTTPClient http;
  String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
  Serial.println("[API] Register: " + url);
  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  bool ok = false;

  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      int id = doc["id"] | 0;
      if (id > 0) {
        TOY_NUMERIC_ID = id;
        char buf[16];
        snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
        TOY_ID = String(buf);
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

int tryHeartbeat() {
  WiFiClient client;
  HTTPClient http;
  String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  int result = -1;
  if (code == 200) {
    result = 0;
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      int newId = doc["id"] | 0;
      if (newId > 0 && (uint8_t)newId != TOY_NUMERIC_ID) {
        TOY_NUMERIC_ID = newId;
        char buf[16];
        snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
        TOY_ID = String(buf);
        result = 1;
      }
    }
  }
  http.end();
  return result;
}

// ── setup ────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("[SYSTEM] Starting Excavator Slave (ESP8266)...");
  Serial.println("========================================");

  display.setBrightness(0x0f);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED1_PIN, HIGH);  // LED off initially
  digitalWrite(LED2_PIN, HIGH);

  // boot LED test: flash both so user can see which works
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);
    delay(150);
    digitalWrite(LED1_PIN, HIGH);
    digitalWrite(LED2_PIN, HIGH);
    delay(150);
  }

  // powerloss recovery
  eepromLoad();
  if (remaining > 0) {
    Serial.printf("Powerloss Recovery: rem=%lu, auto-resuming in 3s...\n", remaining);
    for (int i = 0; i < 3; i++) { beep(100, 1); delay(900); }
    beep(500, 1);
    changeState(STATE_RUNNING);
    Serial.printf("Restored %lu seconds. State RUNNING.\n", remaining);
  } else {
    state = STATE_LOCKED;
  }
  applyRelay();

  // show "Conn" on display
  uint8_t d[] = { 0x39, 0x5c, 0x54, 0x54 };
  display.setSegments(d);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Offline mode; timer runs locally.");
  }

  // register
  uint8_t d2[] = { 0x50, 0x79, 0x6f, 0x00 };
  display.setSegments(d2);

  if (WiFi.status() == WL_CONNECTED) {
    if (tryRegister()) {
      isRegistered = true;
      Serial.println("[SYSTEM] Registered as: " + TOY_ID);
      beep(150, 3);
    }
  }

  updateDisplay();

  server.on("/api/state",    HTTP_GET,     handleGetState);
  server.on("/api/command",  HTTP_POST,    handleCommand);
  server.on("/api/command",  HTTP_OPTIONS, handleOptions);
  server.begin();

  Serial.println("[SYSTEM] Ready!");
  beep(150, 3);
}

// ── loop ─────────────────────────────────────────────────────

void loop() {
  server.handleClient();
  uint32_t now = millis();

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiChk >= WIFI_RETRY_MS) {
      WiFi.reconnect();
      activityFlash(200);
      lastWifiChk = now;
    }
  } else if (!isRegistered) {
    if (now - lastWifiChk >= regRetryDelay) {
      if (tryRegister()) {
        isRegistered = true;
        failedHb = 0;
        regRetryDelay = REGISTRATION_RETRY_MS;
        Serial.println("[SYSTEM] Registered: " + TOY_ID);
        activityFlash(200);
        beep(150, 3);
      } else {
        if (regRetryDelay < 60000) regRetryDelay = min(regRetryDelay * 2, (uint32_t)60000);
      }
      lastWifiChk = now;
    }
  } else {
    // heartbeat
    if (now - lastWifiChk >= HEARTBEAT_INTERVAL_MS) {
      int hb = tryHeartbeat();
      activityFlash(80);
      if (hb == -1) {
        failedHb++;
        if (failedHb >= 3) {
          isRegistered = false;
          Serial.println("[WIFI] Master unresponsive, dropping registration.");
        }
      } else {
        failedHb = 0;
        if (hb == 1) beep(100, 2);
      }
      lastWifiChk = now;
    }
  }

  // button
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (now - lastBtnMs > BUTTON_DEBOUNCE_MS) {
      lastBtnMs = now;
      if ((state == STATE_PAUSED || state == STATE_FAULT) && remaining > 0) {
        Serial.println("[ACTION] Button: Resumed");
        beep(50, 1);
        changeState(STATE_RUNNING);
        eepromSave();
      }
    }
  }

  // timer tick
  if (state == STATE_RUNNING && remaining > 0 && now - lastTickMs >= 1000) {
    lastTickMs += 1000;
    colonState = !colonState;
    remaining--;

    if (remaining == 60)        beep(200, 1);
    else if (remaining <= 10 && remaining > 0) beep(50, 1);

    if (remaining % FLASH_SAVE_INTERVAL_S == 0 && remaining > 0) eepromSave();

    updateDisplay();

    if (remaining == 0) {
      Serial.println("[TIMER] Time up! Locking.");
      changeState(STATE_ENDED);
      eepromSave();
      beep(1000, 1);
    }
  }

  if (state != STATE_RUNNING) {
    lastTickMs = now;
  }

  updateLeds();

  // heartbeat serial print
  static uint32_t lastHb = 0;
  if (millis() - lastHb >= 1000) {
    lastHb = millis();
    Serial.printf("[SLAVE-HB] up=%lus id=%s state=%s rem=%lu\n",
                  millis() / 1000, TOY_ID.c_str(), stateName(state), remaining);
  }
}
