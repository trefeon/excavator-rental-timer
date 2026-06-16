/*
 * Excavator Rental Timer - Wi-Fi Master (ESP-NOW Gateway)
 * 
 * Architecture:
 * - Acts as an Access Point (SSID: ExcavatorMaster) for Android App
 * - Uses ESP-NOW to communicate with Slave devices (no Wi-Fi connection needed)
 * - Android App connects via Wi-Fi and uses HTTP API (unchanged)
 * - Master translates HTTP commands to ESP-NOW packets for slaves
 * - Registry logic (MAC -> ID mapping) stored in Preferences
 * - DNSServer captive portal keeps Android Wi-Fi connection stable
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include "index_html.h"

#include <esp_idf_version.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <esp_now.h>

#include "esp_now_protocol.h"

// ===== CONFIG =====
#define DEMO_MODE 1
static const char* AP_SSID = "ExcavatorMaster";
static const char* AP_PASS = "12345678";

WebServer server(80);
Preferences preferences;
SemaphoreHandle_t slavesMutex = NULL;

// ===== TIMING =====
static const uint32_t ONLINE_THRESHOLD_MS = 30000;
static const uint32_t MUTEX_TIMEOUT_TICKS = pdMS_TO_TICKS(500);
static const uint32_t CMD_RESPONSE_TIMEOUT_MS = 500;

// ===== DNS =====
const byte DNS_PORT = 53;
DNSServer dnsServer;

// ===== LED =====
#define LED_PIN 2

void addCorsHeaders();
void handleOptions();

uint32_t ledOffTime = 0;

void netLedFlash(int ms = 50) {
  digitalWrite(LED_PIN, HIGH);  // ON (Standard ESP32 is Active High)
  ledOffTime = millis() + ms;
}

void updateLed() {
  if (ledOffTime > 0 && millis() >= ledOffTime) {
    digitalWrite(LED_PIN, LOW); // OFF
    ledOffTime = 0;
  }
}

// ===== SLAVE REGISTRY =====
struct SlaveRecord {
  String mac;
  int id;
  String ip;         // empty for ESP-NOW slaves, kept for API compatibility
  uint32_t lastSeen;
  String state;      // "RUNNING", "LOCKED", "PAUSED", "ENDED"
  int time_left;
  String battery;
  uint8_t rawMac[6]; // raw MAC bytes for ESP-NOW addressing
};
SlaveRecord slaves[50];
int slaveCount = 0;

// ===== COMMAND RESPONSE SYNCHRONIZATION =====
// Used to wait for a slave's ESP-NOW response after sending a command
SemaphoreHandle_t cmdResponseSem = NULL;
volatile bool cmdResponseReceived = false;
EspNowPacket cmdResponsePkt;
portMUX_TYPE cmdResponseMux = portMUX_INITIALIZER_UNLOCKED;

// ===== HELPERS =====
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

String getMacKey(const String& m) {
  String k = m;
  k.replace(":", "");
  return k;
}

String macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool isValidMac(const String& mac) {
  if (mac.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    char c = mac.charAt(i);
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else {
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    }
  }
  return true;
}

// ===== REGISTRY LOGIC =====
// Assigns an ID for a given MAC address. Returns the assigned ID (or 0 if full).
int assignIdForMac(const String& mac) {
  int assignedId = 0;

  // Check if already in Preferences
  preferences.begin("registry", false);
  assignedId = preferences.getInt(getMacKey(mac).c_str(), 0);
  preferences.end();

  if (assignedId == 0) {
    // Find next free ID
    preferences.begin("id_map", false);
    for (int i = 1; i <= 50; i++) {
      String existingMac = preferences.getString(String(i).c_str(), "");
      if (existingMac == "") {
        assignedId = i;
        preferences.putString(String(i).c_str(), mac);
        break;
      }
    }
    preferences.end();

    if (assignedId > 0) {
      preferences.begin("registry", false);
      preferences.putInt(getMacKey(mac).c_str(), assignedId);
      preferences.end();
      Serial.printf("[REGISTRY] New Slave registered. MAC: %s assigned ID: EXC-%02d\n", mac.c_str(), assignedId);
    } else {
      Serial.println("[REGISTRY] ERROR: No free IDs available!");
    }
  } else {
    Serial.printf("[REGISTRY] Known Slave returning from Flash. MAC: %s loaded ID: EXC-%02d\n", mac.c_str(), assignedId);
  }

  return assignedId;
}

// Add or update slave in the in-memory array
void upsertSlave(const String& mac, int id, const uint8_t* rawMac) {
  // Check if already exists
  for (int i = 0; i < slaveCount; i++) {
    if (slaves[i].mac == mac) {
      slaves[i].id = id;
      slaves[i].lastSeen = millis();
      memcpy(slaves[i].rawMac, rawMac, 6);
      return;
    }
  }
  // Add new
  if (slaveCount < 50) {
    slaves[slaveCount].mac = mac;
    slaves[slaveCount].id = id;
    slaves[slaveCount].ip = "";  // ESP-NOW slaves don't have IPs
    slaves[slaveCount].lastSeen = millis();
    slaves[slaveCount].state = "LOCKED";
    slaves[slaveCount].time_left = 0;
    slaves[slaveCount].battery = "OK";
    memcpy(slaves[slaveCount].rawMac, rawMac, 6);
    slaveCount++;
  }
}

// ===== ESP-NOW RECEIVE CALLBACK =====
// Note: ESP32 standard (IDF v4) uses the old callback signature.
// ESP32-C3 (IDF v5) uses esp_now_recv_info_t. We handle both via macro.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac_addr = info->src_addr;
#else
void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
#endif
  if (len != sizeof(EspNowPacket)) return;

  EspNowPacket pkt;
  memcpy(&pkt, data, sizeof(EspNowPacket));

  String senderMac = macToString(pkt.mac);

  switch (pkt.type) {
    case PKT_REGISTER_REQ: {
      Serial.printf("[ESPNOW] Registration request from MAC: %s\n", senderMac.c_str());

      int assignedId = 0;
      if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        assignedId = assignIdForMac(senderMac);
        if (assignedId > 0) {
          upsertSlave(senderMac, assignedId, pkt.mac);
        }
        xSemaphoreGive(slavesMutex);
      }

      // Send registration response
      EspNowPacket resp;
      memset(&resp, 0, sizeof(resp));
      resp.type = PKT_REGISTER_RESP;
      resp.targetId = assignedId;
      resp.senderId = 0; // Master
      memcpy(resp.mac, pkt.mac, 6);

      esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&resp, sizeof(resp));
      Serial.printf("[ESPNOW] Sent registration response: ID=%d to %s\n", assignedId, senderMac.c_str());
      break;
    }

    case PKT_HEARTBEAT: {
      if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        bool found = false;
        for (int i = 0; i < slaveCount; i++) {
          if (slaves[i].mac == senderMac) {
            found = true;
            slaves[i].state = pktStateName(pkt.state);
            slaves[i].time_left = pkt.timeLeft;
            slaves[i].battery = "OK";
            slaves[i].lastSeen = millis();

            // Check if slave's ID is out of sync (e.g., after edit_slave)
            if (pkt.senderId != (uint8_t)slaves[i].id) {
              Serial.printf("[ESPNOW] Slave %s has stale ID %d, correcting to %d\n",
                            senderMac.c_str(), pkt.senderId, slaves[i].id);
              // Send correction
              EspNowPacket corr;
              memset(&corr, 0, sizeof(corr));
              corr.type = PKT_REGISTER_RESP;
              corr.targetId = slaves[i].id;
              memcpy(corr.mac, pkt.mac, 6);
              esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&corr, sizeof(corr));
            }
            break;
          }
        }

        // Unknown MAC sending heartbeats — auto-register from Preferences
        // This handles Master reboot: slave is still heartbeating but Master
        // lost its in-memory array. The Preferences (flash) still has the MAC→ID mapping.
        if (!found) {
          Serial.printf("[ESPNOW] Heartbeat from unknown MAC %s — auto-registering\n", senderMac.c_str());
          int assignedId = assignIdForMac(senderMac);
          if (assignedId > 0) {
            upsertSlave(senderMac, assignedId, pkt.mac);
            // Update state from heartbeat data
            for (int i = 0; i < slaveCount; i++) {
              if (slaves[i].mac == senderMac) {
                slaves[i].state = pktStateName(pkt.state);
                slaves[i].time_left = pkt.timeLeft;
                slaves[i].lastSeen = millis();
                break;
              }
            }
            // Send registration response so slave knows it's recognized
            EspNowPacket resp;
            memset(&resp, 0, sizeof(resp));
            resp.type = PKT_REGISTER_RESP;
            resp.targetId = assignedId;
            memcpy(resp.mac, pkt.mac, 6);
            esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&resp, sizeof(resp));
          }
        }

        xSemaphoreGive(slavesMutex);
      }
      break;
    }

    case PKT_COMMAND_RESP: {
      // Store the response and signal the waiting HTTP handler
      portENTER_CRITICAL(&cmdResponseMux);
      memcpy(&cmdResponsePkt, &pkt, sizeof(pkt));
      cmdResponseReceived = true;
      portEXIT_CRITICAL(&cmdResponseMux);

      if (cmdResponseSem) {
        xSemaphoreGive(cmdResponseSem);
      }

      // Also update the slave cache
      if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        for (int i = 0; i < slaveCount; i++) {
          if (slaves[i].mac == senderMac) {
            slaves[i].state = pktStateName(pkt.state);
            slaves[i].time_left = pkt.timeLeft;
            slaves[i].lastSeen = millis();
            break;
          }
        }
        xSemaphoreGive(slavesMutex);
      }
      break;
    }

    default:
      Serial.printf("[ESPNOW] Unknown packet type: %d\n", pkt.type);
      break;
  }
}

// ===== HTTP HANDLERS =====

// GET /api/slaves — List all slave units (unchanged API)
void handleSlaves() {
  addCorsHeaders();

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  uint32_t now = millis();

  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      JsonObject obj = array.add<JsonObject>();
      obj["id"] = slaves[i].id;
      obj["ip"] = slaves[i].ip;
      obj["mac"] = slaves[i].mac;
      obj["online"] = (now - slaves[i].lastSeen) < ONLINE_THRESHOLD_MS;
      obj["state"] = slaves[i].state;
      obj["time_left"] = slaves[i].time_left;
      obj["battery"] = slaves[i].battery;
    }
    xSemaphoreGive(slavesMutex);
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// POST /api/command — Send command to slave via ESP-NOW (same API contract)
void handleCommandProxy() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400);
    return;
  }
  String body = server.arg("plain");

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Invalid JSON\"}");
    return;
  }
  int targetId = doc["id"] | 0;
  String cmdStr = doc["cmd"] | "";
  int time = doc["time"] | 0;
  if (time < 0) time = 0;

  if (targetId <= 0 || cmdStr == "") {
    Serial.println("[PROXY] Command failed: Missing id or cmd");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Missing id or cmd\"}");
    return;
  }

  cmdStr.toUpperCase();
  CmdType cmdEnum = cmdFromString(cmdStr.c_str());
  if (cmdEnum == CMD_NONE) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Unknown command\"}");
    return;
  }

  Serial.printf("[PROXY] Command '%s' (time: %d) for EXC-%02d\n", cmdStr.c_str(), time, targetId);
  netLedFlash(80);

  // Check slave exists
  bool found = false;
  int slaveIdx = -1;
  String targetMac = "";
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].id == targetId) {
        found = true;
        slaveIdx = i;
        targetMac = slaves[i].mac;
        break;
      }
    }
    xSemaphoreGive(slavesMutex);
  }

  if (!found) {
    Serial.printf("[PROXY] Failed: Slave EXC-%02d not found in registry\n", targetId);
    server.send(404, "application/json", "{\"ok\":0,\"error\":\"Slave not found\"}");
    return;
  }

#if DEMO_MODE
  if (targetMac.startsWith("FF:FF:FF:00:00:")) {
    if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
      if (cmdEnum == CMD_ADD_TIME) {
        slaves[slaveIdx].time_left += time;
        if (slaves[slaveIdx].state == "LOCKED" || slaves[slaveIdx].state == "ENDED") {
          slaves[slaveIdx].state = "RUNNING";
        }
      } else if (cmdEnum == CMD_PAUSE) {
        slaves[slaveIdx].state = "PAUSED";
      } else if (cmdEnum == CMD_RESUME) {
        if (slaves[slaveIdx].time_left > 0) {
          slaves[slaveIdx].state = "RUNNING";
        }
      } else if (cmdEnum == CMD_STOP) {
        slaves[slaveIdx].state = "ENDED";
        slaves[slaveIdx].time_left = 0;
      }
      slaves[slaveIdx].lastSeen = millis();
      char resp[128];
      snprintf(resp, sizeof(resp), "{\"ok\":1,\"code\":\"SUCCESS\",\"time_left\":%d,\"state\":\"%s\"}", slaves[slaveIdx].time_left, slaves[slaveIdx].state.c_str());
      server.send(200, "application/json", String(resp));
      xSemaphoreGive(slavesMutex);
    } else {
      server.send(503, "application/json", "{\"ok\":0,\"error\":\"Mutex timeout\"}");
    }
    return;
  }
#endif

  // Build ESP-NOW command packet
  EspNowPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = PKT_COMMAND;
  pkt.targetId = targetId;
  pkt.senderId = 0; // Master
  pkt.cmd = cmdEnum;
  pkt.value = time;

  // Reset response state
  portENTER_CRITICAL(&cmdResponseMux);
  cmdResponseReceived = false;
  portEXIT_CRITICAL(&cmdResponseMux);

  // Broadcast the command
  esp_err_t result = esp_now_send(ESPNOW_BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
  if (result != ESP_OK) {
    Serial.printf("[PROXY] ESP-NOW send failed: %d\n", result);
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Radio send failed\"}");
    return;
  }

  // Wait for response from slave
  bool gotResponse = false;
  if (xSemaphoreTake(cmdResponseSem, pdMS_TO_TICKS(CMD_RESPONSE_TIMEOUT_MS)) == pdTRUE) {
    portENTER_CRITICAL(&cmdResponseMux);
    gotResponse = cmdResponseReceived;
    portEXIT_CRITICAL(&cmdResponseMux);
  }

  if (gotResponse && cmdResponsePkt.senderId == (uint8_t)targetId) {
    // Build JSON response matching the API spec exactly
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":%d,\"code\":\"%s\",\"time_left\":%lu,\"state\":\"%s\"}",
             (cmdResponsePkt.respCode == RESP_OK || cmdResponsePkt.respCode == RESP_REBOOTING) ? 1 : 0,
             respCodeName(cmdResponsePkt.respCode),
             (unsigned long)cmdResponsePkt.timeLeft,
             pktStateName(cmdResponsePkt.state));
    Serial.printf("[PROXY] Slave responded: %s\n", resp);
    server.send(200, "application/json", String(resp));
  } else {
    Serial.printf("[PROXY] Slave EXC-%02d timeout (no ESP-NOW response)\n", targetId);
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Slave offline/timeout\"}");
  }
}

// POST /api/edit_slave — Change slave ID (unchanged logic)
void handleEditSlave() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400);
    return;
  }
  String body = server.arg("plain");

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  String mac = doc["mac"] | "";
  int newId = doc["id"] | 0;

  if (mac == "" || !isValidMac(mac) || newId <= 0) {
    Serial.println("[MANAGE] Edit ID failed: Invalid MAC or ID");
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }

  Serial.printf("[MANAGE] Changing ID for MAC %s to EXC-%02d\n", mac.c_str(), newId);

  preferences.begin("id_map", false);
  String owner = preferences.getString(String(newId).c_str(), "");
  if (owner != "" && owner != mac) {
    preferences.end();
    Serial.println("[MANAGE] Edit ID failed: ID already taken");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"ID Taken\"}");
    return;
  }
  preferences.end();

  int oldId = 0;
  preferences.begin("registry", false);
  oldId = preferences.getInt(getMacKey(mac).c_str(), 0);
  preferences.putInt(getMacKey(mac).c_str(), newId);
  preferences.end();

  preferences.begin("id_map", false);
  if (oldId > 0 && oldId != newId) {
    preferences.remove(String(oldId).c_str());
  }
  preferences.putString(String(newId).c_str(), mac);
  preferences.end();

  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].mac == mac) {
        slaves[i].id = newId;
        break;
      }
    }
    xSemaphoreGive(slavesMutex);
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// POST /api/delete_slave — Delete slave from registry (unchanged logic)
void handleDeleteSlave() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400);
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  String mac = doc["mac"] | "";

  if (mac == "" || !isValidMac(mac)) {
    Serial.println("[MANAGE] Delete Slave failed: Invalid MAC");
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }

  Serial.printf("[MANAGE] Deleting Slave MAC %s from registry\n", mac.c_str());
  preferences.begin("registry", false);
  int deletedId = preferences.getInt(getMacKey(mac).c_str(), 0);
  preferences.remove(getMacKey(mac).c_str());
  preferences.end();

  if (deletedId > 0) {
    preferences.begin("id_map", false);
    preferences.remove(String(deletedId).c_str());
    preferences.end();
  }

  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].mac == mac) {
        for (int j = i; j < slaveCount - 1; j++) {
          slaves[j] = slaves[j + 1];
        }
        slaveCount--;
        break;
      }
    }
    xSemaphoreGive(slavesMutex);
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// POST /api/reset_registry — Wipes all MAC-to-ID mappings in memory and flash
void handleResetRegistry() {
  addCorsHeaders();

  Serial.println("[MANAGE] FACTORY RESET: Wiping slave registry...");

  // Wipe flash
  preferences.begin("registry", false);
  preferences.clear();
  preferences.end();

  preferences.begin("id_map", false);
  preferences.clear();
  preferences.end();

  // Wipe memory
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    slaveCount = 0;
    xSemaphoreGive(slavesMutex);
  }

  server.send(200, "application/json", "{\"ok\":1,\"message\":\"Registry wiped. Restart Slaves to re-register.\"}");
  netLedFlash(500); // long flash
}

// GET /api/register — Kept for backward compatibility with HTTP-based slaves
void handleRegister() {
  addCorsHeaders();
  String mac = server.arg("mac");
  if (mac == "") {
    server.send(400, "application/json", "{\"error\":\"MAC required\"}");
    return;
  }
  if (!isValidMac(mac)) {
    server.send(400, "application/json", "{\"error\":\"Invalid MAC format\"}");
    return;
  }
  String ip = server.client().remoteIP().toString();

  int assignedId = 0;

  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"Server busy\"}");
    return;
  }

  assignedId = assignIdForMac(mac);
  if (assignedId > 0) {
    // For HTTP slaves, we store the IP
    uint8_t dummyMac[6] = {0};
    upsertSlave(mac, assignedId, dummyMac);
    // Update IP for HTTP slaves
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].mac == mac) {
        slaves[i].ip = ip;
        slaves[i].lastSeen = millis();
        break;
      }
    }
  }

  xSemaphoreGive(slavesMutex);

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"id\":%d}", assignedId);
  server.send(200, "application/json", String(buf));
  netLedFlash(80);
}

// ===== AP EVENT HANDLER =====
void onApEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      uint8_t* mac = info.wifi_ap_staconnected.mac;
      Serial.printf("[AP] Station connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      uint8_t* mac = info.wifi_ap_stadisconnected.mac;
      Serial.printf("[AP] Station disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    default:
      break;
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  slavesMutex = xSemaphoreCreateMutex();
  cmdResponseSem = xSemaphoreCreateBinary();

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

  Serial.println("Starting Master Access Point (ESP-NOW Gateway)");
  Serial.printf("[BOOT] Free heap: %lu bytes, Min free ever: %lu bytes\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());

  // Wi-Fi: AP+STA mode (AP for Android, STA for ESP-NOW)
  WiFi.mode(WIFI_AP_STA);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, ESPNOW_CHANNEL, 0, 10);
  WiFi.setSleep(false);

  // Wi-Fi optimization
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  esp_wifi_set_max_tx_power(84); // 21dBm Max power

  WiFi.onEvent(onApEvent);

  // Captive portal DNS — keeps Android Wi-Fi connected
  dnsServer.start(DNS_PORT, "*", apIP);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ERROR: Init failed! Rebooting...");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onEspNowRecv);

  // Add broadcast peer so we can send broadcasts
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST, 6);
  peerInfo.channel = 0; // 0 = current channel
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[ESPNOW] Initialized successfully");

#if DEMO_MODE
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    uint8_t fakeRaws[5][6] = {
      {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01},
      {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x02},
      {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x03},
      {0xEE, 0xEE, 0xEE, 0x00, 0x00, 0x04},
      {0xEE, 0xEE, 0xEE, 0x00, 0x00, 0x05}
    };
    String fakeMacs[5] = {
      "FF:FF:FF:00:00:01", "FF:FF:FF:00:00:02", "FF:FF:FF:00:00:03",
      "EE:EE:EE:00:00:04", "EE:EE:EE:00:00:05"
    };
    String fakeStates[5] = {"ENDED", "ENDED", "ENDED", "ENDED", "ENDED"};
    int fakeTimes[5] = {0, 0, 0, 0, 0};

    for (int i = 0; i < 5; i++) {
      int assignedId = assignIdForMac(fakeMacs[i]);
      if (assignedId > 0) {
        upsertSlave(fakeMacs[i], assignedId, fakeRaws[i]);
        for(int j = 0; j < slaveCount; j++) {
          if(slaves[j].mac == fakeMacs[i]) {
            slaves[j].state = fakeStates[i];
            slaves[j].time_left = fakeTimes[i];
            // If offline demo unit, reset lastSeen so it instantly shows offline
            if (fakeMacs[i].startsWith("EE:")) {
              slaves[j].lastSeen = 0;
            }
          }
        }
      }
    }
    xSemaphoreGive(slavesMutex);
    Serial.println("[DEMO] Added fake slaves.");
  }
#endif

// HTTP endpoints — identical API surface for Android app
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send_P(200, "text/html", (const char*)index_html_gz, sizeof(index_html_gz));
  });

  server.on("/api/register", HTTP_GET, handleRegister);
  server.on("/api/register", HTTP_OPTIONS, handleOptions);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/slaves", HTTP_OPTIONS, handleOptions);
  server.on("/api/command", HTTP_POST, handleCommandProxy);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.on("/api/edit_slave", HTTP_POST, handleEditSlave);
  server.on("/api/edit_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete_slave", HTTP_POST, handleDeleteSlave);
  server.on("/api/delete_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/reset_registry", HTTP_POST, handleResetRegistry);
  server.on("/api/reset_registry", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Web Server running at http://192.168.4.1/");
  Serial.println("[SYSTEM] Master ready! Android connects via Wi-Fi, Slaves via ESP-NOW.");
}

// ===== LOOP =====
void loop() {
  dnsServer.processNextRequest();
  esp_task_wdt_reset();
  server.handleClient();
  updateLed();

  static uint32_t lastLedBlink = 0;
  static uint32_t lastHb = 0;
  uint32_t now = millis();

  if (now - lastLedBlink >= 3000) {
    lastLedBlink = now;
    netLedFlash(50);
  }

  if (now - lastHb >= 1000) {
    lastHb = now;
    if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
      Serial.printf("[MASTER-HB] up=%lus slaves=%d\n", now / 1000, slaveCount);
#if DEMO_MODE
      for (int i = 0; i < slaveCount; i++) {
        if (slaves[i].mac.startsWith("FF:FF:FF:00:00:")) {
          slaves[i].lastSeen = now;
          if (slaves[i].state == "RUNNING" && slaves[i].time_left > 0) {
            slaves[i].time_left--;
            if (slaves[i].time_left <= 0) {
              slaves[i].state = "ENDED";
            }
          }
        }
      }
#endif
      xSemaphoreGive(slavesMutex);
    }
  }
}
