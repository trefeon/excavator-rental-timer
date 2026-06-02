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

WebServer server(80);
Preferences preferences;
SemaphoreHandle_t slavesMutex = NULL;

static const uint32_t HTTP_TIMEOUT_MS = 2000;
static const uint32_t ONLINE_THRESHOLD_MS = 30000;
static const uint32_t MUTEX_TIMEOUT_TICKS = pdMS_TO_TICKS(500);



#define LED_PIN 2

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

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Excavator Master</title>
  <style>
    :root {
      --bg: #f8fafc;
      --surface: #ffffff;
      --surface-hover: #f1f5f9;
      --primary: #3b82f6;
      --primary-active: #2563eb;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
      --text: #0f172a;
      --text-muted: #64748b;
      --border: #e2e8f0;
      --radius: 16px;
      --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
    
    body {
      background-color: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      -webkit-font-smoothing: antialiased;
      padding: max(16px, env(safe-area-inset-top)) 16px max(16px, env(safe-area-inset-bottom));
      line-height: 1.5;
    }

    .container {
      max-width: 600px;
      margin: 0 auto;
      display: flex;
      flex-direction: column;
      gap: 20px;
    }

    .panel {
      background: rgba(255, 255, 255, 0.8);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 20px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.05);
    }

    h1 { font-size: 24px; font-weight: 700; text-align: center; margin-bottom: 20px; background: linear-gradient(135deg, #2563eb, #3b82f6); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    h2 { font-size: 18px; font-weight: 600; margin-bottom: 12px; }
    p.text-muted { color: var(--text-muted); font-size: 14px; margin-bottom: 16px; }

    .select-wrapper { position: relative; margin-bottom: 24px; }
    select {
      width: 100%;
      background: var(--surface);
      color: var(--text);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
      font-size: 16px;
      font-weight: 600;
      appearance: none;
      outline: none;
      transition: border-color 0.2s, box-shadow 0.2s;
    }
    select:focus { border-color: var(--primary); box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.1); }
    .select-wrapper::after {
      content: "▼";
      position: absolute;
      right: 16px;
      top: 50%;
      transform: translateY(-50%);
      color: var(--text-muted);
      font-size: 12px;
      pointer-events: none;
    }

    .display {
      text-align: center;
      padding: 20px 0;
      margin-bottom: 24px;
      background: rgba(0, 0, 0, 0.03);
      border-radius: 16px;
      border: 1px solid rgba(0, 0, 0, 0.05);
    }
    .time {
      font-family: var(--font-mono);
      font-size: 64px;
      font-weight: 700;
      line-height: 1;
      letter-spacing: -2px;
      margin-bottom: 12px;
      color: var(--text);
      text-shadow: 0 4px 10px rgba(0, 0, 0, 0.05);
    }
    
    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 12px;
      background: var(--surface);
      border-radius: 20px;
      font-size: 13px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      border: 1px solid var(--border);
      box-shadow: 0 2px 5px rgba(0,0,0,0.02);
    }
    .dot { width: 8px; height: 8px; border-radius: 50%; }
    .dot.online { background: var(--success); box-shadow: 0 0 8px rgba(16, 185, 129, 0.6); }
    .dot.offline { background: var(--danger); box-shadow: 0 0 8px rgba(239, 68, 68, 0.6); }
    .dot.paused { background: var(--warning); box-shadow: 0 0 8px rgba(245, 158, 11, 0.6); }

    button {
      background: var(--surface);
      color: var(--text);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      font-family: inherit;
      box-shadow: 0 1px 2px rgba(0,0,0,0.05);
    }
    button:active { transform: scale(0.97); }
    button:disabled { opacity: 0.5; cursor: not-allowed; transform: none; box-shadow: none; }
    
    .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 12px; }
    .grid-1 { display: grid; grid-template-columns: 1fr; gap: 12px; margin-bottom: 12px; }

    .btn-primary { background: var(--primary); border-color: var(--primary); color: white; box-shadow: 0 4px 10px rgba(59, 130, 246, 0.3); }
    .btn-primary:active { background: var(--primary-active); }
    .btn-danger { background: rgba(239, 68, 68, 0.1); border-color: rgba(239, 68, 68, 0.3); color: var(--danger); }
    .btn-danger:active { background: rgba(239, 68, 68, 0.2); }
    .btn-warning { background: rgba(245, 158, 11, 0.1); border-color: rgba(245, 158, 11, 0.3); color: #d97706; }
    .btn-warning:active { background: rgba(245, 158, 11, 0.2); }
    .btn-outline { background: var(--surface); border-color: var(--border); }
    .btn-outline:active { background: var(--surface-hover); }

    .modal-overlay {
      position: fixed; inset: 0; background: rgba(0,0,0,0.4); backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px);
      z-index: 100; opacity: 0; pointer-events: none; transition: opacity 0.3s;
      display: flex; align-items: flex-end;
    }
    .modal-overlay.active { opacity: 1; pointer-events: auto; }
    
    .modal-content {
      width: 100%; max-width: 600px; margin: 0 auto;
      background: var(--bg); border: 1px solid var(--border); border-bottom: none;
      border-radius: 24px 24px 0 0; padding: 24px 20px max(24px, env(safe-area-inset-bottom));
      transform: translateY(100%); transition: transform 0.3s cubic-bezier(0.16, 1, 0.3, 1);
      max-height: 85vh; overflow-y: auto; box-shadow: 0 -10px 40px rgba(0,0,0,0.2);
    }
    .modal-overlay.active .modal-content { transform: translateY(0); }
    
    .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .close-btn { background: var(--surface); border: 1px solid var(--border); width: 36px; height: 36px; border-radius: 50%; display: flex; align-items: center; justify-content: center; padding: 0; color: var(--text-muted); box-shadow: 0 2px 4px rgba(0,0,0,0.05); }

    .device-card {
      background: var(--surface); border: 1px solid var(--border); border-radius: 16px;
      padding: 16px; margin-bottom: 12px; display: flex; flex-direction: column; gap: 12px;
      box-shadow: 0 4px 6px rgba(0,0,0,0.02);
    }
    .device-header { display: flex; justify-content: space-between; align-items: center; }
    .device-title { font-weight: 700; font-size: 16px; display: flex; align-items: center; gap: 8px; }
    .device-mac { font-family: var(--font-mono); font-size: 12px; color: var(--text-muted); }
    
    .action-row { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; }
    .action-row button { padding: 10px; font-size: 14px; border-radius: 8px; font-weight: 600; }

    .target-card {
      background: var(--surface); border: 2px solid transparent; border-radius: 12px;
      padding: 16px; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; cursor: pointer;
      transition: all 0.2s; box-shadow: 0 2px 4px rgba(0,0,0,0.02);
    }
    .target-card.selected { border-color: var(--primary); background: rgba(59, 130, 246, 0.05); }
    .target-info { display: flex; flex-direction: column; gap: 4px; }
    .target-name { font-weight: 700; }
    .target-time { font-family: var(--font-mono); font-size: 13px; color: var(--text-muted); }

    .toast-container { position: fixed; top: 20px; left: 50%; transform: translateX(-50%); z-index: 200; display: flex; flex-direction: column; gap: 8px; width: 90%; max-width: 400px; pointer-events: none; }
    .toast {
      background: var(--surface); border: 1px solid var(--border); color: var(--text); padding: 14px 20px;
      border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.1); font-weight: 600; font-size: 14px;
      display: flex; align-items: center; gap: 12px; animation: slideDown 0.3s cubic-bezier(0.16, 1, 0.3, 1) forwards;
    }
    .toast.success { border-left: 4px solid var(--success); }
    .toast.error { border-left: 4px solid var(--danger); }
    @keyframes slideDown { from { opacity: 0; transform: translateY(-20px) scale(0.95); } to { opacity: 1; transform: translateY(0) scale(1); } }
    .toast.leaving { animation: fadeOut 0.2s ease-in forwards; }
    @keyframes fadeOut { to { opacity: 0; transform: translateY(-10px) scale(0.95); } }

    .loader-overlay {
      position: absolute; inset: 0; background: rgba(255, 255, 255, 0.7); backdrop-filter: blur(2px);
      display: flex; align-items: center; justify-content: center; border-radius: var(--radius);
      z-index: 10; opacity: 0; pointer-events: none; transition: opacity 0.2s;
    }
    .loader-overlay.active { opacity: 1; pointer-events: auto; }
    .spinner { width: 32px; height: 32px; border: 3px solid rgba(0, 0, 0, 0.1); border-top-color: var(--primary); border-radius: 50%; animation: spin 1s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }

    .confirm-overlay {
      position: fixed; inset: 0; background: rgba(0,0,0,0.5); backdrop-filter: blur(4px); -webkit-backdrop-filter: blur(4px);
      z-index: 150; display: none; align-items: center; justify-content: center; padding: 20px;
    }
    .confirm-overlay.active { display: flex; }
    .confirm-box { background: var(--surface); border: 1px solid var(--border); border-radius: 20px; padding: 24px; width: 100%; max-width: 340px; box-shadow: 0 20px 40px rgba(0,0,0,0.15); text-align: center; animation: popIn 0.3s cubic-bezier(0.16, 1, 0.3, 1); }
    .confirm-title { font-size: 18px; font-weight: 700; margin-bottom: 12px; }
    .confirm-msg { color: var(--text-muted); font-size: 14px; margin-bottom: 24px; white-space: pre-line; }
    @keyframes popIn { from { opacity: 0; transform: scale(0.9); } to { opacity: 1; transform: scale(1); } }
    
    .api-log-container {
      background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
      padding: 16px; margin-top: 12px; max-height: 200px; overflow-y: auto; font-family: var(--font-mono);
      font-size: 11px;
    }
    .api-log { margin-bottom: 6px; padding-bottom: 6px; border-bottom: 1px solid var(--border); }
    .api-log.error { color: var(--danger); }
    .api-log.success { color: var(--success); }
    .api-log-time { color: var(--text-muted); margin-right: 8px; font-weight: bold; }
    
    .api-endpoint { margin-bottom: 16px; }
    .api-method { display: inline-block; font-size: 10px; font-weight: 800; padding: 2px 6px; border-radius: 4px; font-family: var(--font-mono); color: white; text-transform: uppercase; margin-right: 6px; }
    .api-method.get { background: var(--success); }
    .api-method.post { background: var(--primary); }
    .api-path { font-family: var(--font-mono); font-weight: 700; font-size: 13px; color: var(--text); }
    .api-desc { font-size: 12px; color: var(--text-muted); margin: 4px 0 6px 0; }
    .api-code { background: rgba(0,0,0,0.03); border: 1px solid var(--border); border-radius: 8px; padding: 8px; font-family: var(--font-mono); font-size: 11px; color: var(--text); display: block; overflow-x: auto; white-space: pre; }
  </style>
