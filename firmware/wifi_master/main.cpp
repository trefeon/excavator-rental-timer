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

static const uint32_t HTTP_TIMEOUT_MS = 2000;
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
  String state;
  int rem;
  String disp;
  int paid;
  String bat;
};
SlaveRecord slaves[50];
int slaveCount = 0;

void recordSessionEnd(int id, uint32_t paidSec);

// ══════════════════════════════════════════════════════════════
//  PACKAGE / REVENUE SYSTEM
// ══════════════════════════════════════════════════════════════

struct TimePackage {
  uint8_t durationMin;
  uint32_t priceIDR;
};

struct RevenueSession {
  int slaveId;
  uint32_t durationSec;
  uint32_t priceIDR;
  uint32_t timestamp;
};

// ── Usage history (per slave, stored in NVS) ───────────────
struct HistoryData {
  uint32_t totalSec;
  uint32_t sessions;
  uint32_t lastSec;
  uint32_t lastTime;
};
bool prevRunning[50] = {false};

void loadHistory(int id, HistoryData* h) {
  memset(h, 0, sizeof(HistoryData));
  char key[16];
  snprintf(key, sizeof(key), "h%d", id);
  preferences.begin("history", true);
  preferences.getBytes(key, h, sizeof(HistoryData));
  preferences.end();
}

void saveHistory(int id, const HistoryData* h) {
  char key[16];
  snprintf(key, sizeof(key), "h%d", id);
  preferences.begin("history", false);
  preferences.putBytes(key, h, sizeof(HistoryData));
  preferences.end();
}

void recordSessionEnd(int id, uint32_t paidSec) {
  if (paidSec == 0) return;
  HistoryData h;
  loadHistory(id, &h);
  h.totalSec += paidSec;
  h.sessions++;
  h.lastSec = paidSec;
  h.lastTime = millis() / 1000;
  saveHistory(id, &h);

  // Find price for this duration (closest match)
  uint32_t price = 0;
  uint32_t bestDiff = 999999;
  preferences.begin("packages", true);
  int pkgCount = preferences.getInt("count", 0);
  for (int i = 0; i < pkgCount; i++) {
    TimePackage pkg;
    memset(&pkg, 0, sizeof(TimePackage));
    char k[8];
    snprintf(k, sizeof(k), "p%d", i);
    preferences.getBytes(k, &pkg, sizeof(TimePackage));
    uint32_t pkgSec = pkg.durationMin * 60;
    uint32_t diff = (paidSec > pkgSec) ? (paidSec - pkgSec) : (pkgSec - paidSec);
    if (diff < bestDiff) {
      bestDiff = diff;
      price = pkg.priceIDR;
    }
  }
  preferences.end();

  // Save revenue session
  preferences.begin("revenue", false);
  int revCount = preferences.getInt("count", 0);
  RevenueSession rs;
  rs.slaveId = id;
  rs.durationSec = paidSec;
  rs.priceIDR = price;
  rs.timestamp = millis() / 1000;
  char rk[16];
  snprintf(rk, sizeof(rk), "r%d", revCount);
  preferences.putBytes(rk, &rs, sizeof(RevenueSession));
  preferences.putInt("count", revCount + 1);
  preferences.end();

  Serial.printf("[HISTORY] EXC-%02d session ended: %lus Rp%lu (total=%lu, count=%lu)\n",
                id, paidSec, price, h.totalSec, h.sessions);
}


static const int PKG_COUNT = 7;
int defaultDurations[PKG_COUNT] = {1, 2, 3, 5, 10, 30, 60};

// ── Package CRUD (NVS) ──────────────────────────────────────

void initDefaultPackages() {
  preferences.begin("packages", true);
  int c = preferences.getInt("count", 0);
  preferences.end();
  if (c > 0) return;

  preferences.begin("packages", false);
  for (int i = 0; i < PKG_COUNT; i++) {
    TimePackage pkg;
    pkg.durationMin = defaultDurations[i];
    pkg.priceIDR = 0;
    char k[8];
    snprintf(k, sizeof(k), "p%d", i);
    preferences.putBytes(k, &pkg, sizeof(TimePackage));
  }
  preferences.putInt("count", PKG_COUNT);
  preferences.end();
  Serial.println("[PKG] Default packages initialized (prices=0)");
}

// ══════════════════════════════════════════════════════════════
//  NEW API HANDLERS
// ══════════════════════════════════════════════════════════════
void handleGetPackages() {
  addCorsHeaders();
  preferences.begin("packages", true);
  int count = preferences.getInt("count", 0);
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    TimePackage pkg;
    char k[8];
    snprintf(k, sizeof(k), "p%d", i);
    memset(&pkg, 0, sizeof(TimePackage));
    preferences.getBytes(k, &pkg, sizeof(TimePackage));
    JsonObject obj = array.add<JsonObject>();
    obj["id"] = i;
    obj["durationMin"] = pkg.durationMin;
    obj["priceIDR"] = pkg.priceIDR;
  }
  preferences.end();
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleUpdatePackage() {
  addCorsHeaders();

  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  int pkgId = doc["id"] | -1;
  int price = doc["priceIDR"] | -1;

  if (pkgId < 0 || pkgId >= PKG_COUNT || price < 0) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Invalid id or price\"}");
    return;
  }

  preferences.begin("packages", false);
  TimePackage pkg;
  char k[8];
  snprintf(k, sizeof(k), "p%d", pkgId);
  memset(&pkg, 0, sizeof(TimePackage));
  preferences.getBytes(k, &pkg, sizeof(TimePackage));
  pkg.priceIDR = price;
  preferences.putBytes(k, &pkg, sizeof(TimePackage));
  preferences.end();

  server.send(200, "application/json", "{\"ok\":1}");
  Serial.printf("[PKG] Updated package %d: %dmin = Rp%lu\n", pkgId, pkg.durationMin, price);
}

