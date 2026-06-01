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

static const char* AP_SSID = "ExcavatorMaster";
static const char* AP_PASS = "12345678";

WebServer server(80);
Preferences preferences;

struct SlaveRecord {
  String mac;
  int id;
  String ip;
  uint32_t lastSeen;
  String state;
  int rem;
  String disp;
  int paid;
  String bat;
};
SlaveRecord slaves[50];
int slaveCount = 0;

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Master Dashboard</title>
<style>
body { font-family: sans-serif; text-align: center; margin-top: 20px; background: #f8fafc; color: #0f172a; }
.card { background: white; padding: 20px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); display: inline-block; min-width: 320px; margin-bottom: 20px; }
h1 { margin-top: 0; font-size: 24px; color: #2563eb; }
select { font-size: 18px; padding: 10px; width: 100%; margin-bottom: 20px; border-radius: 8px; border: 1px solid #cbd5e1; }
.time { font-size: 64px; font-weight: bold; margin: 10px 0; color: #1e293b; font-family: monospace; }
.status { font-size: 18px; color: #64748b; font-weight: bold; margin-bottom: 20px; text-transform: uppercase; }
button { font-size: 16px; padding: 12px 20px; margin: 5px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; }
.btn-add { background: #10b981; color: white; width: 45%; }
.btn-pause { background: #f59e0b; color: white; width: 45%; }
.btn-stop { background: #ef4444; color: white; width: 95%; margin-top: 10px; }
.btn-manage { background: #475569; color: white; width: 95%; margin-top: 10px; font-size: 14px; }
.loader { font-size: 14px; color: #94a3b8; margin-top: 10px; display: none; }

/* Modal */
.modal { display: none; position: fixed; z-index: 10; left: 0; top: 0; width: 100%; height: 100%; background-color: rgba(0,0,0,0.5); }
.modal-content { background-color: white; margin: 10% auto; padding: 20px; border-radius: 12px; width: 90%; max-width: 500px; text-align: left; }
.close { color: #aaa; float: right; font-size: 28px; font-weight: bold; cursor: pointer; }
table { width: 100%; border-collapse: collapse; margin-top: 15px; }
th, td { border: 1px solid #e2e8f0; padding: 8px; text-align: left; font-size: 14px; }
th { background-color: #f1f5f9; }
.btn-sm { padding: 6px 10px; font-size: 12px; margin: 2px; }
</style>
</head>
<body>
  <div class="card">
    <h1>Excavator Master</h1>
    <select id="toySelect" onchange="pollState()"><option value="">(Mencari Mainan...)</option></select>
    <div class="time" id="dispTime">--:--</div>
    <div class="status" id="dispStatus">OFFLINE</div>
    
    <div>
      <button class="btn-add" onclick="sendCommand('ADD_TIME', 300)">+ 5 Mnt</button>
      <button class="btn-add" onclick="sendCommand('ADD_TIME', 600)">+ 10 Mnt</button>
    </div>
    <div style="margin-top: 10px;">
      <button class="btn-pause" id="btnPause" onclick="togglePause()">Pause</button>
    </div>
    <button class="btn-stop" onclick="sendCommand('STOP', 0)">STOP / KUNCI</button>
    <button class="btn-manage" onclick="openManage()">⚙️ Manage Slaves</button>
    <div class="loader" id="loader">Syncing...</div>
  </div>

  <!-- Manage Modal -->
  <div id="manageModal" class="modal">
    <div class="modal-content">
      <span class="close" onclick="closeManage()">&times;</span>
      <h2>Manajemen Slaves</h2>
      <p style="font-size: 12px; color: #64748b;">(Hijau = Online, Abu-abu = Offline)</p>
      <table>
        <thead><tr><th>ID</th><th>MAC & IP</th><th>Aksi</th></tr></thead>
        <tbody id="slavesTableBody"></tbody>
      </table>
    </div>
  </div>

  <script>
    let currentState = "";
    let slavesList = [];
    
    function getTargetId() { return document.getElementById('toySelect').value; }
    
    async function fetchSlaves() {
      try {
        const res = await fetch('/api/slaves');
        slavesList = await res.json();
        const select = document.getElementById('toySelect');
        const currentVal = select.value;
        if (slavesList.length === 0) {
          select.innerHTML = '<option value="">(Belum ada mainan online)</option>';
          renderManageTable();
          return;
        }
        
        let html = '';
        let onlineCount = 0;
        slavesList.sort((a,b) => a.id - b.id);
        slavesList.forEach(toy => {
          if (toy.online) {
            html += `<option value="${toy.id}">EXC-0${toy.id}</option>`;
            onlineCount++;
          }
        });
        
        if (onlineCount === 0) {
          select.innerHTML = '<option value="">(Semua mainan offline)</option>';
        } else {
          select.innerHTML = html;
          if (currentVal && Array.from(select.options).find(o => o.value === currentVal)) { select.value = currentVal; }
          updateDashboardUI();
        }
        renderManageTable();
      } catch(e) { console.error("Gagal refresh daftar mainan"); }
    }

    function updateDashboardUI() {
      const id = parseInt(getTargetId());
      if (!id) return;
      const toy = slavesList.find(t => t.id === id);
      if (toy && toy.online) {
        document.getElementById('dispTime').innerText = toy.disp || "--:--";
        document.getElementById('dispStatus').innerText = toy.state || "UNKNOWN";
        currentState = toy.state;
        
        let btn = document.getElementById('btnPause');
        if (currentState === "PAUSED") {
          btn.innerText = "Resume";
          btn.style.background = "#3b82f6";
        } else {
          btn.innerText = "Pause";
          btn.style.background = "#f59e0b";
        }
      } else {
        document.getElementById('dispStatus').innerText = "OFFLINE";
        document.getElementById('dispTime').innerText = "--:--";
      }
    }

    async function sendCommand(cmd, val) {
      const id = getTargetId();
      if (!id) return alert("Pilih mainan terlebih dahulu!");
      try {
        await fetch(`/api/command`, {
          method: 'POST',
          body: JSON.stringify({id: parseInt(id), cmd: cmd, val: val}),
          headers: {'Content-Type': 'text/plain'}
        });
        fetchSlaves(); // Segera refresh setelah perintah
      } catch(e) { alert("Gagal mengirim perintah!"); }
    }

    function togglePause() {
      if (currentState === "PAUSED") sendCommand("RESUME", 0);
      else sendCommand("PAUSE", 0);
    }

    /* Management Functions */
    function openManage() { document.getElementById('manageModal').style.display = "block"; fetchSlaves(); }
    function closeManage() { document.getElementById('manageModal').style.display = "none"; }
    
    function renderManageTable() {
      const tbody = document.getElementById('slavesTableBody');
      tbody.innerHTML = '';
      slavesList.forEach(t => {
        const tr = document.createElement('tr');
        const color = t.online ? '#10b981' : '#94a3b8';
        tr.innerHTML = `
          <td style="color: ${color}; font-weight: bold;">EXC-0${t.id}</td>
          <td style="font-size: 11px;">${t.mac}<br>${t.ip}</td>
          <td>
            <button class="btn-sm" style="background:#3b82f6;color:white;" onclick="editId('${t.mac}', ${t.id})">Edit</button>
            <button class="btn-sm" style="background:#ef4444;color:white;" onclick="deleteSlave('${t.mac}')">Del</button>
          </td>
        `;
        tbody.appendChild(tr);
      });
    }

    async function editId(mac, oldId) {
      const newId = prompt(`Ubah nomor ID untuk EXC-0${oldId} (Masukkan angka baru):`, oldId);
      if (!newId || newId == oldId || isNaN(newId)) return;
      try {
        await fetch('/api/edit_slave', {
          method: 'POST',
          body: JSON.stringify({mac: mac, id: parseInt(newId)}),
          headers: {'Content-Type': 'text/plain'}
        });
        fetchSlaves();
      } catch(e) { alert("Gagal ubah ID"); }
    }

    async function deleteSlave(mac) {
      if (!confirm("Hapus slave ini dari registry Master?")) return;
      try {
        await fetch('/api/delete_slave', {
          method: 'POST',
          body: JSON.stringify({mac: mac}),
          headers: {'Content-Type': 'text/plain'}
        });
        fetchSlaves();
      } catch(e) { alert("Gagal hapus"); }
    }

    setInterval(fetchSlaves, 1000); // Polling RAM Master 1 detik sekali
    fetchSlaves();
  </script>
</body>
</html>
)rawliteral";

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleRegister() {
  addCorsHeaders();
  String mac = server.arg("mac");
  if (mac == "") { server.send(400, "application/json", "{\"error\":\"MAC required\"}"); return; }
  String ip = server.client().remoteIP().toString();
  
  int assignedId = 0;
  
  // Look in RAM
  int foundIdx = -1;
  for (int i=0; i<slaveCount; i++) {
    if (slaves[i].mac == mac) { foundIdx = i; break; }
  }

  if (foundIdx >= 0) {
    assignedId = slaves[foundIdx].id;
    slaves[foundIdx].ip = ip;
    slaves[foundIdx].lastSeen = millis();
  } else {
    // Look in Flash
    preferences.begin("registry", false);
    assignedId = preferences.getInt(mac.c_str(), 0);
    if (assignedId == 0) { // Brand new
      assignedId = preferences.getInt("next_id", 1);
      preferences.putInt("next_id", assignedId + 1);
      preferences.putInt(mac.c_str(), assignedId);
    }
    preferences.end();
    
    if (slaveCount < 50) {
      slaves[slaveCount].mac = mac;
      slaves[slaveCount].id = assignedId;
      slaves[slaveCount].ip = ip;
      slaves[slaveCount].lastSeen = millis();
      slaveCount++;
    }
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"id\":%d}", assignedId);
  server.send(200, "application/json", String(buf));
}

void handleSlaves() {
  addCorsHeaders();
  String json = "[";
  uint32_t now = millis();
  for(int i=0; i<slaveCount; i++) {
    bool online = (now - slaves[i].lastSeen) < 30000;
    if (i > 0) json += ",";
    json += "{";
    json += "\"id\":" + String(slaves[i].id) + ",";
    json += "\"ip\":\"" + slaves[i].ip + "\",";
    json += "\"mac\":\"" + slaves[i].mac + "\",";
    json += "\"online\":" + String(online ? "true" : "false") + ",";
    json += "\"state\":\"" + slaves[i].state + "\",";
    json += "\"rem\":" + String(slaves[i].rem) + ",";
    json += "\"disp\":\"" + slaves[i].disp + "\",";
    json += "\"paid\":" + String(slaves[i].paid) + ",";
    json += "\"bat\":\"" + slaves[i].bat + "\"";
    json += "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// Simple JSON body parser
String extractJsonString(String body, String key) {
  int idx = body.indexOf("\"" + key + "\":");
  if (idx < 0) return "";
  int start = body.indexOf("\"", idx + key.length() + 2) + 1;
  int end = body.indexOf("\"", start);
  if (start > 0 && end > start) return body.substring(start, end);
  return "";
}

int extractJsonInt(String body, String key) {
  int idx = body.indexOf("\"" + key + "\":");
  if (idx < 0) return -1;
  int start = idx + key.length() + 3;
  int end = body.indexOf("}", start);
  if (end < 0) end = body.indexOf(",", start);
  if (start > 0 && end > start) return body.substring(start, end).toInt();
  return -1;
}

void handleEditSlave() {
  addCorsHeaders();
  if (!server.hasArg("plain")) { server.send(400); return; }
  String body = server.arg("plain");
  
  String mac = extractJsonString(body, "mac");
  int newId = extractJsonInt(body, "id");
  
  if (mac != "" && newId > 0) {
    // Update Flash
    preferences.begin("registry", false);
    preferences.putInt(mac.c_str(), newId);
    preferences.end();
    
    // Update RAM
    for (int i=0; i<slaveCount; i++) {
      if (slaves[i].mac == mac) {
        slaves[i].id = newId;
        break;
      }
    }
    server.send(200, "application/json", "{\"ok\":1}");
  } else {
    server.send(400, "application/json", "{\"ok\":0}");
  }
}

void handleDeleteSlave() {
  addCorsHeaders();
  if (!server.hasArg("plain")) { server.send(400); return; }
  String body = server.arg("plain");
  String mac = extractJsonString(body, "mac");
  
  if (mac != "") {
    // Remove from Flash
    preferences.begin("registry", false);
    preferences.remove(mac.c_str());
    preferences.end();
    
    // Remove from RAM array
    for (int i=0; i<slaveCount; i++) {
      if (slaves[i].mac == mac) {
        // Shift remaining items
        for (int j=i; j<slaveCount-1; j++) {
          slaves[j] = slaves[j+1];
        }
        slaveCount--;
        break;
      }
    }
    server.send(200, "application/json", "{\"ok\":1}");
  } else {
    server.send(400, "application/json", "{\"ok\":0}");
  }
}

void handleCommandProxy() {
  addCorsHeaders();
  if (!server.hasArg("plain")) { server.send(400); return; }
  String body = server.arg("plain");
  
  int targetId = extractJsonInt(body, "id");
  String cmd = extractJsonString(body, "cmd");
  int val = extractJsonInt(body, "val");

  if (targetId <= 0 || cmd == "") {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Missing id or cmd\"}");
    return;
  }

  // Find IP of target slave
  String targetIp = "";
  for (int i=0; i<slaveCount; i++) {
    if (slaves[i].id == targetId) {
      targetIp = slaves[i].ip;
      break;
    }
  }

  if (targetIp == "") {
    server.send(404, "application/json", "{\"ok\":0,\"error\":\"Slave not found\"}");
    return;
  }

  // Forward command to slave
  HTTPClient http;
  http.begin("http://" + targetIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"cmd\":\"" + cmd + "\",\"val\":" + String(val) + "}";
  int httpCode = http.POST(payload);
  
  if (httpCode > 0) {
    String response = http.getString();
    server.send(httpCode, "application/json", response);
  } else {
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Slave offline/timeout\"}");
  }
  http.end();
}

// ================= Background Polling Task (Core 0) =================
void pollSlavesTask(void *pvParameters) {
  for (;;) {
    uint32_t now = millis();
    for (int i=0; i<slaveCount; i++) {
      if (now - slaves[i].lastSeen < 30000) { // Only poll online slaves
         HTTPClient http;
         http.begin("http://" + slaves[i].ip + "/api/state");
         http.setTimeout(1000);
         int code = http.GET();
         if (code == 200) {
           String payload = http.getString();
           slaves[i].state = extractJsonString(payload, "state");
           int rem = extractJsonInt(payload, "rem");
           if (rem >= 0) slaves[i].rem = rem;
           slaves[i].disp = extractJsonString(payload, "disp");
           int paid = extractJsonInt(payload, "paid");
           if (paid >= 0) slaves[i].paid = paid;
           slaves[i].bat = extractJsonString(payload, "bat");
         }
         http.end();
      }
      vTaskDelay(100 / portTICK_PERIOD_MS); // Brief yield between IP queries
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Master polling interval 1 second
  }
}
// ===================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Master Access Point (DHCP enabled)");
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", DASHBOARD_HTML); });
  
  // Registry Endpoints
  server.on("/api/register", HTTP_GET, handleRegister);
  server.on("/api/register", HTTP_OPTIONS, handleOptions);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/slaves", HTTP_OPTIONS, handleOptions);
  
  // Management Endpoints
  server.on("/api/edit_slave", HTTP_POST, handleEditSlave);
  server.on("/api/edit_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete_slave", HTTP_POST, handleDeleteSlave);
  server.on("/api/delete_slave", HTTP_OPTIONS, handleOptions);
  
  // Proxy Endpoints
  server.on("/api/command", HTTP_POST, handleCommandProxy);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  
  server.begin();
  Serial.println("Web Server running at http://192.168.4.1/");

  // Start polling task on Core 0 (Web server runs on Core 1 by default)
  xTaskCreatePinnedToCore(pollSlavesTask, "PollTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  server.handleClient();
}