</head>
<body>

<div class="container">
  <div class="panel" style="position: relative;">
    <div class="loader-overlay" id="mainLoader"><div class="spinner"></div></div>
    
    <h1>Excavator Master</h1>
    
    <div class="select-wrapper">
      <select id="deviceSelect"></select>
    </div>

    <div class="display">
      <div class="time" id="dispTime">--:--</div>
      <div class="status-badge" id="dispStatus">
        <div class="dot offline"></div>
        <span>OFFLINE</span>
      </div>
    </div>

    <div class="grid-2">
      <button class="btn-outline" onclick="Commands.addTime(300)">+ 5 MIN</button>
      <button class="btn-outline" onclick="Commands.addTime(600)">+ 10 MIN</button>
    </div>
    
    <div class="grid-1">
      <button class="btn-warning" id="btnPause" onclick="Commands.togglePause()">PAUSE</button>
      <button class="btn-danger" onclick="Commands.confirmStop()">STOP / LOCK</button>
    </div>
  </div>

  <div class="grid-2">
    <button class="btn-outline" onclick="Modals.open('transferModal')">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m8 3 4 8 5-5 5 15H2L8 3z"/></svg>
      Transfer
    </button>
    <button class="btn-outline" onclick="Modals.open('manageModal')">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/></svg>
      Manage
    </button>
  </div>
  
  <div style="margin-top: 10px; display: grid; grid-template-columns: 1fr 1fr; gap: 12px;">
    <button class="btn-outline" style="font-size: 13px; color: var(--text-muted);" onclick="UI.toggleApiLog()">Toggle Logs</button>
    <button class="btn-outline" style="font-size: 13px; color: var(--text-muted);" onclick="Modals.open('docsModal')">API Docs</button>
  </div>
  <div id="apiLogContainer" class="api-log-container" style="display: none;"></div>
