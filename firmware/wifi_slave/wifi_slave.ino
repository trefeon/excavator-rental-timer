/*
 * Excavator Rental Timer - Wi-Fi Slave (Zero-Touch Auto Provisioning)
 * 
 * =========================================================
 * HARDWARE PINOUT & WIRING GUIDE
 * =========================================================
 * 
 * 1. TM1637 4-Digit Display
 *    - VCC  -> 5V (or 3.3V, but 5V gives better brightness)
 *    - GND  -> GND
 *    - CLK  -> GPIO 22
 *    - DIO  -> GPIO 23
 * 
 * 2. Relay Module (1-Channel)
 *    - VCC  -> 5V (or 3.3V depending on relay module)
 *    - GND  -> GND
 *    - IN   -> GPIO 26
 * 
 * 3. Buzzer (Active)
 *    - VCC / +  -> GPIO 27
 *    - GND / -  -> GND
 * 
 * =========================================================
 * NETWORK CONFIGURATION
 * =========================================================
 * - Connects to SSID "ExcavatorMaster" via DHCP.
 * - Auto-Registers itself to Master via http://192.168.4.1/api/register
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <TM1637Display.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ===== PINS =====
static const uint8_t RELAY_PIN = 26;
static const uint8_t BUZZER_PIN = 27;
static const uint8_t CLK_PIN = 22;
static const uint8_t DIO_PIN = 23;
static const uint8_t BUTTON_PIN = 32;
static const bool RELAY_ACTIVE_HIGH = true;

// ===== TIMING CONSTANTS =====
static const uint32_t REGISTRATION_TIMEOUT_MS = 60000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 15000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t FLASH_SAVE_INTERVAL_S = 30;
static const uint32_t BUTTON_DEBOUNCE_MS = 300;
static const uint32_t HTTP_TIMEOUT_MS = 2000;

// ===== GLOBALS =====
TM1637Display display(CLK_PIN, DIO_PIN);
bool colonState = false;
WebServer server(80);
Preferences preferences;

uint8_t TOY_NUMERIC_ID = 0;
String TOY_ID = "EXC-00";
static const char* WIFI_SSID = "ExcavatorMaster";
static const char* WIFI_PASS = "12345678";

enum RentalState : uint8_t {
  STATE_LOCKED = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED = 2,
  STATE_LOW_BATT = 3,
  STATE_ENDED = 4,
  STATE_FAULT = 5,
};

RentalState state = STATE_LOCKED;
uint32_t remainingSeconds = 0;
uint32_t totalPaidSeconds = 0;
uint8_t seq = 0;
uint32_t lastTickMs = 0;
uint32_t lastRegisterMs = 0;
uint32_t lastWifiCheck = 0;
uint32_t lastButtonPressMs = 0;
uint32_t registrationStartMs = 0;
bool isRegistered = false;
bool wifiDisconnected = false;

void updateDisplay() {
  if (state == STATE_LOCKED || state == STATE_ENDED) {
    uint8_t data[] = { 0x40, 0x40, 0x40, 0x40 };
    display.setSegments(data);
  } else {
    int mins = remainingSeconds / 60;
    int secs = remainingSeconds % 60;
    int dispTime = (mins * 100) + secs;
    display.showNumberDecEx(dispTime, colonState ? 0b01000000 : 0, true);
  }
}

void beep(int durationMs, int count = 1) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(100);
  }
}

void applyRelay() {
  if (state == STATE_RUNNING && remainingSeconds > 0) {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? HIGH : LOW);
  } else {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  }
}

String stateName(RentalState value) {
  switch (value) {
    case STATE_LOCKED: return "LOCKED";
    case STATE_RUNNING: return "RUNNING";
    case STATE_PAUSED: return "PAUSED";
    case STATE_LOW_BATT: return "LOW_BATT";
    case STATE_ENDED: return "ENDED";
    case STATE_FAULT: return "FAULT";
  }
  return "FAULT";
}

void saveStateToFlash() {
  preferences.begin("state", false);
  preferences.putUInt("rem", remainingSeconds);
  preferences.putUInt("paid", totalPaidSeconds);
  preferences.end();
  Serial.printf("State saved to flash: rem=%lu\n", remainingSeconds);
}

void changeState(RentalState nextState) {
  if (state != nextState) {
    state = nextState;
    seq++;
  }
  applyRelay();
  colonState = true;
  updateDisplay();
}

String buildJsonState() {
  char buf[128];
  snprintf(buf, sizeof(buf), 
    "{\"toy\":\"%s\",\"state\":\"%s\",\"rem\":%lu,\"disp\":\"%02lu:%02lu\",\"paid\":%lu,\"bat\":\"OK\",\"fault\":0,\"seq\":%u}",
    TOY_ID.c_str(), stateName(state).c_str(), remainingSeconds, remainingSeconds/60, remainingSeconds%60, totalPaidSeconds, seq
  );
  return String(buf);
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleGetState() {
  addCorsHeaders();
  server.send(200, "application/json", buildJsonState());
}

void handleCommand() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":0,\"code\":\"BAD_FORMAT\"}");
    return;
  }
  
  String body = server.arg("plain");
  String cmd = "";
  int val = 0;
  
  int cmdIdx = body.indexOf("\"cmd\":");
  if (cmdIdx >= 0) {
    int start = body.indexOf("\"", cmdIdx + 6) + 1;
    int end = body.indexOf("\"", start);
    if (start > 0 && end > start) cmd = body.substring(start, end);
  }
  
  int valIdx = body.indexOf("\"val\":");
  if (valIdx >= 0) {
    int start = valIdx + 6;
    int end = body.indexOf("}", start);
    if (end < 0) end = body.indexOf(",", start);
    if (start > 0 && end > start) val = body.substring(start, end).toInt();
  }

  bool ok = false;
  String code = "OK";
  
  cmd.toUpperCase();
  Serial.printf("[API] Received Command: '%s' with Val: %d\n", cmd.c_str(), val);
  beep(50, 1);

  if (cmd == "ADD_TIME") {
    remainingSeconds += val;
    totalPaidSeconds += val;
    Serial.printf("[ACTION] Added %d seconds. Total remaining: %lu\n", val, remainingSeconds);
    if (state == STATE_LOCKED || state == STATE_ENDED) {
      changeState(STATE_RUNNING);
    } else {
      changeState(state);
    }
    saveStateToFlash();
    ok = true;
  } else if (cmd == "PAUSE") {
    if (remainingSeconds > 0 && state == STATE_RUNNING) {
      Serial.println("[ACTION] Paused Timer");
      changeState(STATE_PAUSED);
      saveStateToFlash();
      ok = true;
    } else { code = "BAD_STATE"; }
  } else if (cmd == "RESUME") {
    if (remainingSeconds == 0) { code = "BAD_STATE"; }
    else { 
      Serial.println("[ACTION] Resumed Timer via API");
      changeState(STATE_RUNNING); 
      saveStateToFlash();
      ok = true; 
    }
  } else if (cmd == "STOP") {
    Serial.println("[ACTION] Stopped / Locked Timer");
    remainingSeconds = 0;
    changeState(STATE_LOCKED);
    saveStateToFlash();
    ok = true;
  } else if (cmd == "REBOOT") {
    Serial.println("[ACTION] Rebooting device by API command");
    server.send(200, "application/json", "{\"ok\":1,\"code\":\"REBOOTING\"}");
    delay(500);
    ESP.restart();
    return;
  } else if (cmd == "IDENTIFY") {
    Serial.println("[ACTION] Identify Ping triggered");
    for (int i = 0; i < 3; i++) {
      beep(100, 1);
      display.clear();
      delay(100);
      updateDisplay();
    }
    ok = true;
  } else {
    code = "UNKNOWN_COMMAND";
  }
  
  char resp[128];
  snprintf(resp, sizeof(resp), "{\"ok\":%d,\"code\":\"%s\",\"rem\":%lu,\"state\":\"%s\"}", 
           ok ? 1 : 0, code.c_str(), remainingSeconds, stateName(state).c_str());
            
  server.send(200, "application/json", String(resp));
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected from AP");
      wifiDisconnected = true;
      isRegistered = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Got IP: ");
      Serial.println(WiFi.localIP());
      wifiDisconnected = false;
      registrationStartMs = millis();
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WIFI] Connected to AP");
      break;
    default:
      break;
  }
}

bool tryRegister() {
  HTTPClient http;
  String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
  Serial.println("[API] Registering: " + url);
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int httpCode = http.GET();
  bool success = false;
  
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[API] Registration Response: " + payload);
    int idx = payload.indexOf("\"id\":");
    if (idx >= 0) {
      int start = idx + 5;
      int end = payload.indexOf("}", start);
      if (end < 0) end = payload.indexOf(",", start);
      if (start > 0 && end > start) {
        TOY_NUMERIC_ID = payload.substring(start, end).toInt();
        char buf[16];
        snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
        TOY_ID = String(buf);
        success = true;
      }
    }
  } else {
    Serial.printf("[API] Registration failed (HTTP %d)\n", httpCode);
  }
  http.end();
  return success;
}

bool tryHeartbeat() {
  HTTPClient http;
  String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int httpCode = http.GET();
  bool idChanged = false;
  
  if (httpCode == 200) {
    String payload = http.getString();
    int idx = payload.indexOf("\"id\":");
    if (idx >= 0) {
      int start = idx + 5;
      int end = payload.indexOf("}", start);
      if (end < 0) end = payload.indexOf(",", start);
      if (start > 0 && end > start) {
        uint8_t newId = payload.substring(start, end).toInt();
        if (newId != TOY_NUMERIC_ID && newId > 0) {
          TOY_NUMERIC_ID = newId;
          char buf[16];
          snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
          TOY_ID = String(buf);
          Serial.println("[SYNC] ID updated by Master to: " + TOY_ID);
          beep(100, 2);
          idChanged = true;
        }
      }
    }
  }
  http.end();
  return idChanged;
}

void setup() {
  Serial.begin(115200);
  
  Serial.println("\n\n========================================");
  Serial.println("[SYSTEM] Starting Excavator Slave...");
  Serial.println("========================================");
  
  display.setBrightness(0x0f);
  
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  preferences.begin("state", true);
  uint32_t savedRem = preferences.getUInt("rem", 0);
  uint32_t savedPaid = preferences.getUInt("paid", 0);
  preferences.end();
  
  if (savedRem > 0) {
    remainingSeconds = savedRem;
    totalPaidSeconds = savedPaid;
    
    Serial.println("Powerloss Recovery: Auto-resuming in 3 seconds...");
    for (int i = 0; i < 3; i++) {
       beep(100, 1);
       delay(900);
    }
    beep(500, 1);
    
    state = STATE_RUNNING;
    Serial.printf("Powerloss Recovery: Restored %lu seconds. State RUNNING.\n", remainingSeconds);
  } else {
    state = STATE_LOCKED;
  }

  applyRelay();

  uint8_t dataConn[] = { 0x39, 0x5c, 0x54, 0x54 };
  display.setSegments(dataConn);

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  Serial.print("[WIFI] Connecting to Wi-Fi");
  uint32_t connectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - connectStart > 30000) {
      Serial.println("\n[WIFI] Connection timeout, restarting...");
      ESP.restart();
    }
  }
  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());

  uint8_t dataReg[] = { 0x50, 0x79, 0x6f, 0x00 };
  display.setSegments(dataReg);

  registrationStartMs = millis();
  while (!isRegistered) {
    esp_task_wdt_reset();
    
    if (tryRegister()) {
      isRegistered = true;
    } else {
      if (millis() - registrationStartMs > REGISTRATION_TIMEOUT_MS) {
        Serial.println("[REGISTER] Timeout waiting for Master. Retrying with reboot...");
        delay(1000);
        ESP.restart();
      }
      delay(2000);
    }
  }

  Serial.println("[SYSTEM] Successfully Registered as: " + TOY_ID);
  updateDisplay();

  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/command", HTTP_POST, handleCommand);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.begin();
  
  Serial.println("[SYSTEM] Ready! Listening for commands...");
  esp_task_wdt_add(NULL);
  beep(150, 3);
}

void loop() {
  uint32_t now = millis();
  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiDisconnected) {
      wifiDisconnected = true;
      isRegistered = false;
      Serial.println("[WIFI] Connection lost!");
    }
    if (now - lastWifiCheck >= WIFI_RETRY_INTERVAL_MS) {
      Serial.println("[WIFI] Attempting reconnect...");
      WiFi.reconnect();
      lastWifiCheck = now;
    }
  } else {
    if (wifiDisconnected) {
      wifiDisconnected = false;
      isRegistered = false;
      registrationStartMs = now;
      Serial.println("[WIFI] Reconnected! IP: " + WiFi.localIP().toString());
    }
    server.handleClient();
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (now - lastButtonPressMs > BUTTON_DEBOUNCE_MS) {
      lastButtonPressMs = now;
      if ((state == STATE_PAUSED || state == STATE_FAULT) && remainingSeconds > 0) {
        Serial.println("[ACTION] Physical Button Pressed: Resumed Timer");
        beep(50, 1);
        changeState(STATE_RUNNING);
        saveStateToFlash();
      }
    }
  }

  if (!isRegistered && WiFi.status() == WL_CONNECTED) {
    if (now - registrationStartMs < REGISTRATION_TIMEOUT_MS) {
      if (tryRegister()) {
        isRegistered = true;
        Serial.println("[SYSTEM] Re-registered as: " + TOY_ID);
        updateDisplay();
        beep(150, 3);
      }
    }
  }

  if (isRegistered && WiFi.status() == WL_CONNECTED) {
    if (now - lastRegisterMs >= HEARTBEAT_INTERVAL_MS) {
      lastRegisterMs = now;
      tryHeartbeat();
    }

    if (state == STATE_RUNNING && (now - lastTickMs >= 1000)) {
      lastTickMs = now;
      colonState = !colonState;
      if (remainingSeconds > 0) {
        remainingSeconds--;
        
        if (remainingSeconds == 60) {
          beep(200, 2);
        } else if (remainingSeconds <= 10 && remainingSeconds > 0) {
          beep(50, 1);
        }
        
        if (remainingSeconds % FLASH_SAVE_INTERVAL_S == 0 && remainingSeconds > 0) {
          saveStateToFlash();
        }
      }
      updateDisplay();
      
      if (remainingSeconds == 0) {
        Serial.println("[TIMER] Time is up! Locking Excavator.");
        changeState(STATE_ENDED);
        saveStateToFlash();
        beep(1000, 1);
      }
    } else if (state != STATE_RUNNING) {
      lastTickMs = now;
    }
  }
}