void handleGetRevenue() {
  addCorsHeaders();

  preferences.begin("revenue", true);
  int count = preferences.getInt("count", 0);
  preferences.end();

  int totalRevenue = 0;
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  // Aggregate by slave
  preferences.begin("revenue", true);
  count = preferences.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    RevenueSession rs;
    char k[16];
    snprintf(k, sizeof(k), "r%d", i);
    memset(&rs, 0, sizeof(RevenueSession));
    if (preferences.getBytes(k, &rs, sizeof(RevenueSession)) > 0) {
      totalRevenue += rs.priceIDR;
    }
  }
  preferences.end();

  // Per-slave breakdown from history
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      HistoryData h;
      loadHistory(slaves[i].id, &h);
      uint32_t slaveRevenue = 0;
      preferences.begin("revenue", true);
      int rc = preferences.getInt("count", 0);
      for (int j = 0; j < rc; j++) {
        RevenueSession rs;
        char k[16];
        snprintf(k, sizeof(k), "r%d", j);
        memset(&rs, 0, sizeof(RevenueSession));
        if (preferences.getBytes(k, &rs, sizeof(RevenueSession)) > 0) {
          if (rs.slaveId == slaves[i].id) slaveRevenue += rs.priceIDR;
        }
      }
      preferences.end();

      JsonObject obj = array.add<JsonObject>();
      obj["id"] = slaves[i].id;
      obj["totalSec"] = h.totalSec;
      obj["sessions"] = h.sessions;
      obj["revenueIDR"] = slaveRevenue;
    }
    xSemaphoreGive(slavesMutex);
  }

  JsonObject total = array.add<JsonObject>();
  total["totalRevenueIDR"] = totalRevenue;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleResetRevenue() {
  addCorsHeaders();


  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));

  int targetId = doc["id"] | 0;

  preferences.begin("revenue", false);
  if (targetId > 0) {
    int count = preferences.getInt("count", 0);
    int dst = 0;
    for (int i = 0; i < count; i++) {
      RevenueSession rs;
      char k[16];
      snprintf(k, sizeof(k), "r%d", i);
      memset(&rs, 0, sizeof(RevenueSession));
      if (preferences.getBytes(k, &rs, sizeof(RevenueSession)) > 0) {
        if (rs.slaveId != targetId) {
          if (dst != i) {
            char dk[16];
            snprintf(dk, sizeof(dk), "r%d", dst);
            preferences.putBytes(dk, &rs, sizeof(RevenueSession));
          }
          dst++;
        }
      }
    }
    preferences.putInt("count", dst);
    Serial.printf("[REVENUE] Reset revenue for EXC-%02d\n", targetId);
  } else {
    preferences.clear();
    Serial.println("[REVENUE] Cleared all revenue");
  }
  preferences.end();

  server.send(200, "application/json", "{\"ok\":1}");
}

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Excavator Rental</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#f0f2f5;--card:#fff;--primary:#2563eb;--success:#10b981;--danger:#ef4444;--warning:#f59e0b;--text:#1e293b;--muted:#64748b;--border:#e2e8f0;--radius:12px}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
.hidden{display:none!important}

