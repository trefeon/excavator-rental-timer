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

// ===== PINS =====
static const uint8_t RELAY_PIN = 26;
static const uint8_t BUZZER_PIN = 27;
static const uint8_t CLK_PIN = 22;
static const uint8_t DIO_PIN = 23;
static const bool RELAY_ACTIVE_HIGH = true;

// ===== GLOBALS =====
TM1637Display display(CLK_PIN, DIO_PIN);
bool colonState = false;
WebServer server(80);
Preferences preferences;

uint8_t TOY_NUMERIC_ID = 0;
String TOY_ID = "EXC-00"; // Default until registered
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
bool isRegistered = false;

void updateDisplay() {
  if (state == STATE_LOCKED || state == STATE_ENDED) {
    uint8_t data[] = { 0x40, 0x40, 0x40, 0x40 }; // "----"
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
  Serial.printf("State saved to flash: rem=%u\n", remainingSeconds);
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
    "{\"toy\":\"%s\",\"state\":\"%s\",\"rem\":%u,\"disp\":\"%02u:%02u\",\"paid\":%u,\"bat\":\"OK\",\"fault\":0,\"seq\":%u}",
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
  beep(50, 1);

  if (cmd == "ADD_TIME") {
    remainingSeconds += val;
    totalPaidSeconds += val;
    if (state == STATE_LOCKED || state == STATE_ENDED) {
      changeState(STATE_RUNNING);
    } else {
      changeState(state);
    }
    saveStateToFlash();
    ok = true;
  } else if (cmd == "PAUSE") {
    if (remainingSeconds > 0 && state == STATE_RUNNING) {
      changeState(STATE_PAUSED);
      saveStateToFlash();
      ok = true;
    } else { code = "BAD_STATE"; }
  } else if (cmd == "RESUME") {
    if (remainingSeconds == 0) { code = "BAD_STATE"; }
    else { 
      changeState(STATE_RUNNING); 
      saveStateToFlash();
      ok = true; 
    }
  } else if (cmd == "STOP") {
    remainingSeconds = 0;
    changeState(STATE_LOCKED);
    saveStateToFlash();
    ok = true;
  } else if (cmd == "REBOOT") {
    server.send(200, "application/json", "{\"ok\":1,\"code\":\"REBOOTING\"}");
    delay(500);
    ESP.restart();
    return;
  } else {
    code = "UNKNOWN_COMMAND";
  }
  
  char resp[128];
  snprintf(resp, sizeof(resp), "{\"ok\":%d,\"code\":\"%s\",\"rem\":%u,\"state\":\"%s\"}", 
           ok ? 1 : 0, code.c_str(), remainingSeconds, stateName(state).c_str());
           
  server.send(200, "application/json", String(resp));
}

void setup() {
  Serial.begin(115200);
  
  display.setBrightness(0x0f);
  
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Load State from Flash for Powerloss Recovery
  preferences.begin("state", true);
  uint32_t savedRem = preferences.getUInt("rem", 0);
  uint32_t savedPaid = preferences.getUInt("paid", 0);
  preferences.end();
  
  if (savedRem > 0) {
    remainingSeconds = savedRem;
    totalPaidSeconds = savedPaid;
    state = STATE_PAUSED; // SAFETY: Force Pause to avoid sudden movement
    Serial.printf("Powerloss Recovery: Restored %u seconds. State PAUSED.\n", remainingSeconds);
  } else {
    state = STATE_LOCKED;
  }

  applyRelay();

  // Show "Conn" (Connecting)
  uint8_t dataConn[] = { 0x39, 0x5c, 0x54, 0x54 }; // C o n n
  display.setSegments(dataConn);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // Show "rEg " (Registering)
  uint8_t dataReg[] = { 0x50, 0x79, 0x6f, 0x00 }; // r E g (blank)
  display.setSegments(dataReg);

  String mac = WiFi.macAddress();
  while (!isRegistered) {
    HTTPClient http;
    String url = "http://192.168.4.1/api/register?mac=" + mac;
    Serial.println("Registering: " + url);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      Serial.println("Registration Response: " + payload);
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
          isRegistered = true;
        }
      }
    }
    http.end();
    if (!isRegistered) delay(2000);
  }

  Serial.println("Successfully Registered as: " + TOY_ID);
  updateDisplay(); // Shows "----"

  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/command", HTTP_POST, handleCommand);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.begin();
  
  beep(100, 2);
}

void loop() {
  server.handleClient();
  
  if (isRegistered) {
    uint32_t now = millis();
    
    // Heartbeat & ID Sync every 15 seconds
    if (now - lastRegisterMs >= 15000) {
      lastRegisterMs = now;
      HTTPClient http;
      String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
      http.begin(url);
      int httpCode = http.GET();
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
              Serial.println("ID updated by Master to: " + TOY_ID);
            }
          }
        }
      }
      http.end();
    }

    if (state == STATE_RUNNING && (now - lastTickMs >= 1000)) {
      lastTickMs = now;
      colonState = !colonState;
      if (remainingSeconds > 0) {
        remainingSeconds--;
        // Auto-save to flash every 30 seconds to prevent wear & tear
        if (remainingSeconds % 30 == 0 && remainingSeconds > 0) {
          saveStateToFlash();
        }
      }
      updateDisplay();
      
      if (remainingSeconds == 0) {
        changeState(STATE_ENDED);
        saveStateToFlash();
        beep(1000, 1);
      }
    } else if (state != STATE_RUNNING) {
      lastTickMs = now;
    }
  }
}