</div>

<div class="toast-container" id="toastContainer"></div>

<div class="modal-overlay" id="manageModal" onclick="Modals.backdropClick(event, 'manageModal')">
  <div class="modal-content">
    <div class="modal-header">
      <h2>Manage Devices</h2>
      <button class="close-btn" onclick="Modals.close('manageModal')">✕</button>
    </div>
    <div id="slavesList"></div>
  </div>
</div>

<div class="modal-overlay" id="transferModal" onclick="Modals.backdropClick(event, 'transferModal')">
  <div class="modal-content">
    <div class="modal-header">
      <h2>Transfer Time</h2>
      <button class="close-btn" onclick="Modals.close('transferModal')">✕</button>
    </div>
    <p class="text-muted">Move remaining time from <strong style="color:var(--text)">EXC-0<span id="transferSrc"></span></strong> to:</p>
    <div id="transferTargets" style="margin-bottom: 20px;"></div>
    <button class="btn-primary" style="width: 100%" onclick="Commands.executeTransfer()">Complete Transfer</button>
  </div>
</div>

<div class="modal-overlay" id="docsModal" onclick="Modals.backdropClick(event, 'docsModal')">
  <div class="modal-content" style="max-height: 80vh;">
    <div class="modal-header">
      <h2>API Documentation</h2>
      <button class="close-btn" onclick="Modals.close('docsModal')">✕</button>
    </div>
    <div style="display: flex; flex-direction: column; gap: 16px; padding-bottom: 20px;">
      
      <div class="api-endpoint">
        <div><span class="api-method get">GET</span><span class="api-path">/api/slaves</span></div>
        <div class="api-desc">Returns a JSON list of all registered slaves and their status.</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method get">GET</span><span class="api-path">/api/register</span></div>
        <div class="api-desc">Registers a slave to Master and returns its assigned ID. Query param <code>mac</code> is required.</div>
        <div class="api-code">Query: ?mac=80:F3:DA:63:25:DC</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method post">POST</span><span class="api-path">/api/command</span></div>
        <div class="api-desc">Sends commands to a slave (via Master proxy if <code>id</code> is present; directly if POSTed to slave).</div>
        <div class="api-code">Request Body (JSON):
{
  "id": 1, // Optional on Slave
  "cmd": "ADD_TIME", // ADD_TIME, PAUSE, RESUME, STOP, IDENTIFY, REBOOT
  "val": 300 // (seconds)
}</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method post">POST</span><span class="api-path">/api/transfer_time</span></div>
        <div class="api-desc">Transfers remaining timer seconds from one slave to another.</div>
        <div class="api-code">Request Body (JSON):
{
  "from_id": 1,
  "to_id": 2
}</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method post">POST</span><span class="api-path">/api/edit_slave</span></div>
        <div class="api-desc">Changes the assigned ID associated with a slave MAC address.</div>
        <div class="api-code">Request Body (JSON):
{
  "mac": "80:F3:DA:63:25:DC",
  "id": 2
}</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method post">POST</span><span class="api-path">/api/delete_slave</span></div>
        <div class="api-desc">Removes a slave registry record from the system.</div>
        <div class="api-code">Request Body (JSON):
{
  "mac": "80:F3:DA:63:25:DC"
}</div>
      </div>

      <div class="api-endpoint" style="border-top: 1px solid var(--border); padding-top: 12px;">
        <div><span class="api-method get">GET</span><span class="api-path">/api/state</span> (Slave Only)</div>
        <div class="api-desc">Gets real-time state directly from a slave device.</div>
      </div>

    </div>
  </div>
