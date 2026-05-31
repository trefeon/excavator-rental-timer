/*
 * EXCAVATOR TIMER RENTAL — ESP32 Firmware v2.1
 * ==============================================
 * Konteks: Sewa mainan excavator RC di mall untuk dewasa
 * 
 * Arsitektur Daya — 1 Batre untuk RC + ESP32
 *   Batre RC (LiPo 7.4V) → Buck Converter 3.3V → ESP32
 *                           └→ Motor RC (ESC/Receiver) langsung
 *   Kapasitor 1000µF di output buck nahan drop motor.
 * 
 * Source of Truth — Android App
 *   ESP32 NVS cuma buffer sementara. Aplikasi Android yg decide.
 *   Pas boot: ESP32 baca NVS, kirim ke app. App bandingkan dgn
 *   datanya sendiri, lalu kirim perintah yg sesuai.
 * 
 * Fitur:
 * - BLE GATT Server (on-demand connection)
 * - NVS buffer sementara (IDLE / RUNNING / PAUSED / FINISHED / TIMEOUT)
 * - 4-Digit 7-Segment TM1637 (MM:SS countdown)
 * - Buzzer pas timer habis
 * - Battery monitoring via ADC (7.4V LiPo via divider)
 * - Tombol fisik untuk reset lokal (opsional)
 * - Serial command bridge (for Wokwi simulation / debugging)
 * 
 * Wiring:
 *   TM1637   : CLK -> GPIO18, DIO -> GPIO19, VCC -> 3.3V, GND -> GND
 *   Buzzer   : GPIO13 (+), GND (-)
 *   Button 1 : GPIO14 (reset/stop) — pullup
 *   Button 2 : GPIO27 (test buzzer) — pullup
 *   Batre RC : Via divider (R1=100k, R2=33k) -> GPIO34 ADC
 *   Buck     : 7.4V → 3.3V, output + kapasitor 1000µF
 * 
 * BLE Service:
 *   UUID: 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 *   Char 1 — TimerValue (write): set timer duration in seconds
 *   Char 2 — TimerStatus (read/notify): JSON {remaining, state, voltage}
 *   Char 3 — Command (write): "START" / "STOP" / "RESET" / "PAUSE" / "CLEAR"
 *   Char 4 — DeviceName (read): "EX-XX"
 *   Char 5 — DeviceInfo (read): full JSON state dump
 *
 * Serial Commands (for Wokwi / debug):
 *   SET <seconds>  — set timer duration (e.g. SET 300)
 *   START          — start countdown
 *   STOP           — stop (pause) countdown
 *   PAUSE          — pause countdown
 *   RESET          — reset timer to IDLE
 *   CLEAR          — clear NVS + reset
 *   STATUS         — print current state JSON
 */

// Uncomment for Wokwi simulation (disables BLE, enables serial commands)
#define WOKWI_SIMULATION

#ifndef WOKWI_SIMULATION
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#endif
#include <Preferences.h>
#include <TM1637Display.h>

// ===== PINOUT =====
#define BUZZER_PIN     13
#define BTN_RESET      14
#define BTN_TEST       27
#define ADC_POWER_PIN  34    // ADC — baca tegangan batre
#define TM1637_CLK     18
#define TM1637_DIO     19

// ===== BLE =====
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_TIMER_VALUE    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_TIMER_STATUS   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define CHAR_COMMAND        "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CHAR_DEVICE_NAME    "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define CHAR_DEVICE_INFO    "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define DEVICE_NAME         "EX-01"    // ganti per unit

// ===== STATE =====
enum TimerState { IDLE, RUNNING, PAUSED, FINISHED, TIMEOUT };
TimerState currentState = IDLE;

// ===== TIMER =====
unsigned long timerDuration  = 0;
unsigned long timerRemaining = 0;
unsigned long timerStartedAt = 0;
unsigned long timerPausedAt  = 0;
unsigned long lastSecondTick = 0;

// ===== NVS =====
Preferences prefs;
#define NVS_NAMESPACE "timer"
#define NVS_STATE     "state"
#define NVS_REMAINING "remaining"
#define NVS_DURATION  "duration"

// ===== 4-Digit 7-Segment =====
TM1637Display display(TM1637_CLK, TM1637_DIO);

// ===== BLE =====
#ifndef WOKWI_SIMULATION
BLEServer      *pServer       = NULL;
BLECharacteristic *charStatus = NULL;
BLECharacteristic *charInfo   = NULL;
BLEAdvertising *pAdvertising  = NULL;
#endif
bool deviceConnected          = false;

// ===== BUZZER AUTO-OFF =====
unsigned long buzzerStartedAt = 0;

// ===== CALLBACKS =====

