/*
 * Excavator Rental Timer - BLE Direct MVP
 *
 * Hardware:
 * - ESP32 / ESP32-C3 style module
 * - 1x 18650 shared battery
 * - 3.3V regulator for ESP32
 * - 3V/3.3V relay with transistor driver + flyback diode
 * - TM1637 4-digit display mounted on toy
 * - ADC voltage divider for battery status
 *
 * Safety:
 * - Relay is OFF on boot/reset.
 * - Timer is stored in NVS.
 * - Battery swap boots into PAUSED, never auto-runs.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <TM1637Display.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "mbedtls/md.h"

// ===== DEVICE CONFIG =====
static const char *TOY_ID = "EXC-01";
static const uint8_t TOY_NUMERIC_ID = 1;
static const char *DEVICE_SECRET = "replace-with-unique-secret";
static const bool ALLOW_UNSIGNED_DEBUG_COMMANDS = true;

// ===== PINS =====
static const uint8_t RELAY_PIN = 26;
static const uint8_t BATTERY_ADC_PIN = 34;
static const uint8_t DISPLAY_CLK_PIN = 18;
static const uint8_t DISPLAY_DIO_PIN = 19;
static const uint8_t STATUS_LED_PIN = 2;
static const uint8_t SERVICE_BUTTON_PIN = 14;

static const bool RELAY_ACTIVE_HIGH = true;

// ===== BLE UUIDS =====
static const char *SERVICE_UUID = "7b7d0001-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *STATE_CHAR_UUID = "7b7d0002-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *COMMAND_CHAR_UUID = "7b7d0003-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *ACK_CHAR_UUID = "7b7d0004-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *INFO_CHAR_UUID = "7b7d0005-8f2a-4f6b-9b2e-2f3ad5a10001";

// ===== STORAGE =====
static const char *NVS_NAMESPACE = "rental";
static const uint32_t STORAGE_MAGIC = 0xE7CA2026;
static const uint32_t SAVE_INTERVAL_MS = 5000;
static const uint32_t NOTIFY_INTERVAL_MS = 1000;
static const uint32_t ADVERTISE_REFRESH_MS = 2000;
static const uint32_t MAX_REMAINING_SECONDS = 3600;

// ===== BATTERY =====
static const float ADC_REFERENCE_V = 3.3f;
static const float ADC_DIVIDER_RATIO = 3.2f;  // R1=220k, R2=100k
static const float BATTERY_LOW_V = 3.55f;
static const float BATTERY_CRITICAL_V = 3.30f;
static const float BATTERY_CUTOFF_V = 3.15f;
static const uint8_t CUTOFF_CONFIRM_COUNT = 5;

enum RentalState : uint8_t {
  STATE_LOCKED = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED = 2,
  STATE_LOW_BATT = 3,
  STATE_ENDED = 4,
  STATE_FAULT = 5,
};

enum BatteryStatus : uint8_t {
  BAT_UNKNOWN = 0,
  BAT_OK = 1,
  BAT_LOW = 2,
  BAT_CRITICAL = 3,
};

enum FaultCode : uint8_t {
  FAULT_NONE = 0,
  FAULT_LOW_BATT_CUTOFF = 1,
  FAULT_STORAGE_CRC = 2,
  FAULT_RELAY_STUCK = 3,
  FAULT_COMMAND_AUTH = 4,
};

Preferences prefs;
TM1637Display display(DISPLAY_CLK_PIN, DISPLAY_DIO_PIN);

BLEServer *bleServer = nullptr;
BLEAdvertising *bleAdvertising = nullptr;
BLECharacteristic *stateChar = nullptr;
BLECharacteristic *ackChar = nullptr;
BLECharacteristic *infoChar = nullptr;

RentalState state = STATE_LOCKED;
BatteryStatus batteryStatus = BAT_UNKNOWN;
FaultCode faultCode = FAULT_NONE;

uint32_t remainingSeconds = 0;
uint32_t totalPaidSeconds = 0;
uint32_t lastCommandId = 0;
uint32_t lastAppliedCommandId = 0;
String sessionId = "";
String lastAckPayload = "";

uint8_t seq = 0;
uint8_t cutoffCount = 0;
bool bleConnected = false;

uint32_t lastTickMs = 0;
uint32_t lastSaveMs = 0;
uint32_t lastNotifyMs = 0;
uint32_t lastAdvertiseMs = 0;
uint32_t lastBatteryMs = 0;

const uint8_t SEG_LOCKED[] = { SEG_G, SEG_G, SEG_G, SEG_G };
const uint8_t SEG_LOW[] = {
  0,
  SEG_D | SEG_E | SEG_F,
  SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
  0
};
const uint8_t SEG_ERR[] = {
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_E | SEG_G,
  SEG_E | SEG_G,
  0
};

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

String batteryName(BatteryStatus value) {
  switch (value) {
    case BAT_OK: return "OK";
    case BAT_LOW: return "LOW";
    case BAT_CRITICAL: return "CRITICAL";
    default: return "UNKNOWN";
  }
}

String displayText() {
  if (state == STATE_LOCKED) return "----";
  if (state == STATE_LOW_BATT) return "Lo";
  if (state == STATE_ENDED) return "0000";
  if (state == STATE_FAULT) return "Err";

  uint32_t minutes = remainingSeconds / 60;
  uint32_t seconds = remainingSeconds % 60;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
  return String(buf);
}

uint32_t storageCrc() {
  uint32_t crc = STORAGE_MAGIC;
  crc ^= remainingSeconds * 2654435761UL;
  crc ^= totalPaidSeconds * 2246822519UL;
  crc ^= lastCommandId * 3266489917UL;
  crc ^= static_cast<uint8_t>(state) << 8;
  for (size_t i = 0; i < sessionId.length(); i++) {
    crc = (crc << 5) ^ (crc >> 2) ^ sessionId[i];
  }
  return crc;
}

void relayOff() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
}

void relayOn() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

bool canRelayRun() {
  return state == STATE_RUNNING &&
         remainingSeconds > 0 &&
         batteryStatus != BAT_CRITICAL &&
         faultCode == FAULT_NONE;
}

void applyRelay() {
  if (canRelayRun()) {
    relayOn();
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    relayOff();
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

void showRemaining(bool colon) {
  uint32_t minutes = min<uint32_t>(remainingSeconds / 60, 99);
  uint32_t seconds = remainingSeconds % 60;
  uint16_t value = minutes * 100 + seconds;
  display.showNumberDecEx(value, colon ? 0b01000000 : 0, true);
}

void updateDisplay() {
  static bool blink = false;
  blink = !blink;

  switch (state) {
    case STATE_LOCKED:
      display.setSegments(SEG_LOCKED);
      break;
    case STATE_RUNNING:
      showRemaining(true);
      break;
    case STATE_PAUSED:
      showRemaining(blink);
      break;
    case STATE_LOW_BATT:
      display.setSegments(blink ? SEG_LOW : SEG_LOCKED);
      break;
    case STATE_ENDED:
      if (blink) display.showNumberDecEx(0, 0, true);
      else display.clear();
      break;
    case STATE_FAULT:
      display.setSegments(SEG_ERR);
      break;
  }
}

float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }
  float raw = sum / 16.0f;
  float vadc = (raw / 4095.0f) * ADC_REFERENCE_V;
  return vadc * ADC_DIVIDER_RATIO;
}

void updateBatteryStatus(bool force = false) {
  static BatteryStatus previous = BAT_UNKNOWN;
  float voltage = readBatteryVoltage();

  if (voltage < BATTERY_CUTOFF_V) {
    cutoffCount = min<uint8_t>(cutoffCount + 1, CUTOFF_CONFIRM_COUNT);
  } else {
    cutoffCount = 0;
  }

  if (voltage < BATTERY_CRITICAL_V) batteryStatus = BAT_CRITICAL;
  else if (voltage < BATTERY_LOW_V) batteryStatus = BAT_LOW;
  else batteryStatus = BAT_OK;

  if (force || batteryStatus != previous) {
    seq++;
    previous = batteryStatus;
  }
}

void saveSession() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt("magic", STORAGE_MAGIC);
  prefs.putUInt("state", static_cast<uint32_t>(state));
  prefs.putUInt("rem", remainingSeconds);
  prefs.putUInt("paid", totalPaidSeconds);
  prefs.putUInt("lastcmd", lastCommandId);
  prefs.putString("sid", sessionId);
  prefs.putUInt("crc", storageCrc());
  prefs.end();
  lastSaveMs = millis();
}

void loadSession() {
  prefs.begin(NVS_NAMESPACE, true);
  uint32_t magic = prefs.getUInt("magic", 0);
  RentalState savedState = static_cast<RentalState>(prefs.getUInt("state", STATE_LOCKED));
  remainingSeconds = prefs.getUInt("rem", 0);
  totalPaidSeconds = prefs.getUInt("paid", 0);
  lastCommandId = prefs.getUInt("lastcmd", 0);
  sessionId = prefs.getString("sid", "");
  uint32_t savedCrc = prefs.getUInt("crc", 0);
  prefs.end();

  if (magic != STORAGE_MAGIC) {
    state = STATE_LOCKED;
    remainingSeconds = 0;
    totalPaidSeconds = 0;
    sessionId = "";
    lastCommandId = 0;
    return;
  }

  state = savedState;
  if (savedCrc != storageCrc()) {
    state = STATE_FAULT;
    faultCode = FAULT_STORAGE_CRC;
    remainingSeconds = 0;
    return;
  }

  if (remainingSeconds > 0) {
    if (savedState == STATE_LOW_BATT || batteryStatus == BAT_CRITICAL) {
      state = STATE_LOW_BATT;
      faultCode = FAULT_LOW_BATT_CUTOFF;
    } else {
      state = STATE_PAUSED;
      faultCode = FAULT_NONE;
    }
  } else {
    state = STATE_LOCKED;
    faultCode = FAULT_NONE;
  }
}

String hmacSha256(const String &message) {
  unsigned char hmac[32];
  char hex[65];

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char *>(DEVICE_SECRET), strlen(DEVICE_SECRET));
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>(message.c_str()), message.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  for (int i = 0; i < 32; i++) {
    snprintf(hex + (i * 2), 3, "%02x", hmac[i]);
  }
  hex[64] = '\0';
  return String(hex);
}

bool verifySignature(uint32_t commandId, const String &command, uint32_t value, const String &sid, const String &nonce, const String &signature) {
  if (ALLOW_UNSIGNED_DEBUG_COMMANDS && signature == "debug") {
    return true;
  }

  String canonical = String(TOY_ID) + "|" + String(commandId) + "|" + command + "|" + String(value) + "|" + sid + "|" + nonce;
  String expected = hmacSha256(canonical);
  return expected.equalsIgnoreCase(signature);
}

String buildStatePayload() {
  return "v1;toy=" + String(TOY_ID) +
         ";state=" + stateName(state) +
         ";rem=" + String(remainingSeconds) +
         ";disp=" + displayText() +
         ";paid=" + String(totalPaidSeconds) +
         ";bat=" + batteryName(batteryStatus) +
         ";fault=" + String(static_cast<uint8_t>(faultCode)) +
         ";seq=" + String(seq) +
         ";sid=" + sessionId;
}

std::string buildManufacturerData() {
  std::string data;
  data.push_back(static_cast<char>(0xFF)); // test company id low
  data.push_back(static_cast<char>(0xFF)); // test company id high
  data.push_back(static_cast<char>(1));    // protocol version
  data.push_back(static_cast<char>(TOY_NUMERIC_ID));
  data.push_back(static_cast<char>(state));
  data.push_back(static_cast<char>(min<uint32_t>(remainingSeconds / 60, 255)));
  data.push_back(static_cast<char>(batteryStatus));
  data.push_back(static_cast<char>(faultCode));
  data.push_back(static_cast<char>(seq));

  uint8_t flags = 0;
  if (bleConnected) flags |= 0x01;
  if (ALLOW_UNSIGNED_DEBUG_COMMANDS) flags |= 0x08;
  data.push_back(static_cast<char>(flags));
  data.push_back(static_cast<char>(remainingSeconds & 0xFF));
  data.push_back(static_cast<char>((remainingSeconds >> 8) & 0xFF));
  return data;
}

void refreshAdvertising() {
  if (!bleAdvertising) return;

  BLEAdvertisementData adv;
  adv.setName(TOY_ID);
  adv.setManufacturerData(buildManufacturerData());
  adv.setCompleteServices(BLEUUID(SERVICE_UUID));

  bleAdvertising->setAdvertisementData(adv);
  if (!bleConnected) {
    bleAdvertising->start();
  }
  lastAdvertiseMs = millis();
}

void publishState(bool force = false) {
  if (!stateChar) return;
  if (!force && millis() - lastNotifyMs < NOTIFY_INTERVAL_MS) return;

  String payload = buildStatePayload();
  stateChar->setValue(payload.c_str());
  if (bleConnected) {
    stateChar->notify();
  }
  lastNotifyMs = millis();
}

void sendAck(uint32_t commandId, bool ok, const String &code) {
  lastAckPayload = "v1;cmd=" + String(commandId) +
                   ";ok=" + String(ok ? 1 : 0) +
                   ";code=" + code +
                   ";state=" + stateName(state) +
                   ";rem=" + String(remainingSeconds);

  if (ackChar) {
    ackChar->setValue(lastAckPayload.c_str());
    if (bleConnected) {
      ackChar->notify();
    }
  }
}

void changeState(RentalState nextState) {
  if (state != nextState) {
    state = nextState;
    seq++;
  }
  applyRelay();
  updateDisplay();
  publishState(true);
  refreshAdvertising();
}

bool splitCommand(const String &input, String parts[], int expected) {
  int start = 0;
  int count = 0;
  while (count < expected) {
    int pos = input.indexOf('|', start);
    if (pos < 0) {
      parts[count++] = input.substring(start);
      break;
    }
    parts[count++] = input.substring(start, pos);
    start = pos + 1;
  }
  return count == expected;
}

void processCommand(const String &input) {
  String parts[7];
  if (!splitCommand(input, parts, 7) || parts[0] != "v1") {
    sendAck(0, false, "BAD_FORMAT");
    return;
  }

  uint32_t commandId = parts[1].toInt();
  String command = parts[2];
  command.toUpperCase();
  uint32_t value = parts[3].toInt();
  String sid = parts[4];
  String nonce = parts[5];
  String signature = parts[6];

  if (commandId == lastAppliedCommandId) {
    if (ackChar && lastAckPayload.length() > 0) {
      ackChar->setValue(lastAckPayload.c_str());
      if (bleConnected) ackChar->notify();
    }
    return;
  }

  if (commandId <= lastCommandId) {
    sendAck(commandId, false, "BAD_COUNTER");
    return;
  }

  if (!verifySignature(commandId, command, value, sid, nonce, signature)) {
    faultCode = FAULT_COMMAND_AUTH;
    sendAck(commandId, false, "BAD_SIGNATURE");
    publishState(true);
    return;
  }

  bool ok = false;
  String code = "OK";

  if (command == "ADD_TIME") {
    if (value == 0 || value % 300 != 0 || remainingSeconds + value > MAX_REMAINING_SECONDS) {
      code = "LIMIT";
    } else if (state == STATE_FAULT) {
      code = "FAULT";
    } else {
      remainingSeconds += value;
      totalPaidSeconds += value;
      if (sessionId.length() == 0) sessionId = sid;
      faultCode = FAULT_NONE;

      if (state == STATE_LOCKED || state == STATE_ENDED) {
        changeState(batteryStatus == BAT_CRITICAL ? STATE_LOW_BATT : STATE_RUNNING);
      } else if (state == STATE_LOW_BATT) {
        changeState(STATE_LOW_BATT);
      } else {
        changeState(state);
      }
      ok = true;
    }
  } else if (command == "PAUSE") {
    if (remainingSeconds > 0 && state == STATE_RUNNING) {
      changeState(STATE_PAUSED);
      ok = true;
    } else {
      code = "BAD_STATE";
    }
  } else if (command == "RESUME") {
    if (remainingSeconds == 0) {
      code = "BAD_STATE";
    } else if (batteryStatus == BAT_CRITICAL) {
      changeState(STATE_LOW_BATT);
      code = "LOW_BATT";
    } else {
      faultCode = FAULT_NONE;
      changeState(STATE_RUNNING);
      ok = true;
    }
  } else if (command == "STOP") {
    remainingSeconds = 0;
    totalPaidSeconds = 0;
    sessionId = "";
    faultCode = FAULT_NONE;
    changeState(STATE_LOCKED);
    ok = true;
  } else if (command == "CLEAR_FAULT") {
    if (state == STATE_FAULT || state == STATE_LOW_BATT) {
      faultCode = FAULT_NONE;
      changeState(remainingSeconds > 0 ? STATE_PAUSED : STATE_LOCKED);
      ok = true;
    } else {
      code = "BAD_STATE";
    }
  } else if (command == "GET_STATE") {
    publishState(true);
    ok = true;
  } else {
    code = "BAD_FORMAT";
  }

  if (ok) {
    lastCommandId = commandId;
    lastAppliedCommandId = commandId;
    saveSession();
  }

  sendAck(commandId, ok, code);
  publishState(true);
  refreshAdvertising();
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    seq++;
    refreshAdvertising();
    publishState(true);
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    seq++;
    refreshAdvertising();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue().c_str();
    processCommand(value);
  }
};

void setupBle() {
  BLEDevice::init(TOY_ID);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);

  stateChar = service->createCharacteristic(
    STATE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  stateChar->addDescriptor(new BLE2902());

  BLECharacteristic *commandChar = service->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  commandChar->setCallbacks(new CommandCallbacks());

  ackChar = service->createCharacteristic(
    ACK_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  ackChar->addDescriptor(new BLE2902());

  infoChar = service->createCharacteristic(
    INFO_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  infoChar->setValue((String("v1;toy=") + TOY_ID + ";fw=ble-direct-mvp;display=tm1637;relay=cheap").c_str());

  service->start();

  bleAdvertising = BLEDevice::getAdvertising();
  bleAdvertising->addServiceUUID(SERVICE_UUID);
  bleAdvertising->setScanResponse(true);
  refreshAdvertising();
}

void handleTimerTick() {
  if (state != STATE_RUNNING) return;

  uint32_t now = millis();
  if (now - lastTickMs < 1000) return;
  lastTickMs = now;

  if (remainingSeconds > 0) {
    remainingSeconds--;
  }

  if (remainingSeconds == 0) {
    changeState(STATE_ENDED);
    saveSession();
  } else if (now - lastSaveMs >= SAVE_INTERVAL_MS) {
    saveSession();
  }

  updateDisplay();
  publishState(false);
}

void handleBatteryCutoff() {
  if (state != STATE_RUNNING) return;
  if (cutoffCount < CUTOFF_CONFIRM_COUNT) return;

  faultCode = FAULT_LOW_BATT_CUTOFF;
  changeState(STATE_LOW_BATT);
  saveSession();
}

void handleSerialDebug() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toUpperCase();

  if (line == "+5") {
    processCommand("v1|" + String(lastCommandId + 1) + "|ADD_TIME|300|SERIAL|debug|debug");
  } else if (line == "+10") {
    processCommand("v1|" + String(lastCommandId + 1) + "|ADD_TIME|600|SERIAL|debug|debug");
  } else if (line == "PAUSE") {
    processCommand("v1|" + String(lastCommandId + 1) + "|PAUSE|0|SERIAL|debug|debug");
  } else if (line == "RESUME") {
    processCommand("v1|" + String(lastCommandId + 1) + "|RESUME|0|SERIAL|debug|debug");
  } else if (line == "STOP") {
    processCommand("v1|" + String(lastCommandId + 1) + "|STOP|0|SERIAL|debug|debug");
  } else if (line == "STATUS") {
    Serial.println(buildStatePayload());
  }
}

void setup() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(SERVICE_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(100);

  display.setBrightness(5);
  display.setSegments(SEG_LOCKED);

  analogReadResolution(12);
#ifdef ADC_11db
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#endif

  updateBatteryStatus(true);
  loadSession();
  applyRelay();
  updateDisplay();
  saveSession();

  setupBle();
  publishState(true);
  refreshAdvertising();

  Serial.println("Excavator BLE Direct MVP ready");
  Serial.println(buildStatePayload());
  lastTickMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - lastBatteryMs >= 1000) {
    lastBatteryMs = now;
    updateBatteryStatus();
    handleBatteryCutoff();
  }

  handleTimerTick();

  if (now - lastNotifyMs >= NOTIFY_INTERVAL_MS) {
    updateDisplay();
    publishState(false);
  }

  if (now - lastAdvertiseMs >= ADVERTISE_REFRESH_MS) {
    refreshAdvertising();
  }

  handleSerialDebug();
  delay(10);
}