</div>

<div class="confirm-overlay" id="confirmDialog">
  <div class="confirm-box">
    <div class="confirm-title" id="confirmTitle">Confirm</div>
    <div class="confirm-msg" id="confirmMsg">Are you sure?</div>
    <div class="grid-2" style="margin-bottom: 0;">
      <button class="btn-outline" onclick="UI.resolveConfirm(false)">Cancel</button>
      <button class="btn-danger" id="confirmOkBtn" onclick="UI.resolveConfirm(true)">Confirm</button>
    </div>
  </div>
</div>

<script>
var Store = {
  devices: [],
  selectedId: null,
  transferTargetId: null,
  
  updateData: function(data) {
    this.devices = data.sort(function(a, b) { return a.id - b.id; });
    var onlineCount = this.getOnlineCount();
    if (!this.selectedId && onlineCount > 0) {
      var firstOnline = this.devices.find(function(d) { return d.online; });
      if (firstOnline) this.selectedId = firstOnline.id;
    }
  },
  
  getCurrent: function() { 
    var self = this;
    return this.devices.find(function(d) { return d.id == self.selectedId; }); 
  },
  getOnlineCount: function() { 
    return this.devices.filter(function(d) { return d.online; }).length; 
  }
};

var API = {
  log: function(msg, isError) {
    var container = document.getElementById('apiLogContainer');
    if (!container) return;
    var d = new Date();
    var timeStr = d.getHours() + ':' + (d.getMinutes() < 10 ? '0' : '') + d.getMinutes() + ':' + (d.getSeconds() < 10 ? '0' : '') + d.getSeconds();
    var div = document.createElement('div');
    div.className = 'api-log ' + (isError ? 'error' : 'success');
    div.innerHTML = '<span class="api-log-time">[' + timeStr + ']</span> ' + msg;
    container.insertBefore(div, container.firstChild);
    if (container.childNodes.length > 20) {
      container.removeChild(container.lastChild);
    }
  },
  
  fetch: async function(url, options) {
    options = options || {};
    try {
      this.log('REQ ' + (options.method || 'GET') + ' ' + url);
      var res = await fetch(url, options);
      if (!res.ok) {
        throw new Error('HTTP ' + res.status);
      }
      var json = await res.json();
      this.log('RES ' + url + ' -> OK');
      return json;
    } catch (e) {
      this.log('ERR ' + url + ' -> ' + e.message, true);
      throw e;
    }
  },
  
  loadDevices: async function() {
    try {
      var data = await this.fetch('/api/slaves');
      Store.updateData(data);
      UI.render();
    } catch (e) {
      console.error(e);
    }
  },

  postCommand: async function(cmd, val) {
    val = val || 0;
    if (!Store.selectedId) return UI.toast("Select a device first", "error");
    UI.setLoading(true);
    try {
      await this.fetch('/api/command', {
        method: 'POST',
        body: JSON.stringify({ id: Number(Store.selectedId), cmd: cmd, val: val })
      });
      UI.toast("Command sent successfully", "success");
      await this.loadDevices();
    } catch (e) {
      UI.toast("Failed to send command", "error");
    } finally {
      UI.setLoading(false);
    }
  },
  
  manageDevice: async function(endpoint, payload) {
    UI.setLoading(true);
    try {
      await this.fetch(endpoint, {
        method: 'POST',
        body: JSON.stringify(payload)
      });
      UI.toast("Success", "success");
      await this.loadDevices();
    } catch(e) {
      UI.toast("Action failed", "error");
    } finally {
      UI.setLoading(false);
    }
  }
};

