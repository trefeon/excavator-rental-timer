const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML_PATH = path.join(__dirname, '..', 'frontend', 'index.html');

// Read the HTML and extract the script contents
const html = fs.readFileSync(HTML_PATH, 'utf-8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
if (!scriptMatch) {
  console.error("Could not find <script> block in HTML!");
  process.exit(1);
}
const dashboardScript = scriptMatch[1] + "\nwindow.timers = timers; window.slaves = slaves; globalThis.timers = timers; globalThis.slaves = slaves;\n";

// Mock localStorage
class MockLocalStorage {
  constructor() { this.store = {}; }
  getItem(k) { return this.store[k] || null; }
  setItem(k, v) { this.store[k] = String(v); }
  removeItem(k) { delete this.store[k]; }
  clear() { this.store = {}; }
}

// Mock DOM element
class MockElement {
  constructor(id = '') {
    this.id = id;
    this.textContent = '';
    this.value = '';
    this.className = '';
    this.style = { display: '' };
    this.classList = {
      add: (c) => { if (!this.className.includes(c)) this.className += ' ' + c; },
      remove: (c) => { this.className = this.className.replace(c, '').trim(); },
      toggle: (c, cond) => { if (cond) this.classList.add(c); else this.classList.remove(c); }
    };
    this.listeners = {};
    this.innerHTML = '';
    this.children = [];
    this.parent = null;
    this.dataset = { id: '' };
  }
  
  addEventListener(event, callback) {
    if (!this.listeners[event]) this.listeners[event] = [];
    this.listeners[event].push(callback);
  }
  
  trigger(event) {
    if (this.listeners[event]) {
      this.listeners[event].forEach(cb => cb());
    }
  }

  querySelector(selector) {
    // If we're looking for class rs-total/rs-sesi inside stats card, return a mock
    return new MockElement();
  }

  querySelectorAll(selector) {
    // Return children that match classes (simplistic mapping)
    if (selector === '.rc') {
      return this.children;
    }
    return [new MockElement()];
  }
  
  appendChild(child) {
    child.parent = this;
    this.children.push(child);
  }
  
  remove() {
    if (this.parent) {
      this.parent.children = this.parent.children.filter(c => c !== this);
    }
  }
}

// Mock document
const mockElements = {};
const getMockElement = (id) => {
  if (!mockElements[id]) mockElements[id] = new MockElement(id);
  return mockElements[id];
};

const mockDocument = {
  getElementById: (id) => getMockElement(id),
  querySelectorAll: (selector) => {
    return [getMockElement('nb-dashboard'), getMockElement('nb-laporan')];
  },
  createElement: (tag) => new MockElement(tag),
};

// Mock fetch requests registry
let fetchHistory = [];
let mockFetchResponses = {};

const mockFetch = (url, options = {}) => {
  const method = options.method || 'GET';
  const body = options.body ? JSON.parse(options.body) : null;
  fetchHistory.push({ url, method, body });
  
  // Find matching mocked response
  for (const [pattern, resp] of Object.entries(mockFetchResponses)) {
    if (url.includes(pattern)) {
      if (resp.status && resp.status !== 200) {
        return Promise.reject(new Error(`HTTP ${resp.status}`));
      }
      return Promise.resolve({
        ok: resp.ok !== false,
        status: resp.status || 200,
        json: () => Promise.resolve(resp.data)
      });
    }
  }
  
  // Default fallback response
  return Promise.resolve({
    ok: true,
    status: 200,
    json: () => Promise.resolve({ ok: 1 })
  });
};

// Fake Timers and Fake Clock
let activeTimers = [];
let fakeClockTime = 1729000000000; // Fixed start timestamp

const mockSetInterval = (callback, ms) => {
  const id = activeTimers.length + 1;
  activeTimers.push({ id, callback, ms });
  return id;
};
const mockClearInterval = (id) => {
  activeTimers = activeTimers.filter(t => t.id !== id);
};

const tickTimers = (seconds) => {
  for (let i = 0; i < seconds; i++) {
    fakeClockTime += 1000; // Increment fake clock by 1 second
    activeTimers.forEach(t => t.callback());
  }
};

// Setup sandbox context
const sandbox = {
  // Browser Globals
  window: {},
  location: { hostname: 'localhost', search: '?debug=1' },
  document: mockDocument,
  localStorage: new MockLocalStorage(),
  console: {
    log: () => {},
    warn: (...args) => console.log("   [Console Warn]", ...args),
    error: (...args) => console.log("   [Console Error]", ...args)
  },
  AbortController: class { constructor() { this.signal = { aborted: false }; } abort() {} },
  toast: (msg) => console.log("   [Toast]", msg),
  fetch: mockFetch,
  setInterval: mockSetInterval,
  clearInterval: mockClearInterval,
  setTimeout: (cb, ms) => cb(), // Immediate timeout for testing
  clearTimeout: () => {},
  Date: {
    now: () => fakeClockTime
  },
  // Placeholders for external library APIs (like PDF export if any)
  navigator: { userAgent: "mock" },
};

// Run dashboard script in sandbox context
vm.createContext(sandbox);
vm.runInContext(dashboardScript, sandbox);

console.log("====================================================");
console.log("    DASHBOARD E2E LOGIC & EDGE-CASE VALIDATOR SCRIPT  ");
console.log("====================================================\n");

// helper resets
function resetTestState() {
  sandbox.localStorage.clear();
  if (sandbox.slaves) sandbox.slaves.length = 0; else sandbox.slaves = [];
  if (sandbox.timers) { for (let k in sandbox.timers) delete sandbox.timers[k]; } else sandbox.timers = {};
  if (sandbox.statsCache) { for (let k in sandbox.statsCache) delete sandbox.statsCache[k]; } else sandbox.statsCache = {};
  if (sandbox.tfsAlreadyInBase) { for (let k in sandbox.tfsAlreadyInBase) delete sandbox.tfsAlreadyInBase[k]; } else sandbox.tfsAlreadyInBase = {};
  if (sandbox.trxCache) sandbox.trxCache.length = 0; else sandbox.trxCache = [];
  fetchHistory = [];
  mockFetchResponses = {};
  activeTimers = [];
  
  // Set default logged in credentials
  sandbox.localStorage.setItem('rc_u', 'admin');
  sandbox.localStorage.setItem('rc_p', 'admin');
  sandbox.localStorage.setItem('rc_r', 'sa');
}

// -----------------------------------------------------------------------------
// TEST CASE 1: Successful Session Start & Live Uptime Calculations
// -----------------------------------------------------------------------------
resetTestState();
console.log("[Test 1] Starting a normal 5-minute session for EXC-01...");

// Mock stats cache returned from SPIFFS
mockFetchResponses['/api/stats'] = {
  ok: true,
  data: { "1": { "totalDetik": 1000, "totalSesi": 5 } }
};
sandbox.loadStatsFromEsp32();

// Simulate start session trigger
sandbox.setPending(1, "Budi", 5, 25000); // 5 mins, Rp 25k

// Mock command execution API
mockFetchResponses['/api/command'] = {
  ok: true,
  data: { ok: 1, code: "SUCCESS", time_left: 300, state: "RUNNING" }
};

// Trigger starting command E2E
sandbox.sendCmd(1, 'ADD_TIME', 300).then(() => {
  // Assert command request details
  const cmdCall = fetchHistory.find(h => h.url.includes('/api/command'));
  if (cmdCall && cmdCall.body.id === 1 && cmdCall.body.cmd === 'ADD_TIME') {
    console.log("  -> SUCCESS: command POST /api/command dispatched correctly!");
  } else {
    console.log("  -> FAIL: command POST /api/command failed or has wrong body!");
  }

  // Assert local state is running and timer initialized
  console.log("DEBUG: sandbox.timers =", sandbox.timers);
  if (sandbox.timers[1] && sandbox.timers[1].running) {
    console.log("  -> SUCCESS: local timer initialized and running.");
  } else {
    console.log("  -> FAIL: local timer was not initialized!");
  }

  // Simulate active countdown for 30 seconds
  tickTimers(30);

  // Assert timer progress and snapshots
  const snapStr = sandbox.localStorage.getItem('rc_tfs_1');
  const snap = snapStr ? JSON.parse(snapStr) : null;
  if (snap && snap.tfs === 30) {
    console.log(`  -> SUCCESS: timer ticked 30s. tfs snapshot saved properly: tfs=${snap.tfs}`);
  } else {
    console.log("  -> FAIL: timer did not save tfs snapshot correctly!");
  }
});


// -----------------------------------------------------------------------------
// TEST CASE 2: Natural Timer End (ENDED state) and Stats Sync
// -----------------------------------------------------------------------------
setTimeout(() => {
  console.log("\n[Test 2] Simulating natural end of session (timeLeft = 0)...");
  
  // Fast forward remaining 270 seconds
  tickTimers(270);
  
  // Mock the ended state returning from /api/slaves poll
  sandbox.slaves = [
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ];
  
  // Mock API statistics returning the new counts (server auto-saved it)
  mockFetchResponses['/api/stats'] = {
    ok: true,
    data: { "1": { "totalDetik": 1300, "totalSesi": 6 } } // 1000 + 300 played, session +1
  };
  
  // Trigger applySlaves (the ended state transition logic)
  sandbox.applySlaves(sandbox.slaves);
  
  // Assert transaction is saved locally and posted to SPIFFS
  const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
  if (trxCall && trxCall.body.pelanggan === 'Budi' && trxCall.body.menit === 5) {
    console.log("  -> SUCCESS: Transaction recorded automatically on natural session end!");
  } else {
    console.log("  -> FAIL: Transaction failed to record on natural end!");
  }
  
  // Assert pending state is cleared
  const pending = sandbox.localStorage.getItem('rc_pending');
  const hasPending = pending ? JSON.parse(pending)['1'] : null;
  if (!hasPending) {
    console.log("  -> SUCCESS: pending cache cleared successfully.");
  } else {
    console.log("  -> FAIL: pending cache was not cleared!");
  }
}, 50);


// -----------------------------------------------------------------------------
// TEST CASE 3: BUG CHECK - Manual Stop Session Transaction Recording
// -----------------------------------------------------------------------------
setTimeout(() => {
  console.log("\n[Test 3] Simulating manual STOP command during active session (Checking for Bug)...");
  resetTestState();
  
  // Set a pending session
  sandbox.setPending(1, "Andi", 10, 40000);
  
  // Simulate active session
  sandbox.timers[1] = { running: true, tfs: 120, sisa: 480, sessionDone: false };
  
  // Mock STOP command response
  mockFetchResponses['/api/command'] = {
    ok: true,
    data: { ok: 1, code: "SUCCESS", time_left: 0, state: "LOCKED" }
  };
  
  // Trigger manual stop
  sandbox.sendCmd(1, 'STOP', 0).then(() => {
    // Assert if recordTrx was called
    const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
    if (trxCall && trxCall.body.pelanggan === 'Andi') {
      console.log("  -> SUCCESS: Transaction recorded for manual STOP!");
    } else {
      console.log("  -> [BUG DETECTED]: Manual STOP wipes pending cache but NEVER records the transaction!");
    }
  });
}, 100);


// -----------------------------------------------------------------------------
// TEST CASE 4: BUG CHECK - Closed Tab Transaction Loss (Offline Ending)
// -----------------------------------------------------------------------------
setTimeout(() => {
  console.log("\n[Test 4] Simulating browser closed/reopened after session ends (Checking for Bug)...");
  resetTestState();
  
  // 1. Simulate browser had a running session for Cici
  sandbox.localStorage.setItem('rc_pending', JSON.stringify({ "1": { pelanggan: "Cici", menit: 10, harga: 40000 } }));
  sandbox.localStorage.setItem('rc_tfs_1', JSON.stringify({ tfs: 5, savedAt: Date.now(), sisa: 595 }));
  
  // 2. Browser tab is closed and now reopened (fresh load - timers object is empty)
  sandbox.timers = {}; 
  
  // 3. Next poll returns ENDED state (session finished while tab was closed)
  const polledSlaves = [
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ];
  
  // Trigger poll apply
  sandbox.applySlaves(polledSlaves);
  
  // Assert if transaction was recovered and recorded
  const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
  if (trxCall && trxCall.body.pelanggan === 'Cici') {
    console.log("  -> SUCCESS: Closed-tab transaction recovered and written to history!");
  } else {
    console.log("  -> [BUG DETECTED]: If browser is closed when timer ends, the transaction is permanently LOST!");
  }
}, 150);


// -----------------------------------------------------------------------------
// TEST CASE 5: BUG CHECK - Stale Pending Record on Start Failure
// -----------------------------------------------------------------------------
setTimeout(() => {
  console.log("\n[Test 5] Simulating network command failure during startup (Checking for Bug)...");
  resetTestState();
  
  // Start session
  sandbox.setPending(1, "Doni", 5, 25000);
  
  // Mock API returns error / timeout
  mockFetchResponses['/api/command'] = {
    ok: false,
    status: 502,
    data: { ok: 0, error: "Radio send failed" }
  };
  
  // Send start command
  sandbox.sendCmd(1, 'ADD_TIME', 300).then(() => {
    // Assert if pending cache is cleared on failure
    const pending = sandbox.localStorage.getItem('rc_pending');
    const hasPending = pending ? JSON.parse(pending)['1'] : null;
    if (!hasPending) {
      console.log("  -> SUCCESS: Pending session cleared immediately upon start command failure.");
    } else {
      console.log("  -> [BUG DETECTED]: Stale pending record remains in cache if starting command fails!");
    }
    console.log("\n====================================================");
    console.log("                  VALIDATION COMPLETE               ");
    console.log("====================================================");
  });
}, 200);