/* Header */
.header{background:var(--primary);color:#fff;padding:12px 16px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:10}
.header h2{font-size:16px;font-weight:600}
.header-btn{background:rgba(255,255,255,.2);border:none;color:#fff;padding:6px 12px;border-radius:6px;font-size:13px;cursor:pointer}

/* Tabs */




/* Content */
.content{padding:12px;max-width:600px;margin:0 auto}
.section{margin-bottom:16px}
.section-title{font-size:13px;font-weight:600;color:var(--muted);margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}

/* Device Card */
.device{background:var(--card);border-radius:var(--radius);padding:16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.06)}
.device-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}
.device-name{font-weight:700;font-size:16px}
.device-status{font-size:12px;padding:4px 8px;border-radius:12px;font-weight:600}
.device-status.online{background:#d1fae5;color:#065f46}
.device-status.offline{background:#fee2e2;color:#991b1b}
.device-time{font-size:36px;font-weight:700;text-align:center;font-family:monospace;margin:8px 0}
.device-meta{font-size:12px;color:var(--muted);text-align:center}

/* Package Grid */
.pkg-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:12px}
.pkg-btn{background:var(--card);border:2px solid var(--border);border-radius:var(--radius);padding:12px 8px;text-align:center;cursor:pointer;transition:all .15s}
.pkg-btn:active{transform:scale(.95);border-color:var(--primary);background:#eff6ff}
.pkg-min{font-size:20px;font-weight:700;color:var(--primary)}
.pkg-label{font-size:11px;color:var(--muted);margin-top:2px}
.pkg-price{font-size:12px;font-weight:600;color:var(--success);margin-top:4px}

/* Action Buttons */
.action-row{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px}
.action-row.three{grid-template-columns:1fr 1fr 1fr}
.btn{padding:14px;border:none;border-radius:var(--radius);font-size:14px;font-weight:600;cursor:pointer;text-align:center}
.btn:active{opacity:.85;transform:scale(.98)}
.btn-danger{background:rgba(239,68,68,.1);color:var(--danger);border:1px solid rgba(239,68,68,.3)}
.btn-warning{background:rgba(245,158,11,.1);color:#d97706;border:1px solid rgba(245,158,11,.3)}
.btn-success{background:rgba(16,185,129,.1);color:#059669;border:1px solid rgba(16,185,129,.3)}
.btn-primary{background:var(--primary);color:#fff}
.btn-outline{background:var(--card);color:var(--text);border:1px solid var(--border)}

/* Revenue Card */
.rev-card{background:var(--card);border-radius:var(--radius);padding:16px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 1px 3px rgba(0,0,0,.06)}
.rev-id{font-weight:700}
.rev-sec{font-size:12px;color:var(--muted)}
.rev-money{font-weight:700;color:var(--success);font-size:16px}
.rev-total{background:var(--primary);color:#fff;border-radius:var(--radius);padding:16px;margin-bottom:12px;text-align:center}
.rev-total-label{font-size:13px;opacity:.8}
.rev-total-amount{font-size:28px;font-weight:700}



/* Modal */
.modal-overlay{position:fixed;inset:0;background:rgba(0,0,0,.4);z-index:100;display:none;align-items:flex-end;justify-content:center}
.modal-overlay.active{display:flex}
.modal{background:var(--bg);border-radius:16px 16px 0 0;width:100%;max-width:500px;padding:20px;max-height:80vh;overflow-y:auto}
.modal h3{margin-bottom:16px}
.modal input,.modal select{width:100%;padding:12px;border:1px solid var(--border);border-radius:8px;font-size:15px;margin-bottom:12px;outline:none}
.modal-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}

/* Toast */
.toast{position:fixed;top:60px;left:50%;transform:translateX(-50%);background:var(--card);border:1px solid var(--border);border-radius:8px;padding:12px 20px;font-size:14px;font-weight:600;z-index:200;box-shadow:0 4px 12px rgba(0,0,0,.1);animation:slideIn .3s ease}
@keyframes slideIn{from{opacity:0;transform:translateX(-50%) translateY(-10px)}to{opacity:1;transform:translateX(-50%) translateY(0)}}
</style>
</head>
<body>

</div>

<!-- MAIN APP -->
<div id="mainApp">
  <div class="header">
    <h2 id="headerTitle">🚜 Excavator Rental</h2>
  </div>

    <!-- DASHBOARD TAB -->
  <div id="tab-dashboard" class="content">
    <div id="slaveList"></div>
  </div>

  <!-- REVENUE TAB -->
  <div id="tab-revenue" class="content">
    <div id="revenueTotal"></div>
    <div id="revenueList"></div>
    <div id="resetButtons" style="margin-top:12px">
      <div class="section-title">Reset Data</div>
      <div class="action-row" style="margin-bottom:8px">
        <button class="btn btn-warning" onclick="resetHistory()">Reset Total Waktu</button>
        <button class="btn btn-warning" onclick="resetRevenue()">Reset Pendapatan</button>
      </div>
      <button class="btn btn-danger" style="width:100%" onclick="resetAll()">Reset Semua (Waktu + Uang)</button>
    </div>
  </div>

  </div>
</div>

<!-- MODALS -->
</div>

<div class="modal-overlay" id="modal-editPkg" onclick="closeModal(event,'editPkg')">
  <div class="modal" onclick="event.stopPropagation()">
    <h3>Ubah Harga Paket</h3>
    <div id="editPkgInfo" style="margin-bottom:12px;font-weight:600"></div>
    <input id="editPkgPrice" type="number" placeholder="Harga (Rp)">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModalDirect('editPkg')">Batal</button>
      <button class="btn btn-primary" onclick="savePkgPrice()">Simpan</button>
    </div>
  </div>
</div>

<div class="modal-overlay" id="modal-editSlave" onclick="closeModal(event,'editSlave')">
  <div class="modal" onclick="event.stopPropagation()">
    <h3>Ubah ID Slave</h3>
    <div id="editSlaveInfo" style="margin-bottom:12px;font-weight:600;color:var(--muted)"></div>
    <input id="editSlaveNewId" type="number" placeholder="ID baru (angka)">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModalDirect('editSlave')">Batal</button>
      <button class="btn btn-primary" onclick="saveEditSlave()">Simpan</button>
    </div>
  </div>
</div>

<div class="modal-overlay" id="modal-transfer" onclick="closeModal(event,'transfer')">
  <div class="modal" onclick="event.stopPropagation()">
    <h3>Transfer Waktu</h3>
    <div id="transferInfo" style="margin-bottom:12px;font-weight:600"></div>
    <select id="transferTarget"></select>
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModalDirect('transfer')">Batal</button>
      <button class="btn btn-primary" onclick="doTransfer()">Transfer</button>
    </div>
  </div>
</div>

</div>

<div id="toast" class="toast" style="display:none"></div>

<script>
var S={devices:[],packages:[],revenue:[],history:[],selectedId:null};

function api(ep,opts){
  opts=opts||{};
  var h={'Content-Type':'application/json'};
  
  
  return fetch(ep,{method:opts.m||'GET',headers:h,body:opts.b?JSON.stringify(opts.b):undefined})
    .then(function(r){return r.json().then(function(d){
      return{status:r.status,data:d};
    })});
}

function toast(msg){
  var t=document.getElementById('toast');t.textContent=msg;t.style.display='block';
  setTimeout(function(){t.style.display='none'},2500);
}

// ── Tabs ────────────────────────────────────────────────────


// ── Devices ─────────────────────────────────────────────────
function loadHistory(){
  api('/api/history').then(function(r){
    if(r.status===200){S.history=r.data;renderDevices();}
  });
}

function loadDevices(){
  api('/api/slaves').then(function(r){
    if(r.status===200){
      S.devices=r.data;
      loadHistory();
      loadRevenue();
    }
  });
}

function renderDevices(){
  var c=document.getElementById('slaveList');
  if(!S.devices.length){c.innerHTML='<div class="device"><p style="text-align:center;color:var(--muted)">Belum ada device terdaftar</p></div>';return}
  c.innerHTML=S.devices.map(function(d){
    var online=d.online?'online':'offline';
    var state=d.state||'UNKNOWN';
    var isRunning=state==='RUNNING';
    var isPaused=state==='PAUSED';
    var pkgsHtml='';
    if(S.packages.length){
      pkgsHtml='<div class="pkg-grid">'+S.packages.map(function(p){
        return '<div class="pkg-btn" onclick="addTime('+d.id+','+p.durationMin+')">'+
          '<div class="pkg-min">'+p.durationMin+'</div>'+
          '<div class="pkg-label">menit</div>'+
          '<div class="pkg-price">Rp'+p.priceIDR.toLocaleString('id-ID')+'</div>'+
        '</div>';
      }).join('')+'</div>';
    }
    var actions='';
    actions='<div class="action-row">'+
      (isPaused?'<button class="btn btn-success" onclick="sendCmd('+d.id+',\'RESUME\')">Lanjut</button>':
       isRunning?'<button class="btn btn-warning" onclick="sendCmd('+d.id+',\'PAUSE\')">Jeda</button>':
       '<button class="btn btn-outline" disabled>Tidak Aktif</button>')+
      '<button class="btn btn-danger" onclick="sendCmd('+d.id+',\'STOP\')">Stop</button>'+
    '</div>';
    actions+='<div class="action-row" style="margin-top:4px">'+
      '<button class="btn btn-outline" style="font-size:12px" onclick="identifySlave('+d.id+')">Identify</button>'+
      '<button class="btn btn-outline" style="font-size:12px" onclick="openTransfer('+d.id+')">Transfer</button>'+
    '</div>';
    actions+='<div class="action-row" style="margin-top:4px">'+
      '<button class="btn btn-outline" style="font-size:12px" onclick="openEditSlave('+d.id+')">Edit ID</button>'+
      '<button class="btn btn-outline" style="font-size:12px;color:var(--danger)" onclick="deleteSlave(\''+d.mac+'\')">Hapus</button>'+
    '</div>';
    actions+='<div style="margin-top:4px"><button class="btn btn-outline" style="font-size:12px;width:100%;color:var(--danger)" onclick="rebootSlave('+d.id+')">Reboot</button></div>';
    return '<div class="device">'+
      '<div class="device-header">'+
        '<span class="device-name">EXC-0'+d.id+'</span>'+
        '<span class="device-status '+online+'">'+(online==='online'?state:'OFFLINE')+'</span>'+
      '</div>'+
      '<div class="device-time">'+(d.disp||'--:--')+'</div>'+
      '<div class="device-meta">Sisa: '+Math.floor((d.rem||0)/60)+'j '+((d.rem||0)%60)+'m &bull; Bayar: '+Math.floor((d.paid||0)/60)+'j '+((d.paid||0)%60)+'m</div>'+
      (function(){
        var h=S.history.find(function(x){return x.id===d.id});
        var rev=S.revenue.find(function(x){return x.id===d.id});
        var totalSec=h?h.totalSec:0;
        var sessions=h?h.sessions:0;
        var lastSec=h?h.lastSec:0;
        var revenueIDR=rev?rev.revenueIDR:0;
        if(sessions===0&&revenueIDR===0) return '';
        var hrs=Math.floor(totalSec/3600);
        var mins=Math.floor((totalSec%3600)/60);
        var timeStr=hrs>0?hrs+'j '+mins+'m':mins+' menit';
        return '<div style="background:rgba(37,99,235,.05);border-radius:8px;padding:8px 12px;margin-top:8px;font-size:12px">'+
          '<div style="display:flex;justify-content:space-between;margin-bottom:4px">'+
            '<span style="font-weight:600;color:var(--primary)">Riwayat</span>'+
            '<span style="color:var(--muted)">'+sessions+' sesi</span>'+
          '</div>'+
          '<div style="display:flex;justify-content:space-between;color:var(--muted)">'+
            '<span>Total: '+timeStr+'</span>'+
            '<span>Terakhir: '+Math.floor(lastSec/60)+'m '+lastSec%60+'d</span>'+
          '</div>'+
          (revenueIDR>0?'<div style="text-align:right;margin-top:4px;font-weight:600;color:var(--success)">Rp'+revenueIDR.toLocaleString('id-ID')+'</div>':'')+
        '</div>';
      })()+
      pkgsHtml+actions+
    '</div>';
  }).join('');
}

function addTime(id,minutes){
  api('/api/command',{m:'POST',b:{id:id,cmd:'ADD_TIME',val:minutes}}).then(function(r){
    toast(r.data.ok?'Waktu ditambahkan!':(r.data.error||'Gagal'));
    setTimeout(loadDevices,500);
  });
}

function sendCmd(id,cmd){
  api('/api/command',{m:'POST',b:{id:id,cmd:cmd,val:0}}).then(function(r){
    toast(r.data.ok?'OK':(r.data.error||'Gagal'));
    setTimeout(loadDevices,500);
  });
}

function identifySlave(id){
  api('/api/command',{m:'POST',b:{id:id,cmd:'IDENTIFY',val:0}}).then(function(r){
    toast(r.data.ok?'Buzzer berbunyi!':'Gagal');
  });
}

function rebootSlave(id){
  if(!confirm('Reboot EXC-0'+id+'?'))return;
  api('/api/command',{m:'POST',b:{id:id,cmd:'REBOOT',val:0}}).then(function(r){
    toast(r.data.ok?'Rebooting...':'Gagal');
    setTimeout(loadDevices,5000);
  });
}

function openTransfer(id){
  var others=S.devices.filter(function(d){return d.id!==id&&d.online});
  if(!others.length){toast('Tidak ada device lain online');return}
  var d=S.devices.find(function(x){return x.id===id});
  document.getElementById('transferInfo').textContent='Transfer dari EXC-0'+id+' ('+(d?d.rem:0)+' detik)';
  var sel=document.getElementById('transferTarget');
  sel.innerHTML=others.map(function(x){
    return '<option value="'+x.id+'">EXC-0'+x.id+' ('+x.state+')</option>';
  }).join('');
  sel.dataset.fromId=id;
  document.getElementById('modal-transfer').classList.add('active');
}

function doTransfer(){
  var fromId=parseInt(document.getElementById('transferTarget').dataset.fromId);
  var toId=parseInt(document.getElementById('transferTarget').value);
  closeModalDirect('transfer');
  api('/api/transfer_time',{m:'POST',b:{from_id:fromId,to_id:toId}}).then(function(r){
    toast(r.data.ok?'Transfer berhasil!':(r.data.error||'Gagal'));
    setTimeout(loadDevices,1000);
  });
}

function openEditSlave(id){
  var d=S.devices.find(function(x){return x.id===id});
  document.getElementById('editSlaveInfo').textContent='MAC: '+(d?d.mac:'?');
  document.getElementById('editSlaveNewId').value=id;
  document.getElementById('editSlaveNewId').dataset.mac=d?d.mac:'';
  document.getElementById('modal-editSlave').classList.add('active');
}

function saveEditSlave(){
  var mac=document.getElementById('editSlaveNewId').dataset.mac;
  var newId=parseInt(document.getElementById('editSlaveNewId').value)||0;
  if(newId<1){toast('ID tidak valid');return}
  closeModalDirect('editSlave');
  api('/api/edit_slave',{m:'POST',b:{mac:mac,id:newId}}).then(function(r){
    toast(r.data.ok?'ID diubah!':(r.data.error||'Gagal'));
    setTimeout(loadDevices,500);
  });
}

function deleteSlave(mac){
  if(!confirm('Hapus device '+mac+' dari registry?'))return;
  api('/api/delete_slave',{m:'POST',b:{mac:mac}}).then(function(r){
    toast(r.data.ok?'Device dihapus!':(r.data.error||'Gagal'));
    setTimeout(loadDevices,500);
  });
}





// ── Packages ────────────────────────────────────────────────
function loadPackages(){
  api('/api/packages').then(function(r){
    if(r.status===200){S.packages=r.data;renderDevices();renderPackages();}
  });
}

function renderPackages(){
  var c=document.getElementById('packageList');
  c.innerHTML=S.packages.map(function(p){
    return '<div class="rev-card" onclick="editPkg('+p.id+',\''+p.durationMin+' min\',\''+p.priceIDR+'\')">'+
      '<div><span class="rev-id">Set '+p.durationMin+'</span><span class="rev-sec" style="margin-left:8px">'+p.durationMin+' menit</span></div>'+
      '<div class="rev-money">Rp'+p.priceIDR.toLocaleString('id-ID')+'</div>'+
    '</div>';
  }).join('');
}

function editPkg(id,name,price){
  document.getElementById('editPkgInfo').textContent=name;
  document.getElementById('editPkgPrice').value=price;
  document.getElementById('editPkgPrice').dataset.id=id;
  document.getElementById('modal-editPkg').classList.add('active');
}

function savePkgPrice(){
  var id=parseInt(document.getElementById('editPkgPrice').dataset.id);
  var price=parseInt(document.getElementById('editPkgPrice').value)||0;
  api('/api/packages/update',{m:'POST',b:{id:id,priceIDR:price}}).then(function(r){
    toast(r.data.ok?'Harga updated!':'Gagal');
    closeModalDirect('editPkg');
    loadPackages();
  });
}

// ── Revenue ─────────────────────────────────────────────────
function loadRevenue(){
  api('/api/revenue').then(function(r){
    if(r.status===200){S.revenue=r.data;renderRevenue();renderDevices();}
  });
}

function renderRevenue(){
  var total=0;
  var slaveData=S.revenue.filter(function(x){return x.id!==undefined});
  var totalObj=S.revenue.find(function(x){return x.totalRevenueIDR!==undefined});
  if(totalObj)total=totalObj.totalRevenueIDR;

  document.getElementById('revenueTotal').innerHTML=
    '<div class="rev-total"><div class="rev-total-label">Total Pendapatan</div><div class="rev-total-amount">Rp'+total.toLocaleString('id-ID')+'</div></div>';

  document.getElementById('revenueList').innerHTML=slaveData.map(function(x){
    var s=S.devices.find(function(d){return d.id===x.id});
    return '<div class="rev-card">'+
      '<div><span class="rev-id">EXC-0'+x.id+'</span><span class="rev-sec">'+x.sessions+' sesi &bull; '+Math.floor(x.totalSec/60)+' menit</span></div>'+
      '<div class="rev-money">Rp'+x.revenueIDR.toLocaleString('id-ID')+'</div>'+
    '</div>';
  }).join('');
}

// ── Reset Functions ─────────────────────────────────────────
function resetHistory(){
  if(!confirm('Reset total waktu & sesi untuk semua slave?'))return;
  api('/api/history/reset',{m:'POST',b:{}}).then(function(r){
    toast(r.data.ok?'Total waktu direset!':'Gagal: '+(r.data.error||''));
    if(r.data.ok){loadRevenue();loadDevices();}
  });
}

function resetRevenue(){
  if(!confirm('Reset pendapatan (uang) untuk semua slave?'))return;
  api('/api/revenue/reset',{m:'POST',b:{}}).then(function(r){
    toast(r.data.ok?'Pendapatan direset!':'Gagal: '+(r.data.error||''));
    if(r.data.ok)loadRevenue();
  });
}

function resetAll(){
  if(!confirm('Reset SEMUA data (waktu + uang)?\nTindakan ini tidak bisa dibatalkan!'))return;
  api('/api/reset-all',{m:'POST',b:{}}).then(function(r){
    toast(r.data.ok?'Semua data direset!':'Gagal: '+(r.data.error||''));
    if(r.data.ok){loadRevenue();loadDevices();}
  });
}

// ── Modal ───────────────────────────────────────────────────
function openModal(id){document.getElementById('modal-'+id).classList.add('active')}
function closeModal(e,id){if(e.target===e.currentTarget)closeModalDirect(id)}
function closeModalDirect(id){document.getElementById('modal-'+id).classList.remove('active')}

// ── Init ────────────────────────────────────────────────────
(function(){
  loadDevices();
  loadPackages();
  setInterval(loadDevices,3000);
})();
</script>
</body></html>
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
      obj["rem"] = slaves[i].rem;
      obj["disp"] = slaves[i].disp;
      obj["paid"] = slaves[i].paid;
      obj["totalSeconds"] = slaves[i].paid;
      obj["bat"] = slaves[i].bat;
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

void handleHistory() {
  addCorsHeaders();
  int reqId = server.arg("id").toInt();

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (reqId > 0 && slaves[i].id != reqId) continue;
      HistoryData h;
      loadHistory(slaves[i].id, &h);
      JsonObject obj = array.add<JsonObject>();
      obj["id"] = slaves[i].id;
      obj["mac"] = slaves[i].mac;
      obj["totalSec"] = h.totalSec;
      obj["sessions"] = h.sessions;
      obj["lastSec"] = h.lastSec;
      obj["lastTime"] = h.lastTime;
      obj["online"] = (millis() - slaves[i].lastSeen) < ONLINE_THRESHOLD_MS;
    }
    xSemaphoreGive(slavesMutex);
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleHistoryReset() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  deserializeJson(doc, body);

  int targetId = doc["id"] | 0;

  preferences.begin("history", false);
  if (targetId > 0) {
    char key[16];
    snprintf(key, sizeof(key), "h%d", targetId);
    preferences.remove(key);
    Serial.printf("[HISTORY] Reset EXC-%02d\n", targetId);
  } else {
    preferences.clear();
    Serial.println("[HISTORY] Cleared all history");
  }
  preferences.end();
  server.send(200, "application/json", "{\"ok\":1}");
}

void handleResetAll() {
  addCorsHeaders();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  int targetId = doc["id"] | 0;

  // Reset history
  preferences.begin("history", false);
  if (targetId > 0) {
    char key[16];
    snprintf(key, sizeof(key), "h%d", targetId);
    preferences.remove(key);
  } else {
    preferences.clear();
  }
  preferences.end();

  // Reset revenue
  preferences.begin("revenue", false);
  if (targetId > 0) {
    int count = preferences.getInt("count", 0);
    int dst = 0;
    for (int i = 0; i < count; i++) {
      RevenueSession rs;
      char k[16];
      snprintf(k, sizeof(k), "r%d", i);
      memset(&rs, 0, sizeof(RevenueSession));
      if (preferences.getBytes(k, &rs, sizeof(RevenueSession)) > 0) {
        if (rs.slaveId != targetId) {
          if (dst != i) {
            char dk[16];
            snprintf(dk, sizeof(dk), "r%d", dst);
            preferences.putBytes(dk, &rs, sizeof(RevenueSession));
          }
          dst++;
        }
      }
    }
    preferences.putInt("count", dst);
  } else {
    preferences.clear();
  }
  preferences.end();

  Serial.printf("[RESET-ALL] Reset history+revenue for %s\n", targetId > 0 ? String("EXC-" + String(targetId)).c_str() : "ALL");
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
  int val = doc["val"] | 0;
  if (val < 0) val = 0;

  // Simplified ADD_TIME: val matches duration in minutes (1,2,3,5,10,30,60)
  if (cmd == "ADD_TIME" && val >= 1 && val <= 60) {
    preferences.begin("packages", true);
    int pkgCount = preferences.getInt("count", 0);
    for (int i = 0; i < pkgCount; i++) {
      TimePackage pkg;
      memset(&pkg, 0, sizeof(TimePackage));
      char k[8];
      snprintf(k, sizeof(k), "p%d", i);
      preferences.getBytes(k, &pkg, sizeof(TimePackage));
      if (pkg.durationMin == val) {
        val = pkg.durationMin * 60;
        Serial.printf("[PROXY] Set %dmin resolved to %d seconds (Rp%lu)\n", pkg.durationMin, val, pkg.priceIDR);
        break;
      }
    }
    preferences.end();
  }

  if (targetId <= 0 || cmd == "") {
    Serial.println("[PROXY] Command failed: Missing id or cmd");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Missing id or cmd\"}");
    return;
  }

  Serial.printf("[PROXY] Intercepted command '%s' (val: %d) for EXC-%02d\n", cmd.c_str(), val, targetId);
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

  Serial.printf("[PROXY] Forwarding to %s/api/command\n", targetIp.c_str());
  HTTPClient http;
  http.begin("http://" + targetIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);
  String payload = "{\"cmd\":\"" + cmd + "\",\"val\":" + String(val) + "}";
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

void handleTransferTime() {
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
  int fromId = doc["from_id"] | 0;
  int toId = doc["to_id"] | 0;

  if (fromId <= 0 || toId <= 0) {
    Serial.println("[TRANSFER] Failed: Missing from_id or to_id");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Missing from_id or to_id\"}");
    return;
  }

  if (fromId == toId) {
    Serial.println("[TRANSFER] Failed: Cannot transfer to self");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"Cannot transfer to self\"}");
    return;
  }

  Serial.printf("[TRANSFER] Initiating transfer from EXC-%02d to EXC-%02d\n", fromId, toId);
  netLedFlash(100);

  String fromIp = "";
  String toIp = "";
  if (xSemaphoreTake(slavesMutex, MUTEX_TIMEOUT_TICKS) == pdTRUE) {
    for (int i = 0; i < slaveCount; i++) {
      if (slaves[i].id == fromId) fromIp = slaves[i].ip;
      if (slaves[i].id == toId) toIp = slaves[i].ip;
    }
    xSemaphoreGive(slavesMutex);
  }

  if (fromIp == "" || toIp == "") {
    Serial.println("[TRANSFER] Failed: One or both slaves not found in registry");
    server.send(404, "application/json", "{\"ok\":0,\"error\":\"Slaves not found\"}");
    return;
  }

  HTTPClient http;

  Serial.printf("[TRANSFER] 1. Verifying target EXC-%02d (%s)...\n", toId, toIp.c_str());
  http.begin("http://" + toIp + "/api/state");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int toHttpCode = http.GET();
  http.end();

  if (toHttpCode != 200) {
    Serial.println("[TRANSFER] Failed: Target slave is unreachable");
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Target is unreachable\"}");
    return;
  }

  Serial.printf("[TRANSFER] 2. Fetching state from source EXC-%02d (%s)...\n", fromId, fromIp.c_str());
  http.begin("http://" + fromIp + "/api/state");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    Serial.println("[TRANSFER] Failed: Could not get state from source");
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Failed to get state\"}");
    return;
  }
  String statePayload = http.getString();
  http.end();

  JsonDocument stateDoc;
  if (deserializeJson(stateDoc, statePayload)) {
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Failed to parse state\"}");
    return;
  }
  int rem = stateDoc["rem"] | 0;
  if (rem <= 0) {
    Serial.println("[TRANSFER] Failed: Source has 0 remaining time");
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"No time to transfer\"}");
    return;
  }

  Serial.printf("[TRANSFER] 3. Stopping source EXC-%02d...\n", fromId);
  http.begin("http://" + fromIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int stopCode = http.POST("{\"cmd\":\"STOP\",\"val\":0}");
  http.end();

  if (stopCode != 200) {
    Serial.println("[TRANSFER] Failed: Could not stop source slave");
    server.send(502, "application/json", "{\"ok\":0,\"error\":\"Failed to stop source\"}");
    return;
  }

  Serial.printf("[TRANSFER] 4. Stopping target EXC-%02d to replace time...\n", toId);
  http.begin("http://" + toIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.POST("{\"cmd\":\"STOP\",\"val\":0}");
  http.end();
  delay(200);

  Serial.printf("[TRANSFER] 5. Transferring %d seconds to target EXC-%02d...\n", rem, toId);
  http.begin("http://" + toIp + "/api/command");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int addCode = http.POST("{\"cmd\":\"ADD_TIME\",\"val\":" + String(rem) + "}");
  http.end();

  if (addCode != 200) {
    Serial.println("[TRANSFER] CRITICAL: Target failed to receive time! Attempting to revert to source...");
    http.begin("http://" + fromIp + "/api/command");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);
    int revertCode = http.POST("{\"cmd\":\"ADD_TIME\",\"val\":" + String(rem) + "}");
    http.end();
    if (revertCode != 200) {
      Serial.println("[TRANSFER] FATAL: Revert also failed! Time may be lost!");
      server.send(502, "application/json", "{\"ok\":0,\"error\":\"Transfer failed and revert failed. Time may be lost.\"}");
    } else {
      server.send(502, "application/json", "{\"ok\":0,\"error\":\"Target failed to receive time. Reverted.\"}");
    }
    return;
  }

  Serial.println("[TRANSFER] Complete!");
  server.send(200, "application/json", "{\"ok\":1}");
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
                String oldState = slaves[j].state;
                int oldPaid = slaves[j].paid;

                slaves[j].state = stateDoc["state"] | "UNKNOWN";
                int rem = stateDoc["rem"] | -1;
                if (rem >= 0) slaves[j].rem = rem;
                slaves[j].disp = stateDoc["disp"] | "";
                int paid = stateDoc["paid"] | -1;
                if (paid >= 0) slaves[j].paid = paid;
                slaves[j].bat = stateDoc["bat"] | "";
                slaves[j].lastSeen = millis();

                // Detect session end: was RUNNING/PAUSED, now LOCKED/ENDED
                bool wasActive = (oldState == "RUNNING" || oldState == "PAUSED");
                bool nowInactive = (slaves[j].state == "LOCKED" || slaves[j].state == "ENDED");
                if (wasActive && nowInactive && oldPaid > 0) {
                  recordSessionEnd(slaves[j].id, oldPaid);
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

  initDefaultPackages();

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 10);

  WiFi.onEvent(onApEvent);

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/html", DASHBOARD_HTML);
  });

  // All endpoints (no auth - handled by Android app)
  server.on("/api/register", HTTP_GET, handleRegister);
  server.on("/api/register", HTTP_OPTIONS, handleOptions);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/slaves", HTTP_OPTIONS, handleOptions);
  server.on("/api/packages", HTTP_GET, handleGetPackages);
  server.on("/api/packages", HTTP_OPTIONS, handleOptions);

  // Command & slave management
  server.on("/api/command", HTTP_POST, handleCommandProxy);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.on("/api/transfer_time", HTTP_POST, handleTransferTime);
  server.on("/api/transfer_time", HTTP_OPTIONS, handleOptions);
  server.on("/api/edit_slave", HTTP_POST, handleEditSlave);
  server.on("/api/edit_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete_slave", HTTP_POST, handleDeleteSlave);
  server.on("/api/delete_slave", HTTP_OPTIONS, handleOptions);

  // Package management
  server.on("/api/packages/update", HTTP_POST, handleUpdatePackage);
  server.on("/api/packages/update", HTTP_OPTIONS, handleOptions);

  // History & revenue
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/history", HTTP_OPTIONS, handleOptions);
  server.on("/api/history/reset", HTTP_POST, handleHistoryReset);
  server.on("/api/history/reset", HTTP_OPTIONS, handleOptions);
  server.on("/api/revenue", HTTP_GET, handleGetRevenue);
  server.on("/api/revenue", HTTP_OPTIONS, handleOptions);
  server.on("/api/revenue/reset", HTTP_POST, handleResetRevenue);
  server.on("/api/revenue/reset", HTTP_OPTIONS, handleOptions);
  server.on("/api/reset-all", HTTP_POST, handleResetAll);
  server.on("/api/reset-all", HTTP_OPTIONS, handleOptions);

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