var Commands = {
  addTime: function(sec) { API.postCommand('ADD_TIME', sec); },
  togglePause: function() {
    var dev = Store.getCurrent();
    if (!dev) return;
    API.postCommand(dev.state === 'PAUSED' ? 'RESUME' : 'PAUSE', 0);
  },
  confirmStop: function() {
    if (!Store.selectedId) return UI.toast("Select a device first", "error");
    UI.confirm("Stop & Lock EXC-0" + Store.selectedId, "This will clear all remaining time\\nand lock the device immediately.", function() {
      API.postCommand('STOP', 0);
    });
  },
  identify: function(id) {
    API.manageDevice('/api/command', { id: id, cmd: 'IDENTIFY', val: 0 });
  },
  editId: function(mac, oldId) {
    var newId = prompt("Change ID for EXC-0" + oldId + " (Enter new number):", oldId);
    if (newId && newId != oldId && !isNaN(newId)) {
      API.manageDevice('/api/edit_slave', { mac: mac, id: Number(newId) });
    }
  },
  deleteSlave: function(mac) {
    UI.confirm("Delete Device", "Are you sure you want to remove this device from the registry?", function() {
      API.manageDevice('/api/delete_slave', { mac: mac });
    });
  },
  executeTransfer: function() {
    var from = Store.selectedId;
    var to = Store.transferTargetId;
    if (!from || !to) return UI.toast("Select a target device", "error");
    UI.confirm("Confirm Transfer", "Move all remaining time from EXC-0" + from + " to EXC-0" + to + "?\\nThis cannot be undone.", function() {
      Modals.close('transferModal');
      API.manageDevice('/api/transfer_time', { from_id: Number(from), to_id: Number(to) });
    });
  }
};

