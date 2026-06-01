/*
 * Excavator Rental Timer - BLE Demo (Relay + Buzzer Only)
 * 
 * Hardware:
 * - ESP32
 * - Relay (Pin 26) - Active High/Low configurable
 * - Buzzer (Pin 27) - Active High
 * 
 * Behavior:
 * - Connects via BLE (Service: 7b7d0001-8f2a-4f6b-9b2e-2f3ad5a10001)
 * - Beeps on command receive.
 * - Long beep when time ends.
 * - No TM1637, No NVS, No Battery ADC (Mocked to BAT_OK)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===== CONFIG =====
static const char *TOY_ID = "EXC-01";
static const uint8_t TOY_NUMERIC_ID = 1;

// ===== PINS =====
static const uint8_t RELAY_PIN = 26;
static const uint8_t BUZZER_PIN = 27;
static const bool RELAY_ACTIVE_HIGH = true;

// ===== BLE UUIDS =====
static const char *SERVICE_UUID = "7b7d0001-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *STATE_CHAR_UUID = "7b7d0002-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *COMMAND_CHAR_UUID = "7b7d0003-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *ACK_CHAR_UUID = "7b7d0004-8f2a-4f6b-9b2e-2f3ad5a10001";
static const char *INFO_CHAR_UUID = "7b7d0005-8f2a-4f6b-9b2e-2f3ad5a10001";

enum RentalState : uint8_t {
  STATE_LOCKED = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED = 2,
  STATE_LOW_BATT = 3,
  STATE_ENDED = 4,
  STATE_FAULT = 5,
};

BLEServer *bleServer = nullptr;
BLEAdvertising *bleAdvertising = nullptr;
BLECharacteristic *stateChar = nullptr;
BLECharacteristic *ackChar = nullptr;

RentalState state = STATE_LOCKED;
uint32_t remainingSeconds = 0;
uint32_t totalPaidSeconds = 0;
uint32_t lastCommandId = 0;
String sessionId = "S-DEMO-001";
String lastAckPayload = "";

uint8_t seq = 0;
bool bleConnected = false;
uint32_t lastTickMs = 0;
uint32_t lastNotifyMs = 0;
uint32_t lastAdvertiseMs = 0;

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

String buildStatePayload() {
  return "v1;toy=" + String(TOY_ID) +
         ";state=" + stateName(state) +
         ";rem=" + String(remainingSeconds) +
         ";disp=" + String(remainingSeconds/60) + ":" + String(remainingSeconds%60) +
         ";paid=" + String(totalPaidSeconds) +
         ";bat=OK;fault=0;seq=" + String(seq) +
         ";sid=" + sessionId;
}

std::string buildManufacturerData() {
  std::string data;
  data.push_back(static_cast<char>(0xFF));
  data.push_back(static_cast<char>(0xFF));
  data.push_back(static_cast<char>(1)); // version
  data.push_back(static_cast<char>(TOY_NUMERIC_ID));
  data.push_back(static_cast<char>(state));
  data.push_back(static_cast<char>(min<uint32_t>(remainingSeconds / 60, 255)));
  data.push_back(static_cast<char>(1)); // BAT_OK
  data.push_back(static_cast<char>(0)); // FAULT_NONE
  data.push_back(static_cast<char>(seq));
  uint8_t flags = 0;
  if (bleConnected) flags |= 0x01;
  flags |= 0x08; // ALLOW_UNSIGNED_DEBUG
  data.push_back(static_cast<char>(flags));
  data.push_back(static_cast<char>(remainingSeconds & 0xFF));
  data.push_back(static_cast<char>((remainingSeconds >> 8) & 0xFF));
  return data;
}

void refreshAdvertising() {
  if (!bleAdvertising) return;
  BLEAdvertisementData adv;
  adv.setName(TOY_ID);
  std::string mfgData = buildManufacturerData();
  adv.setManufacturerData(String(mfgData.c_str(), mfgData.length()));
  adv.setCompleteServices(BLEUUID(SERVICE_UUID));
  bleAdvertising->setAdvertisementData(adv);
  if (!bleConnected) bleAdvertising->start();
  lastAdvertiseMs = millis();
}

void publishState() {
  if (!stateChar) return;
  String payload = buildStatePayload();
  stateChar->setValue(payload.c_str());
  if (bleConnected) stateChar->notify();
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
    if (bleConnected) ackChar->notify();
  }
}

void changeState(RentalState nextState) {
  if (state != nextState) {
    state = nextState;
    seq++;
  }
  applyRelay();
  publishState();
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
  
  if (commandId <= lastCommandId) {
    sendAck(commandId, false, "BAD_COUNTER");
    return;
  }

  bool ok = false;
  String code = "OK";

  beep(50, 1); // Beep on command

  if (command == "ADD_TIME") {
    remainingSeconds += value;
    totalPaidSeconds += value;
    if (state == STATE_LOCKED || state == STATE_ENDED) {
      changeState(STATE_RUNNING);
    } else {
      changeState(state);
    }
    ok = true;
  } else if (command == "PAUSE") {
    if (remainingSeconds > 0 && state == STATE_RUNNING) {
      changeState(STATE_PAUSED);
      ok = true;
    } else { code = "BAD_STATE"; }
  } else if (command == "RESUME") {
    if (remainingSeconds == 0) { code = "BAD_STATE"; }
    else { changeState(STATE_RUNNING); ok = true; }
  } else if (command == "STOP") {
    remainingSeconds = 0;
    changeState(STATE_LOCKED);
    ok = true;
  } else if (command == "GET_STATE") {
    publishState();
    ok = true;
  } else {
    code = "BAD_FORMAT";
  }

  if (ok) lastCommandId = commandId;
  sendAck(commandId, ok, code);
  publishState();
  refreshAdvertising();
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    seq++;
    refreshAdvertising();
    publishState();
  }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    seq++;
    refreshAdvertising();
    server->startAdvertising(); // restart advertising immediately
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue().c_str();
    processCommand(value);
  }
};

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  applyRelay();

  BLEDevice::init(TOY_ID);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  stateChar = service->createCharacteristic(STATE_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  stateChar->addDescriptor(new BLE2902());

  BLECharacteristic *commandChar = service->createCharacteristic(COMMAND_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  commandChar->setCallbacks(new CommandCallbacks());

  ackChar = service->createCharacteristic(ACK_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  ackChar->addDescriptor(new BLE2902());

  BLECharacteristic *infoChar = service->createCharacteristic(INFO_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
  infoChar->setValue("v1;toy=EXC-01;fw=demo-relay-buzzer");

  service->start();
  bleAdvertising = BLEDevice::getAdvertising();
  bleAdvertising->addServiceUUID(SERVICE_UUID);
  bleAdvertising->setScanResponse(true);
  refreshAdvertising();

  Serial.println("Demo BLE Relay+Buzzer Started.");
  beep(100, 2); // 2 short beeps on boot
}

void loop() {
  uint32_t now = millis();

  if (state == STATE_RUNNING && (now - lastTickMs >= 1000)) {
    lastTickMs = now;
    if (remainingSeconds > 0) {
      remainingSeconds--;
    }
    if (remainingSeconds == 0) {
      changeState(STATE_ENDED);
      beep(1000, 1); // Long beep when ended
    }
    publishState();
  } else if (state != STATE_RUNNING) {
    lastTickMs = now; // keep it synced so it doesn't instantly tick when resumed
  }

  if (now - lastNotifyMs >= 1000) {
    publishState();
  }

  if (now - lastAdvertiseMs >= 2000) {
    refreshAdvertising();
  }
  
  delay(10);
}