#ifndef WOKWI_SIMULATION
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* p) {
    deviceConnected = true;
    Serial.println("[BLE]" " HP connected");
    display.showNumberDecEx(8888, 0b01000000, true);
  }

  void onDisconnect(BLEServer* p) {
    deviceConnected = false;
    Serial.println("[BLE]" " HP disconnected");
    pAdvertising->start();
  }
};

class MyCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String uuid = pChar->getUUID().toString();
    String value = pChar->getValue().c_str();

    Serial.printf("[BLE]" " Write: %s -> %s\n", uuid.c_str(), value.c_str());

    if (uuid == CHAR_TIMER_VALUE) {
      timerDuration = value.toInt();
      if (timerDuration > 0 && timerDuration <= 3600) {
        timerRemaining = timerDuration;
        updateDisplay();
        Serial.printf("[Timer]" " Duration set: %ds\n", timerDuration);
        updateCharStatus();
      }
    }
    else if (uuid == CHAR_COMMAND) {
      value.toUpperCase();
      if (value == "START" && timerRemaining > 0) {
        startTimer();
      }
      else if (value == "STOP") {
        stopTimer();
      }
      else if (value == "RESET") {
        resetTimer();
      }
      else if (value == "PAUSE") {
        pauseTimer();
      }
      else if (value == "CLEAR") {
        // From app: "I'm the source of truth, clear your NVS"
        clearNVS();
        resetTimer();
      }
      updateCharStatus();
    }
  }
};
#endif

// ===== FUNGSI TIMER =====

void startTimer() {
  if (timerRemaining <= 0) return;

  if (currentState == PAUSED) {
    timerStartedAt = millis() - (timerDuration - timerRemaining) * 1000;
  } else {
    timerStartedAt = millis();
  }
  
  currentState = RUNNING;
  lastSecondTick = 0;
  saveToNVS();
  updateDisplay();
    Serial.printf("[Timer]" " STARTED: %ds remaining\n", timerRemaining);
}

void stopTimer() {
  if (currentState == RUNNING) {
    pauseTimer();
    Serial.println("[Timer]" " STOPPED (paused)");
  }
}

void resetTimer() {
  currentState = IDLE;
  timerRemaining = 0;
  timerDuration = 0;
  timerStartedAt = 0;
  timerPausedAt = 0;
  digitalWrite(BUZZER_PIN, LOW);
  saveToNVS();
  updateDisplay();
  updateCharStatus();
  Serial.println("[Timer]" " RESET");
}

void pauseTimer() {
  if (currentState == RUNNING) {
    unsigned long elapsed = (millis() - timerStartedAt) / 1000;
    timerRemaining = (elapsed >= timerDuration) ? 0 : timerDuration - elapsed;
    currentState = PAUSED;
    timerPausedAt = millis();
    saveToNVS();
    updateDisplay();
    Serial.printf("[Timer]" " PAUSED: %ds remaining\n", timerRemaining);
  }
}

void finishTimer() {
  currentState = FINISHED;
  timerRemaining = 0;
  digitalWrite(BUZZER_PIN, HIGH);
  saveToNVS();
  updateDisplay();
  updateCharStatus();
  Serial.println("[Timer]" " FINISHED! Buzzer ON");
}

// ===== NVS =====

void saveToNVS() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_STATE, stateToString(currentState));
  prefs.putULong(NVS_REMAINING, timerRemaining);
  prefs.putULong(NVS_DURATION, timerDuration);
  prefs.end();
  Serial.printf("[NVS]" " Saved: state=%s, remaining=%ds\n",
    stateToString(currentState).c_str(), timerRemaining);
}

void loadFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  String savedState = prefs.getString(NVS_STATE, "IDLE");
  timerRemaining = prefs.getULong(NVS_REMAINING, 0);
  timerDuration  = prefs.getULong(NVS_DURATION, 0);

  Serial.printf("[NVS]" " Loaded: state=%s, remaining=%ds\n",
    savedState.c_str(), timerRemaining);

  if (savedState == "RUNNING" || savedState == "PAUSED") {
    // ⚡ SOURCE OF TRUTH: Android App yg decide.
    // ESP32 cuma report state ke app. App akan:
    //   a) Kalo ada sesi aktif di app → resume timer
    //   b) Kalo nggak ada sesi → kirim CLEAR ke ESP32
    // Sementara: set TIMEOUT, tunjukin "----"
    currentState = TIMEOUT;
    saveToNVS();
    updateDisplay();
  } else if (savedState == "FINISHED") {
    currentState = FINISHED;
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    currentState = IDLE;
  }

  prefs.end();
  updateCharStatus();
}

void clearNVS() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  Serial.println("[NVS]" " Cleared");
}

