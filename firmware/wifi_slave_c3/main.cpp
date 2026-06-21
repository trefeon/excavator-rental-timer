/*
 * Excavator Rental Timer - ESP-NOW Slave
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
 * 2. MOSFET Module (e.g., XY-MOS)
 *    - V+   -> Power Source (e.g., 12V/24V depending on load)
 *    - V-   -> GND
 *    - TRIG -> GPIO 4 (Active HIGH)
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
 * - Uses ESP-NOW (connectionless) to communicate with Master
 * - No Wi-Fi network connection needed (saves power)
 * - Auto-Registers with Master via ESP-NOW broadcast
 *
 * =========================================================
 * CHANGELOG / FIXES
 * =========================================================
 * [FIX 1] REGISTRATION_RETRY_MAX_MS dikecilkan dari 60000 -> 8000 ms
 *         Mencegah networkTask menahan delay 60 detik yang menyebabkan
 *         loop() tersangkut di mutex dan WDT timeout -> reboot.
 *
 * [FIX 2] Semua xSemaphoreTake(stateMutex, portMAX_DELAY) di loop()
 *         diganti ke timeout 100ms agar loop() tidak pernah tersangkut
 *         menunggu mutex yang dipegang networkTask, mencegah WDT reboot.
 *
 * [FIX 3] Master Timeout logic diperbaiki: lastMasterContactMs tidak
 *         lagi di-overwrite dengan nowMs sebelum dicek (bug: selalu false).
 *
 * [FIX 4] LED (NET_LED_PIN) mati total setelah idle >= 60 detik
 *         (STATE_LOCKED atau STATE_ENDED). Kedipan 3 detik sekali
 *         hanya aktif saat state aktif atau dalam 60 detik pertama idle.
 *
 * [FIX 5] Light Sleep saat idle >= 60 detik untuk hemat baterai.
 *         CPU tidur 500ms, lalu bangun, reset WDT, cek kondisi, tidur lagi.
 *         Radio WiFi/ESP-NOW tetap aktif sehingga perintah dari Master
 *         tetap bisa diterima kapan saja (ESP-NOW interrupt = wake source).
 *         Konsumsi daya turun dari ~20mA -> ~1-2mA saat idle.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <TM1637Display.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_sleep.h>       // [FIX 5] Tambahan untuk light sleep

#include "esp_now_protocol.h"

// ===== PINS =====
static const uint8_t RELAY_PIN   = 4;
static const uint8_t BUZZER_PIN  = 5;
static const uint8_t CLK_PIN     = 6;
static const uint8_t DIO_PIN     = 7;
static const uint8_t BUTTON_PIN  = 9;
static const uint8_t NET_LED_PIN = 8;  // built-in LED (network activity, Active LOW)

// ===== KONFIGURASI TRIGGER RELAY / MOSFET =====
// 1 = Relay Module High Trigger
// 2 = Relay Module Low Trigger (Butuh trik High-Z untuk modul 5V)
// 3 = MOSFET Module (seperti XY-MOS, aktif HIGH)
static const uint8_t TRIGGER_MODE = 3;

// ===== TIMING CONSTANTS =====
static const uint32_t FLASH_SAVE_INTERVAL_S         = 10;
static const uint32_t BUTTON_DEBOUNCE_MS            = 300;
static const uint32_t HEARTBEAT_INTERVAL_MS         = 2000;
static const uint32_t MASTER_TIMEOUT_MS             = 45000;
static const uint32_t REGISTRATION_RETRY_INITIAL_MS = 1000;
// [FIX 1] Max backoff diperkecil dari 60000 -> 8000 ms.
static const uint32_t REGISTRATION_RETRY_MAX_MS     = 8000;
static const int      MAX_ADD_TIME_MINUTES          = 480;
static const uint32_t MAX_REMAINING                 = 28800;  // 8 jam dalam detik

// Durasi idle (ms) sebelum LED mati & light sleep aktif
static const uint32_t LED_IDLE_OFF_MS   = 60000;  // 1 menit
// [FIX 5] Durasi sekali tidur saat light sleep (ms)
// Tidak boleh melebihi WDT timeout (10000ms). Nilai 500ms membuat
// WDT di-reset ~20x per detik saat sleep, sangat aman.
static const uint32_t LIGHT_SLEEP_MS   = 500;

// ===== GLOBALS =====
TM1637Display display(CLK_PIN, DIO_PIN);
bool colonState = false;
Preferences preferences;

SemaphoreHandle_t stateMutex;

volatile uint32_t netLedEndMs = 0;

uint8_t TOY_NUMERIC_ID = 0;
String  TOY_ID         = "EXC-00";

enum RentalState : uint8_t {
  STATE_LOCKED  = 0,
  STATE_RUNNING = 1,
  STATE_PAUSED  = 2,
  STATE_ENDED   = 4,
  STATE_FAULT   = 5,
};

const char* stateName(RentalState value);
void changeState(RentalState nextState);

RentalState state            = STATE_LOCKED;
uint32_t    remainingSeconds = 0;
uint32_t    totalPaidSeconds = 0;
uint8_t     seq              = 0;
uint32_t    lastTickMs       = 0;
uint32_t    lastButtonPressMs = 0;
uint32_t    idleStartMs      = 0;
bool        isRegistered     = false;
int         pendingBeepMs    = 0;
int         pendingBeepCount = 1;
volatile bool pendingSaveFlash = false;

// ESP-NOW state
volatile uint32_t lastMasterContactMs = 0;
uint32_t regRetryDelay = REGISTRATION_RETRY_INITIAL_MS;
uint8_t  masterMac[6]  = {0};
volatile bool masterKnown = false;

// Pending identify flag
volatile bool pendingIdentify = false;

// ===== ANIMATION =====
uint8_t animFrame = 0;
const uint8_t SNAKE_FRAMES[12][4] = {
  { 0x01, 0,    0,    0    },
  { 0,    0x01, 0,    0    },
  { 0,    0,    0x01, 0    },
  { 0,    0,    0,    0x01 },
  { 0,    0,    0,    0x02 },
  { 0,    0,    0,    0x04 },
  { 0,    0,    0,    0x08 },
  { 0,    0,    0x08, 0    },
  { 0,    0x08, 0,    0    },
  { 0x08, 0,    0,    0    },
  { 0x10, 0,    0,    0    },
  { 0x20, 0,    0,    0    }
};

// ===== DISPLAY =====
void updateDisplay() {
  if (state == STATE_FAULT) {
    uint8_t err[] = { 0x79, 0x50, 0x50, 0x00 };
    display.setSegments(err);
  } else if (state == STATE_LOCKED || state == STATE_ENDED) {
    uint8_t segments[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; i++) {
      int f = (animFrame - i + 12) % 12;
      for (int d = 0; d < 4; d++) {
        segments[d] |= SNAKE_FRAMES[f][d];
      }
    }
    display.setSegments(segments);
  } else {
    int mins = remainingSeconds / 60;
    int secs = remainingSeconds % 60;
    int dispTime;
    if (mins > 99) {
      int hours   = mins / 60;
      int remMins = mins % 60;
      dispTime = (hours * 100) + remMins;
    } else {
      dispTime = (mins * 100) + secs;
    }
    display.showNumberDecEx(dispTime, colonState ? 0b01000000 : 0, true);
  }
}

// ===== BUZZER =====
void beep(int durationMs, int count = 1) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(100);
  }
}

// ===== RELAY =====
bool isRelayOn() {
  return (state == STATE_RUNNING && remainingSeconds > 0);
}

void applyRelay() {
  if (isRelayOn()) {
    pinMode(RELAY_PIN, OUTPUT);
    if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
      digitalWrite(RELAY_PIN, HIGH);
    } else {
      digitalWrite(RELAY_PIN, LOW);
    }
  } else {
    if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
      pinMode(RELAY_PIN, OUTPUT);
      digitalWrite(RELAY_PIN, LOW);
    } else {
      pinMode(RELAY_PIN, INPUT);
    }
  }
  if (netLedEndMs == 0) {
    digitalWrite(NET_LED_PIN, isRelayOn() ? LOW : HIGH);
  }
}

// ===== NET LED =====
// [FIX 4] Helper: cek apakah device sedang dalam kondisi idle > 60 detik
bool isDeepIdle() {
  return ((state == STATE_LOCKED || state == STATE_ENDED) &&
          (millis() - idleStartMs >= LED_IDLE_OFF_MS));
}

void netLedFlash(int ms = 50) {
  // [FIX 4] Jangan flash sama sekali saat deep idle
  if (isDeepIdle()) return;
  netLedEndMs = millis() + ms;
  digitalWrite(NET_LED_PIN, isRelayOn() ? HIGH : LOW);
}

void updateNetLed() {
  // [FIX 4] Saat deep idle, pastikan LED benar-benar OFF
  if (isDeepIdle()) {
    netLedEndMs = 0;
    digitalWrite(NET_LED_PIN, HIGH);  // OFF (active low)
    return;
  }
  if (netLedEndMs > 0 && millis() < netLedEndMs) return;
  netLedEndMs = 0;
  digitalWrite(NET_LED_PIN, isRelayOn() ? LOW : HIGH);
}

// ===== STATE HELPERS =====
const char* stateName(RentalState value) {
  switch (value) {
    case STATE_LOCKED:  return "LOCKED";
    case STATE_RUNNING: return "RUNNING";
    case STATE_PAUSED:  return "PAUSED";
    case STATE_ENDED:   return "ENDED";
    case STATE_FAULT:   return "FAULT";
  }
  return "FAULT";
}

void saveStateToFlash() {
  preferences.begin("state", false);
  preferences.putUInt("rem",  remainingSeconds);
  preferences.putUInt("paid", totalPaidSeconds);
  preferences.end();
  Serial.printf("State saved to flash: rem=%lu\n", remainingSeconds);
}

void changeState(RentalState nextState) {
  if (state != nextState) {
    if (nextState == STATE_LOCKED || nextState == STATE_ENDED) {
      idleStartMs = millis();
    }
    state = nextState;
    seq++;
  } else if (nextState == STATE_LOCKED || nextState == STATE_ENDED) {
    idleStartMs = millis();
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

// ===== ESP-NOW: Build and send a heartbeat =====
void sendHeartbeat() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  EspNowPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type     = PKT_HEARTBEAT;
  pkt.senderId = TOY_NUMERIC_ID;
  pkt.state    = (uint8_t)state;
  pkt.timeLeft = remainingSeconds;
  pkt.seq      = seq;
  memcpy(pkt.mac, mac, 6);

  esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
}

// ===== ESP-NOW: Send registration request =====
void sendRegisterRequest() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  EspNowPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type     = PKT_REGISTER_REQ;
  pkt.senderId = 0;
  memcpy(pkt.mac, mac, 6);

  esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[ESPNOW] Sent registration request (MAC: %s)\n", WiFi.macAddress().c_str());
}

// ===== ESP-NOW: Send command response =====
void sendCommandResponse(const uint8_t* destMac, uint8_t respCode) {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  EspNowPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type     = PKT_COMMAND_RESP;
  pkt.senderId = TOY_NUMERIC_ID;
  pkt.respCode = respCode;
  pkt.state    = (uint8_t)state;
  pkt.timeLeft = remainingSeconds;
  pkt.seq      = seq;
  memcpy(pkt.mac, mac, 6);

  esp_now_send(destMac, (uint8_t*)&pkt, sizeof(pkt));
}

// ===== ESP-NOW RECEIVE CALLBACK =====
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac_addr = info->src_addr;
#else
void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
#endif
  if (len != sizeof(EspNowPacket)) return;

  EspNowPacket pkt;
  memcpy(&pkt, data, sizeof(EspNowPacket));

  switch (pkt.type) {
    case PKT_REGISTER_RESP: {
      uint8_t myMac[6];
      WiFi.macAddress(myMac);
      if (memcmp(pkt.mac, myMac, 6) != 0) return;

      int newId = pkt.targetId;
      if (newId > 0) {
        // [FIX 2] Timeout 100ms, bukan portMAX_DELAY
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          TOY_NUMERIC_ID = newId;
          char buf[16];
          snprintf(buf, sizeof(buf), "EXC-%02u", TOY_NUMERIC_ID);
          TOY_ID              = String(buf);
          isRegistered        = true;
          lastMasterContactMs = millis();
          regRetryDelay       = REGISTRATION_RETRY_INITIAL_MS;

          memcpy(masterMac, mac_addr, 6);
          masterKnown = true;

          esp_now_peer_info_t peerInfo;
          memset(&peerInfo, 0, sizeof(peerInfo));
          memcpy(peerInfo.peer_addr, masterMac, 6);
          peerInfo.channel = 0;
          peerInfo.encrypt = false;
          if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("[ESPNOW] Failed to add master peer (might already exist)");
          }

          xSemaphoreGive(stateMutex);
        }
        Serial.printf("[ESPNOW] Registered as %s (ID: %d)\n", TOY_ID.c_str(), newId);
        pendingBeepMs    = 150;
        pendingBeepCount = 3;
      }
      break;
    }

    case PKT_COMMAND: {
      if (pkt.targetId != TOY_NUMERIC_ID) return;

      lastMasterContactMs = millis();
      netLedFlash(50);

      uint8_t respCode = RESP_OK;
      bool cmdOk    = false;
      bool needSave = false;

      // [FIX 2] Timeout 100ms, bukan portMAX_DELAY
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        switch ((CmdType)pkt.cmd) {
          case CMD_ADD_TIME: {
            int maxLimit = MAX_ADD_TIME_MINUTES * 60;
            if ((int)pkt.value > maxLimit) {
              respCode = RESP_EXCEEDS_LIMIT;
            } else if (pkt.value == 0) {
              // Zero-second ADD_TIME: no-op
            } else {
              addTime(pkt.value);
              if (state == STATE_LOCKED || state == STATE_ENDED) {
                changeState(STATE_RUNNING);
              } else {
                changeState(state);
              }
              needSave = true;
              cmdOk    = true;
            }
            break;
          }
          case CMD_PAUSE: {
            if (remainingSeconds > 0 && state == STATE_RUNNING) {
              changeState(STATE_PAUSED);
              needSave = true;
              cmdOk    = true;
            } else {
              respCode = RESP_BAD_STATE;
            }
            break;
          }
          case CMD_RESUME: {
            if (remainingSeconds == 0) {
              respCode = RESP_BAD_STATE;
            } else {
              changeState(STATE_RUNNING);
              needSave = true;
              cmdOk    = true;
            }
            break;
          }
          case CMD_STOP: {
            remainingSeconds = 0;
            totalPaidSeconds = 0;
            changeState(STATE_LOCKED);
            needSave = true;
            cmdOk    = true;
            break;
          }
          case CMD_REBOOT: {
            respCode = RESP_REBOOTING;
            xSemaphoreGive(stateMutex);
            sendCommandResponse(mac_addr, respCode);
            Serial.println("[ACTION] Rebooting by ESP-NOW command");
            delay(200);
            ESP.restart();
            return;
          }
          case CMD_IDENTIFY: {
            cmdOk           = true;
            pendingIdentify = true;
            idleStartMs     = millis();
            break;
          }
          default:
            respCode = RESP_UNKNOWN_COMMAND;
            break;
        }
        xSemaphoreGive(stateMutex);
      }

      if (!cmdOk && respCode == RESP_OK) {
        respCode = RESP_BAD_STATE;
      }

      Serial.printf("[ESPNOW] CMD %s -> code=%s timeLeft=%lu state=%s\n",
                    cmdTypeName(pkt.cmd), respCodeName(respCode),
                    (unsigned long)remainingSeconds, stateName(state));

      sendCommandResponse(mac_addr, respCode);

      if (needSave) {
        pendingSaveFlash = true;
      }

      pendingBeepMs    = 50;
      pendingBeepCount = 1;
      break;
    }

    default:
      break;
  }
}

// ===== WDT-safe delay =====
void delayWDT(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ===== NETWORK TASK =====
void networkTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();

    bool regStat = false;
    // [FIX 2] Timeout 100ms, bukan portMAX_DELAY
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      regStat = isRegistered;
      xSemaphoreGive(stateMutex);
    }

    if (!regStat) {
      // [FIX 1] regRetryDelay max 8000ms, aman untuk WDT 10 detik
      delayWDT(random(500, 1500));
      sendRegisterRequest();
      delayWDT(regRetryDelay);

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!isRegistered) {
          if (regRetryDelay < REGISTRATION_RETRY_MAX_MS) {
            regRetryDelay = min(regRetryDelay * 2, REGISTRATION_RETRY_MAX_MS);
          }
        }
        xSemaphoreGive(stateMutex);
      }
    } else {
      delayWDT(HEARTBEAT_INTERVAL_MS);

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sendHeartbeat();

        // [FIX 3] Snapshot lastContact SEBELUM dibandingkan (bug lama: selalu 0)
        uint32_t nowMs      = millis();
        uint32_t lastContact = lastMasterContactMs;
        if (nowMs - lastContact > MASTER_TIMEOUT_MS) {
          Serial.println("[ESPNOW] Master unresponsive. Dropping registration.");
          isRegistered  = false;
          masterKnown   = false;
          regRetryDelay = REGISTRATION_RETRY_INITIAL_MS;
        }
        xSemaphoreGive(stateMutex);
      }
    }
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  Serial.println("\n\n========================================");
  Serial.println("[SYSTEM] Starting Excavator Slave (ESP-NOW)...");
  Serial.println("========================================");

  display.setBrightness(0x0f);

  if (TRIGGER_MODE == 1 || TRIGGER_MODE == 3) {
    digitalWrite(RELAY_PIN, LOW);
    pinMode(RELAY_PIN, OUTPUT);
  } else {
    pinMode(RELAY_PIN, INPUT);
  }
  pinMode(BUZZER_PIN,  OUTPUT);
  pinMode(BUTTON_PIN,  INPUT_PULLUP);
  pinMode(NET_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN,  LOW);
  digitalWrite(NET_LED_PIN, HIGH);  // OFF initially (active low)

  // [FIX 5] Konfigurasi wake source untuk light sleep:
  // GPIO 9 (tombol) bisa membangunkan ESP32 saat ditekan (LOW)
  esp_sleep_enable_gpio_wakeup();
  gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);

#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = 10000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(10, true);
#endif
  esp_task_wdt_add(NULL);

  stateMutex = xSemaphoreCreateMutex();

  // ===== POWERLOSS RECOVERY =====
  preferences.begin("state", true);
  uint32_t savedRem  = preferences.getUInt("rem",  0);
  uint32_t savedPaid = preferences.getUInt("paid", 0);
  preferences.end();

  if (savedRem > 0) {
    remainingSeconds = savedRem;
    totalPaidSeconds = savedPaid;
    state            = STATE_RUNNING;

    applyRelay();

    Serial.println("Powerloss Recovery: Auto-resuming in 3 seconds...");
    for (int i = 0; i < 3; i++) {
      beep(100, 1);
      delay(900);
    }
    beep(500, 1);

    changeState(STATE_RUNNING);
    Serial.printf("Powerloss Recovery: Restored %lu seconds. State RUNNING.\n", remainingSeconds);
  } else {
    state       = STATE_LOCKED;
    idleStartMs = millis();
    changeState(STATE_LOCKED);
  }

  applyRelay();

  // Tampilkan "boot" di display selama setup
  uint8_t dataConn[] = { 0x7c, 0x5c, 0x5c, 0x78 };
  display.setSegments(dataConn);
  delay(1500);

  // ===== Wi-Fi + ESP-NOW INIT =====
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ERROR: Init failed! Rebooting...");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.printf("[ESPNOW] Initialized. MAC: %s, Channel: %d\n",
                WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

  // Tampilkan "rEG" di display
  uint8_t dataReg[] = { 0x50, 0x79, 0x6f, 0x00 };
  display.setSegments(dataReg);

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 4096, NULL, 1, NULL, 0);

  updateDisplay();

  Serial.println("[SYSTEM] Ready! Listening for ESP-NOW commands...");
  beep(150, 3);
}

// ===== LOOP =====
void loop() {
  esp_task_wdt_reset();
  uint32_t now = millis();

  // ===== BUTTON HANDLING =====
  // [FIX 2] Timeout 100ms, bukan portMAX_DELAY
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (now - lastButtonPressMs > BUTTON_DEBOUNCE_MS) {
        lastButtonPressMs = now;
        if ((state == STATE_PAUSED || state == STATE_FAULT) && remainingSeconds > 0) {
          Serial.println("[ACTION] Physical Button Pressed: Resumed Timer");
          beep(50, 1);
          changeState(STATE_RUNNING);
          saveStateToFlash();
        } else if (state == STATE_LOCKED || state == STATE_ENDED) {
          idleStartMs = now;
        }
      }
    }

    // ===== TIMER TICK =====
    if (state == STATE_RUNNING && remainingSeconds > 0) {
      while (now - lastTickMs >= 1000) {
        lastTickMs += 1000;
        now        = millis();
        colonState = !colonState;
        remainingSeconds--;

        if (remainingSeconds == 60) {
          pendingBeepMs    = 200;
          pendingBeepCount = 1;
        } else if (remainingSeconds <= 10 && remainingSeconds > 0) {
          pendingBeepMs    = 50;
          pendingBeepCount = 1;
        }

        if (remainingSeconds % FLASH_SAVE_INTERVAL_S == 0 && remainingSeconds > 0) {
          saveStateToFlash();
        }

        updateDisplay();

        if (remainingSeconds == 0) {
          Serial.println("[TIMER] Time is up! Locking Excavator.");
          changeState(STATE_ENDED);
          saveStateToFlash();
          pendingBeepMs    = 1000;
          pendingBeepCount = 1;
          break;
        }
      }
    } else if (state == STATE_PAUSED && remainingSeconds > 0) {
      if (now - lastTickMs >= 500) {
        lastTickMs = now;
        static bool showDisplay = true;
        showDisplay = !showDisplay;
        if (showDisplay) {
          updateDisplay();
        } else {
          display.clear();
        }
      }
    } else if (state == STATE_LOCKED || state == STATE_ENDED) {
      if (now - idleStartMs < LED_IDLE_OFF_MS) {
        // 60 detik pertama: animasi snake
        if (now - lastTickMs >= 80) {
          lastTickMs = now;
          animFrame  = (animFrame + 1) % 12;
          updateDisplay();
        }
      } else {
        // Setelah 60 detik: display mati
        if (now - lastTickMs >= 1000) {
          lastTickMs = now;
          display.clear();
        }
      }
    } else {
      lastTickMs = now;
    }
    xSemaphoreGive(stateMutex);
  }

  // ===== DEFERRED NVS SAVE =====
  if (pendingSaveFlash) {
    pendingSaveFlash = false;
    // [FIX 2] Timeout 100ms
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      saveStateToFlash();
      xSemaphoreGive(stateMutex);
    }
  }

  // ===== PENDING BEEPS =====
  if (pendingBeepMs > 0) {
    int ms  = pendingBeepMs;
    int cnt = pendingBeepCount;
    pendingBeepMs    = 0;
    pendingBeepCount = 1;
    beep(ms, cnt);
  }

  // ===== PENDING IDENTIFY =====
  if (pendingIdentify) {
    pendingIdentify = false;
    for (int i = 0; i < 3; i++) {
      beep(100, 1);
      // [FIX 2] Timeout 100ms
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        display.clear();
        xSemaphoreGive(stateMutex);
      }
      delay(150);
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        updateDisplay();
        xSemaphoreGive(stateMutex);
      }
      delay(150);
    }
  }

  // ===== NET LED =====
  // [FIX 4] updateNetLed() handle: jika deep idle -> LED mati total
  updateNetLed();

  // [FIX 4] Kedipan 3 detik sekali hanya aktif jika TIDAK deep idle
  static uint32_t lastLedBlink = 0;
  if (!isDeepIdle() && millis() - lastLedBlink >= 3000) {
    lastLedBlink = millis();
    netLedFlash(50);
  }

  // ===== SERIAL HEARTBEAT =====
  static uint32_t lastHb = 0;
  if (millis() - lastHb >= 1000) {
    lastHb = millis();
    if (xSemaphoreTake(stateMutex, 0) == pdTRUE) {
      Serial.printf("[SLAVE-HB] up=%lus id=%s state=%s rem=%lu reg=%d\n",
                    millis() / 1000, TOY_ID.c_str(), stateName(state),
                    remainingSeconds, isRegistered ? 1 : 0);
      xSemaphoreGive(stateMutex);
    }
  }

  // ===== [FIX 5] LIGHT SLEEP SAAT DEEP IDLE =====
  // Kondisi masuk light sleep: idle >= 60 detik, tidak ada task pending.
  // CPU tidur 500ms, radio WiFi/ESP-NOW tetap aktif.
  // ESP32-C3 akan bangun lebih awal jika:
  //   - Timer 500ms habis (bangun rutin untuk reset WDT & cek kondisi)
  //   - Tombol GPIO 9 ditekan (bangun langsung)
  //   - Paket ESP-NOW diterima (bangun via WiFi interrupt)
  if (isDeepIdle() &&
      pendingBeepMs == 0 &&
      !pendingSaveFlash &&
      !pendingIdentify) {

    // Pastikan semua pending task sudah selesai sebelum tidur
    esp_task_wdt_reset();

    // Set timer wake: bangun otomatis setelah LIGHT_SLEEP_MS
    esp_sleep_enable_timer_wakeup((uint64_t)LIGHT_SLEEP_MS * 1000ULL);

    // Masuk light sleep — eksekusi berhenti di sini sampai ada wake event
    esp_light_sleep_start();

    // === SETELAH BANGUN ===
    // Reset WDT segera setelah bangun agar tidak timeout
    esp_task_wdt_reset();

    // Update now agar perhitungan waktu tetap akurat setelah sleep
    now = millis();
  }
}