var UI = {
  init: function() {
    document.getElementById('deviceSelect').addEventListener('change', function(e) {
      Store.selectedId = e.target.value;
      UI.render();
    });
    setInterval(function() { API.loadDevices(); }, 2000);
    API.loadDevices();
  },

  render: function() {
    this.renderSelector();
    this.renderDashboard();
    this.renderManageList();
    this.renderTransferList();
  },

  renderSelector: function() {
    var sel = document.getElementById('deviceSelect');
    if (Store.devices.length === 0) {
      sel.innerHTML = '<option value="">(No devices online)</option>';
      return;
    }
    
    var html = '';
    var hasSelected = false;
    Store.devices.forEach(function(d) {
      if (d.online) {
        var isSel = String(d.id) === String(Store.selectedId);
        if (isSel) hasSelected = true;
        html += '<option value="' + d.id + '" ' + (isSel ? 'selected' : '') + '>EXC-0' + d.id + '</option>';
      }
    });
    
    if (!html) {
      sel.innerHTML = '<option value="">(All devices offline)</option>';
    } else {
      sel.innerHTML = html;
      if (!hasSelected && Store.getOnlineCount() > 0) {
        var firstOnline = Store.devices.find(function(d) { return d.online; });
        if (firstOnline) {
          Store.selectedId = firstOnline.id;
          sel.value = Store.selectedId;
        }
      }
    }
  },

  renderDashboard: function() {
    var dev = Store.getCurrent();
    var timeEl = document.getElementById('dispTime');
    var statusEl = document.getElementById('dispStatus');
    var pauseBtn = document.getElementById('btnPause');
    
    if (dev && dev.online) {
      timeEl.textContent = dev.disp || "--:--";
      
      var dotClass = 'online';
      var stateTxt = dev.state || 'UNKNOWN';
      if (dev.state === 'PAUSED') dotClass = 'paused';
      
      statusEl.innerHTML = '<div class="dot ' + dotClass + '"></div><span>' + stateTxt + '</span>';
      
      if (dev.state === 'PAUSED') {
        pauseBtn.textContent = "RESUME";
        pauseBtn.className = "btn-primary";
      } else {
        pauseBtn.textContent = "PAUSE";
        pauseBtn.className = "btn-warning";
      }
    } else {
      timeEl.textContent = "--:--";
      statusEl.innerHTML = '<div class="dot offline"></div><span>OFFLINE</span>';
    }
  },

  renderManageList: function() {
    var list = document.getElementById('slavesList');
    if (Store.devices.length === 0) {
      list.innerHTML = '<p class="text-muted" style="text-align:center; padding: 20px;">No devices registered yet.<br>Power on a slave to auto-register.</p>';
      return;
    }
    
    list.innerHTML = Store.devices.map(function(d) {
      return '<div class="device-card" style="opacity: ' + (d.online ? '1' : '0.6') + '">' +
        '<div class="device-header">' +
          '<div class="device-title">' +
            '<div class="dot ' + (d.online ? 'online' : 'offline') + '"></div>' +
            'EXC-0' + d.id +
          '</div>' +
          '<div class="device-mac">' + d.mac + '</div>' +
        '</div>' +
        '<div class="device-mac" style="margin-top:-8px; font-weight:600;">IP: ' + (d.ip || '-') + '</div>' +
        '<div class="action-row">' +
          '<button class="btn-outline" style="color:#d97706" onclick="Commands.identify(' + d.id + ')">Ping</button>' +
          '<button class="btn-outline" style="color:var(--primary)" onclick="Commands.editId(\'' + d.mac + '\', ' + d.id + ')">Edit ID</button>' +
          '<button class="btn-danger" onclick="Commands.deleteSlave(\'' + d.mac + '\')">Delete</button>' +
        '</div>' +
      '</div>';
    }).join('');
  },

  renderTransferList: function() {
    var list = document.getElementById('transferTargets');
    document.getElementById('transferSrc').textContent = Store.selectedId || '?';
    
    var targets = Store.devices.filter(function(d) { return d.online && String(d.id) !== String(Store.selectedId); });
    
    if (targets.length === 0) {
      list.innerHTML = '<p class="text-muted" style="text-align:center; padding: 10px;">No other online devices available.</p>';
      return;
    }
    
    list.innerHTML = targets.map(function(d) {
      return '<div class="target-card ' + (Store.transferTargetId == d.id ? 'selected' : '') + '" onclick="UI.selectTransferTarget(' + d.id + ')">' +
        '<div class="target-info">' +
          '<div class="target-name">EXC-0' + d.id + '</div>' +
          '<div class="target-time">' + (d.disp || '--:--') + ' remaining</div>' +
        '</div>' +
        (Store.transferTargetId == d.id ? '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="var(--primary)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>' : '') +
      '</div>';
    }).join('');
  },

  selectTransferTarget: function(id) {
    Store.transferTargetId = id;
    this.renderTransferList();
  },

  setLoading: function(active) {
    var overlay = document.getElementById('mainLoader');
    if (active) overlay.classList.add('active');
    else overlay.classList.remove('active');
  },

  toast: function(msg, type) {
    type = type || "success";
    var container = document.getElementById('toastContainer');
    var el = document.createElement('div');
    
    var icon = type === 'success' ? 
      '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--success)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"></polyline></svg>' : 
      '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--danger)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="12"></line><line x1="12" y1="16" x2="12.01" y2="16"></line></svg>';
    
    el.className = 'toast ' + type;
    el.innerHTML = icon + ' <span>' + msg + '</span>';
    container.appendChild(el);
    
    setTimeout(function() {
      el.classList.add('leaving');
      el.addEventListener('animationend', function() { el.remove(); });
    }, 3000);
  },

  confirmCb: null,
  confirm: function(title, msg, onConfirm) {
    document.getElementById('confirmTitle').textContent = title;
    document.getElementById('confirmMsg').textContent = msg;
    this.confirmCb = onConfirm;
    document.getElementById('confirmDialog').classList.add('active');
  },
  resolveConfirm: function(ok) {
    document.getElementById('confirmDialog').classList.remove('active');
    if (ok && this.confirmCb) this.confirmCb();
    this.confirmCb = null;
  },
  toggleApiLog: function() {
    var c = document.getElementById('apiLogContainer');
    c.style.display = (c.style.display === 'none') ? 'block' : 'none';
  }
};