String stateToString(TimerState s) {
  switch (s) {
    case IDLE:     return "IDLE";
    case RUNNING:  return "RUNNING";
    case PAUSED:   return "PAUSED";
    case FINISHED: return "FINISHED";
    case TIMEOUT:  return "TIMEOUT";
    default:       return "UNKNOWN";
  }
}

// ===== 4-Digit Display — MM:SS format =====

const uint8_t segDASH[] = { 0x40, 0x40, 0x40, 0x40 };  // ----
const uint8_t segZERO[] = { 0x3F, 0x3F, 0x3F, 0x3F };  // 0000
const uint8_t segBLANK[] = { 0x00, 0x00, 0x00, 0x00 };

void updateDisplay() {
  if (currentState == IDLE || currentState == TIMEOUT) {
    display.setSegments((uint8_t*)segDASH, 4, 0);
    return;
  }

  if (currentState == FINISHED) {
    static bool toggle = false;
    toggle = !toggle;
    if (toggle) {
      display.setSegments((uint8_t*)segZERO, 4, 0);
    } else {
      display.showNumberDecEx(0, 0b01000000, true);
    }
    return;
  }

  // RUNNING or PAUSED — show MM:SS
  int mins = timerRemaining / 60;
  int secs = timerRemaining % 60;
  if (mins > 99) mins = 99;
  
  int value = mins * 100 + secs;
  bool colonOn = (currentState == RUNNING);
  
  display.showNumberDecEx(value, colonOn ? 0b01000000 : 0, true);
}

// ===== BLE =====

#ifndef WOKWI_SIMULATION
void setupBLE() {
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharVal = pService->createCharacteristic(
    CHAR_TIMER_VALUE,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharVal->setCallbacks(new MyCharCallbacks());

  charStatus = pService->createCharacteristic(
    CHAR_TIMER_STATUS,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );

  BLECharacteristic *pCharCmd = pService->createCharacteristic(
    CHAR_COMMAND,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharCmd->setCallbacks(new MyCharCallbacks());

  BLECharacteristic *pCharName = pService->createCharacteristic(
    CHAR_DEVICE_NAME,
    BLECharacteristic::PROPERTY_READ
  );
  pCharName->setValue(DEVICE_NAME);

  charInfo = pService->createCharacteristic(
    CHAR_DEVICE_INFO,
    BLECharacteristic::PROPERTY_READ
  );

  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();

  updateCharStatus();
  Serial.println("[BLE]" " Ready. Nama: " + String(DEVICE_NAME));
}
#endif

void updateCharStatus() {
#ifndef WOKWI_SIMULATION
  if (!charStatus) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"s\":%lu,\"t\":\"%s\"}",
    timerRemaining, stateToString(currentState).c_str());
  charStatus->setValue((uint8_t*)buf, strlen(buf));
  charStatus->notify();
#endif
}
  // ===== BATTERY MONITORING =====
// Batre RC: LiPo 7.4V (2S)
// Divider: R1=100kΩ, R2=33kΩ → ratio = (100+33)/33 = 4.03
// ADC: 0-4095 → 0-3.3V
// Full: 8.4V, Low: 6.2V, Cut-off: 6.0V

#define ADC_DIVIDER_RATIO  4.03f
#define BATT_FULL          8.4f
#define BATT_LOW           6.2f
#define BATT_CUTOFF        6.0f

float readBatteryVoltage() {
  int adcVal = 0;
  // Average 16 readings for stability
  for (int i = 0; i < 16; i++) {
    adcVal += analogRead(ADC_POWER_PIN);
    delayMicroseconds(100);
  }
  adcVal /= 16;
  float voltage = (adcVal * 3.3f / 4095.0f) * ADC_DIVIDER_RATIO;
  return voltage;
}

void checkPower() {
  float voltage = readBatteryVoltage();

  // Update charInfo with voltage
  updateCharInfo();

  // Low battery — warn app
  if (voltage < BATT_LOW && voltage >= BATT_CUTOFF && currentState == RUNNING) {
    Serial.printf("[BATT]" " Low: %.2fV\n", voltage);
    // Don't pause yet — let app decide
    updateCharStatus();
  }

  // Critical battery — auto pause + notify
  if (voltage < BATT_CUTOFF && currentState == RUNNING) {
    Serial.printf("[BATT]" " CRITICAL: %.2fV — auto pause\n", voltage);
    pauseTimer();
    saveToNVS();
    updateCharStatus();
  }
}

void updateCharInfo() {
#ifndef WOKWI_SIMULATION
  if (!charInfo) return;
  float voltage = readBatteryVoltage();
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"d\":\"%s\",\"s\":\"%s\",\"r\":%lu,\"du\":%lu,\"v\":%.2f,\"u\":%lu}",
    DEVICE_NAME, stateToString(currentState).c_str(),
    timerRemaining, timerDuration, voltage, millis() / 1000);
  charInfo->setValue((uint8_t*)buf, strlen(buf));
#endif
}

