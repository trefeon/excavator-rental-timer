/*
 * Excavator Rental Timer - Wi-Fi Master (DHCP & Zero-Touch Registry)
 * 
 * Features:
 * - Acts as an Access Point (SSID: ExcavatorMaster)
 * - DHCP server active to assign dynamic IPs to Slaves
 * - Registry logic (MAC -> ID mapping) stored in Preferences
 * - Embedded Web UI Dashboard with Slave Management
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <esp_idf_version.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

static const char* AP_SSID = "ExcavatorMaster";
static const char* AP_PASS = "12345678";
static const char* ADMIN_PASS = "admin123";

WebServer server(80);
Preferences preferences;
SemaphoreHandle_t slavesMutex = NULL;

static const uint32_t HTTP_TIMEOUT_MS = 1500;
static const uint32_t ONLINE_THRESHOLD_MS = 30000;
static const uint32_t MUTEX_TIMEOUT_TICKS = pdMS_TO_TICKS(500);



#define LED_PIN 2

void addCorsHeaders();
void handleOptions();

void netLedFlash(int ms = 80) {
  digitalWrite(LED_PIN, LOW);  // ON
  delay(ms);
  digitalWrite(LED_PIN, HIGH); // OFF
}

struct SlaveRecord {
  String mac;
  int id;
  String ip;
  uint32_t lastSeen;
  String state; // "RUNNING", "LOCKED", "PAUSED", "ENDED", "OFFLINE"
  int time_left;      // remaining time in seconds
  String battery;
};
SlaveRecord slaves[50];
int slaveCount = 0;




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

  int foundIdx = -1;
  for (int i = 0; i < slaveCount; i++) {
    if (slaves[i].mac == mac) {
      foundIdx = i;
      break;
    }
  }

  if (foundIdx >= 0) {
    assignedId = slaves[foundIdx].id;
    slaves[foundIdx].ip = ip;
    slaves[foundIdx].lastSeen = millis();
    Serial.printf("[REGISTRY] Re-registered existing MAC: %s as EXC-%02d (IP: %s)\n", mac.c_str(), assignedId, ip.c_str());
  } else {
    preferences.begin("registry", false);
    assignedId = preferences.getInt(getMacKey(mac).c_str(), 0);
    preferences.end();

    if (assignedId == 0) {
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

    if (assignedId > 0 && slaveCount < 50) {
      bool duplicate = false;
      for (int i = 0; i < slaveCount; i++) {
        if (slaves[i].mac == mac) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        slaves[slaveCount].mac = mac;
        slaves[slaveCount].id = assignedId;
        slaves[slaveCount].ip = ip;
        slaves[slaveCount].lastSeen = millis();
        slaveCount++;
      }
    } else if (slaveCount >= 50) {
      Serial.println("[REGISTRY] WARNING: Max slaves reached!");
    }
  }

  xSemaphoreGive(slavesMutex);

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"id\":%d}", assignedId);
  server.send(200, "application/json", String(buf));
  netLedFlash(80);
}

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
  String cmd = doc["cmd"] | "";
  int time = doc["time"] | 0;
  if (time < 0) time = 0;

  if (targetId <= 0 || cmd == "") {
    Serial.println("[PROXY] Command failed: Missing id or cmd");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Missing id or cmd\"}");
    return;
  }

  Serial.printf("[PROXY] Intercepted command '%s' (time: %d) for EXC-%02d\n", cmd.c_str(), time, targetId);
  netLedFlash(80);

  String targetIp = "";
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].id == targetId) {
        targetIp = slaves[i].ip;
        break;
      }
    }
    xSemaphoreGive(slavesMutex);
  }

  if (targetIp == "") {
    Serial.printf("[PROXY] Failed: Slave EXC-%02d IP not found in registry\n", targetId);
    server.send(404, "application/json", "{\"ok\":0,\"error\":\"Slave not found\"}");
    return;
  }

  // --- WORKAROUND FOR ANDROID APP BUG ---
  // The Android app mistakenly sends CMD_ADD_TIME with time = remaining seconds when resuming a paused timer.
  // Since the app's UI doesn't allow opening the "Add Time" dialog while the timer is paused, 
  // any ADD_TIME received while PAUSED is guaranteed to be this resume bug.
  // We intercept it and convert it to a proper RESUME command.
  /*
  if (cmd == "ADD_TIME" && slaveState == "PAUSED") {
    Serial.println("[PROXY] Intercepted Android App Resume Bug. Converting ADD_TIME to RESUME.");
    cmd = "RESUME";
    time = 0;
  }
  */
  // --------------------------------------

  Serial.printf("[PROXY] Forwarding to %s/api/command\n", targetIp.c_str());
  HTTPClient http;
  http.begin("http://" + targetIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);
  String payload = "{\"cmd\":\"" + cmd + "\",\"time\":" + String(time) + "}";
  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[PROXY] Slave replied (%d): %s\n", httpCode, response.c_str());
    server.send(httpCode, "application/json", response);
  } else {
    Serial.printf("[PROXY] Slave %s offline or timeout\n", targetIp.c_str());
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Slave offline/timeout\"}");
  }
  http.end();
}

void onApEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      {
        uint8_t* mac = info.wifi_ap_staconnected.mac;
        Serial.printf("[AP] Station connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
      }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      {
        uint8_t* mac = info.wifi_ap_stadisconnected.mac;
        Serial.printf("[AP] Station disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
      }
    default:
      break;
  }
}

void pollSlavesTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();

    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    digitalWrite(LED_PIN, LOW);

    uint32_t now = millis();
    String targetIps[50];
    int targetCount = 0;

    if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
      for (int i = 0; i < slaveCount && targetCount < 50; i++) {
        if (now - slaves[i].lastSeen < ONLINE_THRESHOLD_MS && slaves[i].ip != "") {
          targetIps[targetCount++] = slaves[i].ip;
        }
      }
      xSemaphoreGive(slavesMutex);
    }

    for (int i = 0; i < targetCount; i++) {
      esp_task_wdt_reset(); // PREVENT BOOTLOOP if multiple slaves timeout!
      
      HTTPClient http;
      http.begin("http://" + targetIps[i] + "/api/state");
      http.setTimeout(HTTP_TIMEOUT_MS);
      int code = http.GET();
      if (code == 200) {
        String payload = http.getString();
        JsonDocument stateDoc;
        if (!deserializeJson(stateDoc, payload)) {
          if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
            for (int j = 0; j < slaveCount; j++) {
              if (slaves[j].ip == targetIps[i]) {
                if (stateDoc["state"].is<const char*>()) {
                  int time_left = stateDoc["time_left"] | -1;
                  if (time_left >= 0) slaves[j].time_left = time_left;
                  slaves[j].state = stateDoc["state"].as<String>();
                  slaves[j].battery = stateDoc["battery"] | "";
                  slaves[j].lastSeen = millis();
                }
                break;
              }
            }
            xSemaphoreGive(slavesMutex);
          }
        }
      }
      http.end();
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  slavesMutex = xSemaphoreCreateMutex();

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

  Serial.println("Starting Master Access Point (DHCP enabled)");
  Serial.printf("[BOOT] Free heap: %lu bytes, Min free ever: %lu bytes\n", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());



  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 10);

  WiFi.onEvent(onApEvent);

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "application/json", "{\"status\":\"Excavator Master API Ready\",\"mode\":\"bridge\"}");
  });

  // All endpoints (no auth - handled by Android app)
  server.on("/api/register", HTTP_GET, handleRegister);
  server.on("/api/register", HTTP_OPTIONS, handleOptions);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/slaves", HTTP_OPTIONS, handleOptions);


  // Command & slave management
  server.on("/api/command", HTTP_POST, handleCommandProxy);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.on("/api/edit_slave", HTTP_POST, handleEditSlave);
  server.on("/api/edit_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete_slave", HTTP_POST, handleDeleteSlave);
  server.on("/api/delete_slave", HTTP_OPTIONS, handleOptions);





  server.begin();
  Serial.println("Web Server running at http://192.168.4.1/");

  xTaskCreatePinnedToCore(pollSlavesTask, "PollTask", 16384, NULL, 1, NULL, 0);
}

void loop() {
  static uint32_t lastHb = 0;
  esp_task_wdt_reset();
  server.handleClient();
  uint32_t now = millis();
  if (now - lastHb >= 1000) {
    lastHb = now;
    if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
      Serial.printf("[MASTER-HB] up=%lus slaves=%d\n", now / 1000, slaveCount);
      xSemaphoreGive(slavesMutex);
    }
  }
}