var Modals = {
  open: function(id) {
    if(id === 'transferModal') {
      Store.transferTargetId = null;
      UI.renderTransferList();
    }
    document.getElementById(id).classList.add('active');
    document.body.style.overflow = 'hidden';
  },
  close: function(id) {
    document.getElementById(id).classList.remove('active');
    document.body.style.overflow = '';
  },
  backdropClick: function(e, id) {
    if (e.target.id === id) this.close(id);
  }
};

document.addEventListener('DOMContentLoaded', function() { UI.init(); });

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

  Serial.printf("[TRANSFER] 4. Transferring %d seconds to target EXC-%02d...\n", rem, toId);
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
                slaves[j].state = stateDoc["state"] | "UNKNOWN";
                int rem = stateDoc["rem"] | -1;
                if (rem >= 0) slaves[j].rem = rem;
                slaves[j].disp = stateDoc["disp"] | "";
                int paid = stateDoc["paid"] | -1;
                if (paid >= 0) slaves[j].paid = paid;
                slaves[j].bat = stateDoc["bat"] | "";
                slaves[j].lastSeen = millis();
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
    server.send(200, "text/html", DASHBOARD_HTML);
  });

  server.on("/api/register", HTTP_GET, handleRegister);
  server.on("/api/register", HTTP_OPTIONS, handleOptions);
  server.on("/api/slaves", HTTP_GET, handleSlaves);
  server.on("/api/slaves", HTTP_OPTIONS, handleOptions);

  server.on("/api/edit_slave", HTTP_POST, handleEditSlave);
  server.on("/api/edit_slave", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete_slave", HTTP_POST, handleDeleteSlave);
  server.on("/api/delete_slave", HTTP_OPTIONS, handleOptions);

  server.on("/api/command", HTTP_POST, handleCommandProxy);
  server.on("/api/command", HTTP_OPTIONS, handleOptions);
  server.on("/api/transfer_time", HTTP_POST, handleTransferTime);
  server.on("/api/transfer_time", HTTP_OPTIONS, handleOptions);

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