// ===== SERIAL COMMAND BRIDGE =====
// Works in both Wokwi and production (useful for debugging)
// Commands: SET <sec>, START, STOP, PAUSE, RESET, CLEAR, STATUS

void processSerialCommand(String input) {
  input.trim();
  if (input.length() == 0) return;
  input.toUpperCase();

  Serial.printf("[Serial]" " Cmd: %s\n", input.c_str());

  if (input.startsWith("SET ")) {
    int val = input.substring(4).toInt();
    if (val > 0 && val <= 3600) {
      timerDuration = val;
      timerRemaining = val;
      updateDisplay();
      updateCharStatus();
      Serial.printf("[Timer]" " Duration set: %ds\n", val);
    } else {
      Serial.println("[Error]" " SET value must be 1-3600");
    }
  }
  else if (input == "START" && timerRemaining > 0) {
    startTimer();
  }
  else if (input == "STOP") {
    stopTimer();
  }
  else if (input == "PAUSE") {
    pauseTimer();
  }
  else if (input == "RESET") {
    resetTimer();
  }
  else if (input == "CLEAR") {
    clearNVS();
    resetTimer();
  }
  else if (input == "STATUS") {
    float voltage = readBatteryVoltage();
    Serial.printf("{\"device\":\"%s\",\"state\":\"%s\",\"remaining\":%lu,\"duration\":%lu,\"voltage\":%.2f,\"uptime\":%lu}\n",
      DEVICE_NAME, stateToString(currentState).c_str(),
      timerRemaining, timerDuration, voltage, millis() / 1000);
  }
  else {
    Serial.println("[Help]" " Commands: SET <1-3600>, START, STOP, PAUSE, RESET, CLEAR, STATUS");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000); // Wait for Serial Monitor connection
  Serial.println("\n=== EXCAVATOR TIMER RENTAL v2.1 ===");
#ifdef WOKWI_SIMULATION
  Serial.println("[MODE]" " Wokwi Simulation — BLE disabled, use Serial commands");
  Serial.println("[Help]" " Commands: SET <1-3600>, START, STOP, PAUSE, RESET, CLEAR, STATUS");
#endif

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_RESET, INPUT_PULLUP);
  pinMode(BTN_TEST, INPUT_PULLUP);
  pinMode(ADC_POWER_PIN, INPUT);

  // 4-Digit Display
  display.setBrightness(7);  // 0=min, 7=max
  display.setSegments((uint8_t*)segDASH, 4, 0);

  // NVS
  loadFromNVS();

  // BLE (disabled in Wokwi simulation)
#ifndef WOKWI_SIMULATION
  setupBLE();
#endif

  updateDisplay();
  Serial.println("[BOOT]" " Ready!");
}

// ===== LOOP =====
void loop() {
  // === Serial command bridge ===
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    processSerialCommand(cmd);
  }

  // === Timer countdown ===
  if (currentState == RUNNING) {
    unsigned long elapsed = (millis() - timerStartedAt) / 1000;

    if (elapsed >= timerDuration) {
      finishTimer();
    } else {
      timerRemaining = timerDuration - elapsed;

      unsigned long now = millis();
      if (now - lastSecondTick >= 1000) {
        lastSecondTick = now;
        updateDisplay();
        updateCharStatus();
      }
    }
  }

  // === Tombol fisik ===
  if (digitalRead(BTN_RESET) == LOW) {
    delay(50);
    if (digitalRead(BTN_RESET) == LOW) {
      resetTimer();
      digitalWrite(BUZZER_PIN, LOW);
      display.setSegments((uint8_t*)segDASH, 4, 0);
      delay(500);
    }
  }

  if (digitalRead(BTN_TEST) == LOW) {
    delay(50);
    if (digitalRead(BTN_TEST) == LOW) {
      digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
      delay(500);
    }
  }

  // === Power check tiap 5 detik ===
  static unsigned long lastPowerCheck = 0;
  if (millis() - lastPowerCheck >= 5000) {
    lastPowerCheck = millis();
    checkPower();
    updateCharInfo();
  }

  // === Buzzer mati otomatis setelah 30 detik (kalo FINISHED) ===
  if (currentState == FINISHED && digitalRead(BUZZER_PIN) == HIGH) {
    if (buzzerStartedAt == 0) buzzerStartedAt = millis();
    if (millis() - buzzerStartedAt >= 30000) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerStartedAt = 0;
      Serial.println("[Buzzer]" " Auto-off after 30s");
      saveToNVS();
    }
  } else if (currentState != FINISHED) {
    buzzerStartedAt = 0;  // reset when not in FINISHED state
  }

  delay(10);
}
