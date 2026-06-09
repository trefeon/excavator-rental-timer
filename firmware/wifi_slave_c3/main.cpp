/*
 * Excavator Rental Timer - Wi-Fi Slave (Zero-Touch Auto Provisioning)
 * ESP32-C3 Super Mini Version
 * 
 * =========================================================
 * HARDWARE PINOUT & WIRING GUIDE
 * =========================================================
 * 
 * 1. TM1637 4-Digit Display
 *    - VCC  -> 5V
 *    - GND  -> GND
 *    - CLK  -> GPIO 6
 *    - DIO  -> GPIO 7
 * 
 * 2. Relay Module (1-Channel)
 *    - VCC  -> 5V
 *    - GND  -> GND
 *    - IN   -> GPIO 4
 * 
 * 3. Buzzer (Active)
 *    - VCC / +  -> GPIO 5
 *    - GND / -  -> GND
 * 
 * 4. Button
 *    - PIN  -> GPIO 9
 *    - GND  -> GND
 * 
 * 5. Network LED
 *    - Built-in LED on GPIO 8 (Active LOW)
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
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

// ===== PINS =====
static const uint8_t RELAY_PIN = 4;
static const uint8_t BUZZER_PIN = 5;
static const uint8_t CLK_PIN = 6;
static const uint8_t DIO_PIN = 7;
static const uint8_t BUTTON_PIN = 9;
static const uint8_t NET_LED_PIN = 8;   // built-in LED (network activity)

// ===== KONFIGURASI TRIGGER RELAY / MOSFET =====
// 1 = Relay Module High Trigger
// 2 = Relay Module Low Trigger (Butuh trik High-Z untuk modul 5V)
// 3 = MOSFET Module (seperti XY-MOS, aktif HIGH)
static const uint8_t TRIGGER_MODE = 2; // Ganti angka ini sesuai hardware

// ===== TIMING CONSTANTS =====
static const uint32_t REGISTRATION_RETRY_INTERVAL_MS = 5000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 15000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t FLASH_SAVE_INTERVAL_S = 30;
static const uint32_t BUTTON_DEBOUNCE_MS = 300;
static const uint32_t HTTP_TIMEOUT_MS = 2000;
static const int MAX_ADD_TIME_MINUTES = 480;
static const uint32_t MAX_REMAINING = 28800;  // 8 hours in seconds

// ===== GLOBALS =====
TM1637Display display(CLK_PIN, DIO_PIN);
bool colonState = false;
WebServer server(80);
Preferences preferences;

SemaphoreHandle_t stateMutex;
volatile int failedHeartbeats = 0;

volatile uint32_t netLedEndMs = 0;

uint8_t TOY_NUMERIC_ID = 0;
String TOY_ID = "EXC-00";
static const char* WIFI_SSID = "ExcavatorMaster";
static const char* WIFI_PASS = "12345678";

enum RentalState : uint8_t {
  STATE_LOCKED = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED = 2,
  STATE_ENDED = 4,
  STATE_FAULT = 5,
};

const char* stateName(RentalState value);
void changeState(RentalState nextState);

RentalState state = STATE_LOCKED;
uint32_t remainingSeconds = 0;
uint32_t totalPaidSeconds = 0;
uint8_t seq = 0;
uint32_t lastTickMs = 0;
uint32_t lastWifiCheck = 0;
uint32_t lastButtonPressMs = 0;
bool isRegistered = false;
volatile bool wifiDisconnected = false;
uint32_t regRetryDelay = REGISTRATION_RETRY_INTERVAL_MS;
int pendingBeepMs = 0;

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

bool isRelayOn() {
  return (state == STATE_RUNNING && remainingSeconds > 0);
}

void applyRelay() {
  if (isRelayOn()) {
    pinMode(RELAY_PIN, OUTPUT);
    if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
      digitalWrite(RELAY_PIN, HIGH);
    } else {
      digitalWrite(RELAY_PIN, LOW); // Low Trigger On
    }
  } else {
    if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
      pinMode(RELAY_PIN, OUTPUT);
      digitalWrite(RELAY_PIN, LOW);
    } else {
      // Trik Active LOW: atur sebagai INPUT (High-Z) untuk mematikan optocoupler 5V
      pinMode(RELAY_PIN, INPUT);
    }
  }
  // Immediately update LED base state if not currently flashing
  if (netLedEndMs == 0) {
    digitalWrite(NET_LED_PIN, isRelayOn() ? LOW : HIGH);
  }
}

void netLedFlash(int ms = 50) {
  netLedEndMs = millis() + ms;
  // Invert current base state to create a flash/wink
  // Active LOW: LOW = ON, HIGH = OFF
  digitalWrite(NET_LED_PIN, isRelayOn() ? HIGH : LOW);
}

void updateNetLed() {
  if (netLedEndMs > 0 && millis() < netLedEndMs) return;
  netLedEndMs = 0;
  // Restore to base state
  digitalWrite(NET_LED_PIN, isRelayOn() ? LOW : HIGH);
}

const char* stateName(RentalState value) {
  switch (value) {
    case STATE_LOCKED: return "LOCKED";
    case STATE_RUNNING: return "RUNNING";
    case STATE_PAUSED: return "PAUSED";
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
  lastTickMs = millis();
  colonState = true;
  applyRelay();
  updateDisplay();
}

void addTime(int seconds) {
  remainingSeconds += seconds;
  if (remainingSeconds > MAX_REMAINING) remainingSeconds = MAX_REMAINING;
  totalPaidSeconds += seconds;
}

String buildJsonState() {
  JsonDocument doc;
  doc["id"] = TOY_ID;
  doc["state"] = stateName(state);
  doc["time_left"] = remainingSeconds;
  
  char timeStr[6];
  snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu", remainingSeconds / 60, remainingSeconds % 60);
  doc["display"] = timeStr;
  
  doc["battery"] = "OK";
  doc["fault"] = 0;
  doc["seq"] = seq;

  String output;
  serializeJson(doc, output);
  return output;
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
  String json = "";
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    json = buildJsonState();
    xSemaphoreGive(stateMutex);
  }
  server.send(200, "application/json", json);
}

void handleCommand() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":0,\"code\":\"BAD_FORMAT\"}");
    return;
  }

  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "application/json", "{\"ok\":0,\"code\":\"BAD_JSON\"}");
    return;
  }

  String cmd = doc["cmd"] | "";
  int time = doc["time"] | 0;

  bool ok = false;
  const char* code = "OK";
  String respString = "";

  cmd.toUpperCase();
  Serial.printf("[API] Received Command: '%s' with Val: %d\n", cmd.c_str(), time);
  beep(50, 1);
  netLedFlash(50);

  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    if (cmd == "ADD_TIME" && time > 0) {
      int maxLimit = MAX_ADD_TIME_MINUTES * 60;
      if (time > maxLimit) {
        Serial.printf("[COMMAND] ADD_TIME rejected. Max limit is %d minutes.\n", MAX_ADD_TIME_MINUTES);
        code = "EXCEEDS_LIMIT";
      } else {
        addTime(time);
        Serial.printf("[COMMAND] ADD_TIME: %d seconds. Total remaining: %lu\n", time, remainingSeconds);
        if (state == STATE_LOCKED || state == STATE_ENDED) {
          changeState(STATE_RUNNING);
        } else {
          changeState(state);
        }
        saveStateToFlash();
        ok = true;
      }
    } else if (cmd == "PAUSE") {
      if (remainingSeconds > 0 && state == STATE_RUNNING) {
        Serial.println("[ACTION] Paused Timer");
        changeState(STATE_PAUSED);
        saveStateToFlash();
        ok = true;
      } else {
        code = "BAD_STATE";
      }
    } else if (cmd == "RESUME") {
      if (remainingSeconds == 0) {
        code = "BAD_STATE";
      } else {
        Serial.println("[ACTION] Resumed Timer via API");
        changeState(STATE_RUNNING);
        saveStateToFlash();
        ok = true;
      }
    } else if (cmd == "STOP") {
      Serial.println("[ACTION] Stopped / Locked Timer");
      remainingSeconds = 0;
      totalPaidSeconds = 0;
      changeState(STATE_LOCKED);
      saveStateToFlash();
      ok = true;
    } else if (cmd == "REBOOT") {
      Serial.println("[ACTION] Rebooting device by API command");
      xSemaphoreGive(stateMutex);
      server.send(200, "application/json", "{\"ok\":1,\"code\":\"REBOOTING\"}");
      for (int i = 0; i < 5; i++) { netLedFlash(100); delay(100); }
      delay(200);
      ESP.restart();
      return;
    } else if (cmd == "IDENTIFY") {
      Serial.println("[ACTION] Identify Ping triggered");
      ok = true;
    } else {
      code = "UNKNOWN_COMMAND";
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":%d,\"code\":\"%s\",\"time_left\":%lu,\"state\":\"%s\"}",
             ok ? 1 : 0, code, remainingSeconds, stateName(state));
    respString = String(resp);
    xSemaphoreGive(stateMutex);
  }

  server.send(200, "application/json", respString);

  if (cmd == "IDENTIFY") {
    for (int i = 0; i < 3; i++) {
      beep(100, 1);
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        display.clear();
        xSemaphoreGive(stateMutex);
      }
      delay(150);
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        updateDisplay();
        xSemaphoreGive(stateMutex);
      }
      delay(150);
    }
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected from AP");
      wifiDisconnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Got IP: ");
      Serial.println(WiFi.localIP());
      wifiDisconnected = false;
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
    netLedFlash(100);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      int parsedId = doc["id"] | 0;
      if (parsedId > 0) {
        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
          TOY_NUMERIC_ID = parsedId;
          char buf[16];
          snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
          TOY_ID = String(buf);
          success = true;
          xSemaphoreGive(stateMutex);
        }
      } else {
        Serial.println("[API] Registration rejected: Master assigned ID 0 (no free IDs)");
      }
    } else {
      Serial.println("[API] Failed to parse registration response");
    }
  } else {
    Serial.printf("[API] Registration failed (HTTP %d)\n", httpCode);
  }
  http.end();
  return success;
}

int tryHeartbeat() {
  HTTPClient http;
  String url = "http://192.168.4.1/api/register?mac=" + WiFi.macAddress();
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int httpCode = http.GET();
  int result = -1;

  if (httpCode == 200) {
    result = 0;
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      int newId = doc["id"] | 0;
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        if (newId > 0 && (uint8_t)newId != TOY_NUMERIC_ID) {
          TOY_NUMERIC_ID = newId;
          char buf[16];
          snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
          TOY_ID = String(buf);
          Serial.println("[SYNC] ID updated by Master to: " + TOY_ID);
          result = 1;
        }
        xSemaphoreGive(stateMutex);
      }
    }
  } else {
    Serial.printf("[API] Heartbeat failed (HTTP %d)\n", httpCode);
  }
  http.end();
  return result;
}

void delayWDT(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void networkTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    if (WiFi.status() == WL_CONNECTED) {
      bool regStat = false;
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        regStat = isRegistered;
        xSemaphoreGive(stateMutex);
      }

      if (!regStat) {
        delayWDT(random(1000, 3000));
        if (tryRegister()) {
          if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
            isRegistered = true;
            failedHeartbeats = 0;
            regRetryDelay = REGISTRATION_RETRY_INTERVAL_MS;
            xSemaphoreGive(stateMutex);
          }
          Serial.println("[SYSTEM] Successfully Registered as: " + TOY_ID);
          beep(150, 3);
        } else {
          delayWDT(regRetryDelay);
          if (regRetryDelay < 60000) regRetryDelay = min(regRetryDelay * 2, (uint32_t)60000);
        }
      } else {
        delayWDT(HEARTBEAT_INTERVAL_MS);
        int hb = tryHeartbeat();
        netLedFlash(80);
        if (hb == -1) {
          failedHeartbeats = failedHeartbeats + 1;
          if (failedHeartbeats >= 3) {
            Serial.println("[WIFI] Master unresponsive. Dropping registration.");
            if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
              isRegistered = false;
              xSemaphoreGive(stateMutex);
            }
          }
        } else {
          failedHeartbeats = 0;
          if (hb == 1) {
            beep(100, 2);
          }
        }
      }
    } else {
      delayWDT(1000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  Serial.println("\n\n========================================");
  Serial.println("[SYSTEM] Starting Excavator Slave...");
  Serial.println("========================================");

  display.setBrightness(0x0f);

  if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
    digitalWrite(RELAY_PIN, LOW);
    pinMode(RELAY_PIN, OUTPUT);
  } else {
    pinMode(RELAY_PIN, INPUT); // Default ke High-Z (Mati) untuk relay active low
  }
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(NET_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(NET_LED_PIN, HIGH);  // OFF initially

#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = 10000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(10, true);
#endif
  esp_task_wdt_add(NULL);

  stateMutex = xSemaphoreCreateMutex();

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

    changeState(STATE_RUNNING);
    Serial.printf("Powerloss Recovery: Restored %lu seconds. State RUNNING.\n", remainingSeconds);
  } else {
    changeState(STATE_LOCKED);
  }

  applyRelay();

  uint8_t dataConn[] = { 0x39, 0x5c, 0x54, 0x54 };
  display.setSegments(dataConn);

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE); // Disable modem sleep
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // Lower TX power to prevent voltage brownouts on C3 Super Mini
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WIFI] Connecting to Wi-Fi");
  uint32_t connectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    if (millis() - connectStart > 30000) {
      Serial.println("\n[WIFI] Connection timeout, starting offline mode...");
      wifiDisconnected = true;
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Offline mode active; timer will keep running locally.");
  }

  uint8_t dataReg[] = { 0x50, 0x79, 0x6f, 0x00 };
  display.setSegments(dataReg);

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);

  updateDisplay();

  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/command", HTTP_POST, handleCommand);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.begin();

  Serial.println("[SYSTEM] Ready! Listening for commands...");
  beep(150, 3);
}

void loop() {
  esp_task_wdt_reset();
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiDisconnected) {
      wifiDisconnected = true;
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        isRegistered = false;
        failedHeartbeats = 0;
        xSemaphoreGive(stateMutex);
      }
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
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        isRegistered = false;
        failedHeartbeats = 0;
        xSemaphoreGive(stateMutex);
      }
      Serial.println("[WIFI] Reconnected! IP: " + WiFi.localIP().toString());
    }
    server.handleClient();
  }

  now = millis();
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
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

    while (state == STATE_RUNNING && remainingSeconds > 0 && now - lastTickMs >= 1000) {
      lastTickMs += 1000;
      now = millis();
      colonState = !colonState;
      remainingSeconds--;

      if (remainingSeconds == 60) {
        pendingBeepMs = 200;
      } else if (remainingSeconds <= 10 && remainingSeconds > 0) {
        pendingBeepMs = 50;
      }

      if (remainingSeconds % FLASH_SAVE_INTERVAL_S == 0 && remainingSeconds > 0) {
        saveStateToFlash();
      }

      updateDisplay();

      if (remainingSeconds == 0) {
        Serial.println("[TIMER] Time is up! Locking Excavator.");
        changeState(STATE_ENDED);
        saveStateToFlash();
        pendingBeepMs = 1000;
        break;
      }
    }

    if (state != STATE_RUNNING) {
      lastTickMs = now;
    }
    xSemaphoreGive(stateMutex);
  }

  if (pendingBeepMs > 0) {
    int ms = pendingBeepMs;
    pendingBeepMs = 0;
    beep(ms, 1);
  }

  updateNetLed();

  static uint32_t lastLedBlink = 0;
  if (millis() - lastLedBlink >= 3000) {
    lastLedBlink = millis();
    netLedFlash(50);
  }

  static uint32_t lastHb = 0;
  if (millis() - lastHb >= 1000) {
    lastHb = millis();
    if (xSemaphoreTake(stateMutex, 0) == pdTRUE) {
      Serial.printf("[SLAVE-HB] up=%lus id=%s state=%s rem=%lu\n",
                    millis() / 1000, TOY_ID.c_str(), stateName(state), remainingSeconds);
      xSemaphoreGive(stateMutex);
    }
  }
}
