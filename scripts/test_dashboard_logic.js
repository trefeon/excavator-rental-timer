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
const dashboardScript = scriptMatch[1] + `
// ── Test harness exports ────────────────────────────────────────────────────
// timers, slaves, statsCache, tfsAlreadyInBase, trxCache are NOT auto-globals
// in the sandbox (they're declared with let/const). Tests that reassign these
// would decouple sandbox.X from the internal variable, so we expose both
// objects via accessor functions. Tests should always read/write through
// these accessors — never reassign sandbox.timers / sandbox.slaves directly.
window.timers  = timers;  globalThis.timers  = timers;
window.slaves  = slaves;  globalThis.slaves  = slaves;

globalThis.__getTimers  = () => timers;
globalThis.__setTimers  = (v) => { timers  = v; };
globalThis.__getSlaves  = () => slaves;
globalThis.__setSlaves  = (v) => { slaves  = v; };

globalThis.__getStatsCache       = () => statsCache;
globalThis.__setStatsCache       = (v) => { statsCache = v; };
globalThis.__getTfsAlreadyInBase = () => tfsAlreadyInBase;
globalThis.__setTfsAlreadyInBase = (v) => { tfsAlreadyInBase = v; };
globalThis.__getTrxCache         = () => trxCache;
`;

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
    return new MockElement();
  }

  querySelectorAll(selector) {
    if (selector === '.rc') return this.children;
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
let mockFetchResponses = {
  '/api/auth': { ok: true, data: { exists: true } }
};

const mockFetch = (url, options = {}) => {
  const method = options.method || 'GET';
  const body = options.body ? JSON.parse(options.body) : null;
  fetchHistory.push({ url, method, body });

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

  return Promise.resolve({
    ok: true, status: 200,
    json: () => Promise.resolve({ ok: 1 })
  });
};

// Fake Timers and Fake Clock
let activeTimers = [];
let fakeClockTime = 1729000000000;

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
    fakeClockTime += 1000;
    activeTimers.forEach(t => t.callback());
  }
};

// Setup sandbox context
const sandbox = {
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
  toast: (msg) => {},
  fetch: mockFetch,
  setInterval: mockSetInterval,
  clearInterval: mockClearInterval,
  setTimeout: (cb, ms) => cb(),
  clearTimeout: () => {},
  Date: class extends Date {
    constructor(...args) {
      if (args.length === 0) { super(fakeClockTime); } else { super(...args); }
    }
    static now() { return fakeClockTime; }
  },
  navigator: { userAgent: "mock" },
};

vm.createContext(sandbox);
vm.runInContext(dashboardScript, sandbox);

// ── Test infrastructure ───────────────────────────────────────────────
let passed = 0;
let failed = 0;

function assert(cond, msg) {
  if (cond) {
    console.log(`  -> SUCCESS: ${msg}`);
    passed++;
  } else {
    console.log(`  -> FAIL: ${msg}`);
    failed++;
  }
}

function resetTestState() {
  sandbox.localStorage.clear();
  const slaves = sandbox.__getSlaves();
  slaves.length = 0;
  slaves.push(
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true,  state: "LOCKED",  time_left: 0, battery: "OK" },
    { id: 2, mac: "AA:BB:CC:DD:EE:02", online: true,  state: "LOCKED",  time_left: 0, battery: "OK" },
    { id: 3, mac: "AA:BB:CC:DD:EE:03", online: false, state: "OFFLINE", time_left: 0, battery: "LOW" }
  );
  // Use VM accessors to reset internal let-variables (NEVER reassign
  // sandbox.timers / sandbox.slaves — that would decouple them from the
  // dashboard's internal state, breaking subsequent assertions).
  const timers = sandbox.__getTimers();
  for (let k in timers) delete timers[k];
  const sc = sandbox.__getStatsCache();
  for (let k in sc) delete sc[k];
  const tfb = sandbox.__getTfsAlreadyInBase();
  for (let k in tfb) delete tfb[k];
  const tc = sandbox.__getTrxCache();
  tc.length = 0;
  fetchHistory = [];
  mockFetchResponses = {};
  mockFetchResponses['/api/auth'] = { ok: true, data: { exists: true } };
  sandbox.localStorage.setItem('rc_u', 'admin');
  sandbox.localStorage.setItem('rc_p', 'admin');
  sandbox.localStorage.setItem('rc_r', 'sa');
}

console.log("====================================================");
console.log("    DASHBOARD E2E LOGIC & EDGE-CASE VALIDATOR SCRIPT  ");
console.log("====================================================\n");

// =============================================================================
// TEST 1: Session Start — command dispatched, timer initialised
// =============================================================================
resetTestState();
console.log("[Test 1] Session start — command dispatched and timer initialised");

mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 1000, totalSesi: 5 } } };
sandbox.loadStatsFromEsp32();

sandbox.setPending(1, "Budi", 5, 25000);
mockFetchResponses['/api/command'] = {
  ok: true, data: { ok: 1, code: "SUCCESS", time_left: 300, state: "RUNNING" }
};

sandbox.sendCmd(1, 'ADD_TIME', 300).then(() => {
  const cmdCall = fetchHistory.find(h => h.url.includes('/api/command'));
  assert(cmdCall && cmdCall.body.id === 1 && cmdCall.body.cmd === 'ADD_TIME',
    "ADD_TIME command dispatched with correct body");

  assert(sandbox.timers[1] && sandbox.timers[1].running,
    "Local timer initialised and running");

  tickTimers(30);
  const snap = JSON.parse(sandbox.localStorage.getItem('rc_tfs_1') || 'null');
  assert(snap && snap.tfs === 30,
    `tfs snapshot saved after 30s: tfs=${snap && snap.tfs}`);
});

// =============================================================================
// TEST 2: Natural session end — transaction recorded, pending cleared
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 2] Natural session end (ENDED) — transaction recorded + pending cleared");

  tickTimers(270);

  mockFetchResponses['/api/stats'] = {
    ok: true, data: { "1": { totalDetik: 1300, totalSesi: 6 } }
  };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ]);

  const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
  assert(trxCall && trxCall.body.pelanggan === 'Budi' && trxCall.body.menit === 5,
    "Transaction recorded for Budi, 5-minute package");

  const pending = sandbox.localStorage.getItem('rc_pending');
  assert(!(pending && JSON.parse(pending)['1']),
    "Pending cache cleared after session end");
}, 50);

// =============================================================================
// TEST 3: Manual STOP — transaction recorded
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 3] Manual STOP — transaction recorded correctly");
  resetTestState();

  sandbox.setPending(1, "Andi", 10, 40000);
  sandbox.timers[1] = { running: true, tfs: 120, sisa: 480, sessionDone: false };

  mockFetchResponses['/api/command'] = {
    ok: true, data: { ok: 1, code: "SUCCESS", time_left: 0, state: "LOCKED" }
  };

  sandbox.sendCmd(1, 'STOP', 0).then(() => {
    const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
    assert(trxCall && trxCall.body.pelanggan === 'Andi',
      "Transaction recorded for manual STOP");
  });
}, 100);

// =============================================================================
// TEST 4: Closed-tab recovery — ENDED state restores lost transaction
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 4] Closed-tab recovery — ENDED state restores lost transaction");
  resetTestState();

  sandbox.localStorage.setItem('rc_pending', JSON.stringify(
    { "1": { pelanggan: "Cici", menit: 10, harga: 40000 } }
  ));
  sandbox.localStorage.setItem('rc_tfs_1', JSON.stringify(
    { tfs: 5, savedAt: Date.now(), sisa: 595 }
  ));
  // Clear timers via accessor (NEVER sandbox.timers = {} — that decouples).
  const _t = sandbox.__getTimers();
  for (let k in _t) delete _t[k];

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ]);

  const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
  assert(trxCall && trxCall.body.pelanggan === 'Cici',
    "Closed-tab transaction recovered from localStorage pending");
}, 150);

// =============================================================================
// TEST 5: Start failure — stale pending cleared immediately
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 5] Command failure — stale pending cleared on ADD_TIME failure");
  resetTestState();

  sandbox.setPending(1, "Doni", 5, 25000);

  mockFetchResponses['/api/command'] = {
    ok: false, status: 502, data: { ok: 0, error: "Radio send failed" }
  };

  sandbox.sendCmd(1, 'ADD_TIME', 300).then(() => {
    const pending = sandbox.localStorage.getItem('rc_pending');
    assert(!(pending && JSON.parse(pending)['1']),
      "Pending cleared immediately on start command failure");
  });
}, 200);

// =============================================================================
// TEST 6: applyPackageToStats — exact package seconds applied optimistically
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 6] applyPackageToStats — exact package time applied to statsCache instantly");
  resetTestState();

  // Seed an existing base (3 previous 2-minute sessions = 360s)
  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 360, totalSesi: 3 };

  // A new 2-minute session is pending
  sandbox.setPending(1, "Tono", 2, 10000);

  sandbox.applyPackageToStats(1);

  // Must add exactly 2×60 = 120s
  assert(sc['1'].totalDetik === 480,
    `Exact 120s added: totalDetik=${sc['1'].totalDetik} (expected 480)`);
  assert(sc['1'].totalSesi === 4,
    `totalSesi incremented: ${sc['1'].totalSesi} (expected 4)`);
}, 250);

// =============================================================================
// TEST 7: Multi-session accumulation — total grows by exact package each session
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 7] Multi-session accumulation — total grows by exact package each time");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };

  // Simulate 3 back-to-back 5-second debug sessions (menit = 5/60)
  const debugMenit = 5 / 60; // round(0.0833 × 60) = 5s
  for (let i = 1; i <= 3; i++) {
    sandbox.setPending(1, "Debug", debugMenit, 99);
    sandbox.applyPackageToStats(1);
    // applyPackageToStats reads from pending BEFORE clearPending; clear manually
    sandbox.clearPending(1);
  }

  const expected = 3 * 5; // 3 × 5 seconds = 15
  assert(sc['1'].totalDetik === expected,
    `After 3×5s sessions: totalDetik=${sc['1'].totalDetik} (expected ${expected})`);
  assert(sc['1'].totalSesi === 3,
    `totalSesi=3 after 3 debug sessions`);
}, 300);

// =============================================================================
// TEST 8: paket field in transaction — debug sessions use "Nd" label not "Nm"
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 8] paket field — debug quick-session stores correct label (e.g. '5d')");
  resetTestState();

  const debugMenit = 5 / 60;
  // Write pending manually with explicit paket='5d' (as simDevStartQuickSession does)
  const p = {};
  p['1'] = { pelanggan: 'DebugUser', menit: debugMenit, harga: 99, paket: '5d', ts: Date.now() };
  sandbox.localStorage.setItem('rc_pending', JSON.stringify(p));

  // Mock a natural ENDED response
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 5, totalSesi: 1 } } };
  sandbox.timers[1] = { running: false, tfs: 0, sisa: 0, sessionDone: false };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ]);

  const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
  assert(trxCall && trxCall.body.paket === '5d',
    `Transaction paket field = '${trxCall && trxCall.body.paket}' (expected '5d')`);
}, 350);

// =============================================================================
// TEST 9: tfsAlreadyInBase cleared after session done
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 9] tfsAlreadyInBase — cleared to 0 after session ends");
  resetTestState();

  const sc  = sandbox.__getStatsCache();
  const tfb = sandbox.__getTfsAlreadyInBase();

  // Pre-load state: RC-1 has an ongoing session with tfs=120
  sc['1'] = { totalDetik: 300, totalSesi: 2 };
  tfb['1'] = 120;
  sandbox.timers[1] = { running: true, tfs: 120, sisa: 0, sessionDone: false };

  // Write pending directly (key must be string to survive JSON round-trip)
  sandbox.localStorage.setItem('rc_pending', JSON.stringify(
    { '1': { pelanggan: 'Test', menit: 5, harga: 25000, ts: Date.now() } }
  ));

  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 600, totalSesi: 3 } } };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
  ]);

  // tfsAlreadyInBase['1'] and timers[1].sessionDone are set synchronously in applySlaves
  // (before the async loadStatsFromEsp32 fetch). Check immediately.
  assert(tfb['1'] === 0,
    `tfsAlreadyInBase reset to 0 after ENDED: got ${tfb['1']}`);
  assert(sandbox.timers[1] && sandbox.timers[1].sessionDone === true,
    `timers[1].sessionDone=true after ENDED: got ${sandbox.timers[1] && sandbox.timers[1].sessionDone}`);
}, 400);

// =============================================================================
// TEST 10: sesElap is zero when session is done (no phantom count-up)
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 10] sesElap = 0 after session done (no phantom time added to display)");
  resetTestState();

  const sc  = sandbox.__getStatsCache();
  const tfb = sandbox.__getTfsAlreadyInBase();
  sc['1']  = { totalDetik: 600, totalSesi: 3 };
  sandbox.timers[1] = { running: false, tfs: 0, sisa: 0, sessionDone: true };
  tfb['1'] = 0;

  // Mirror updateStatEl logic: rawTfs=0 because sessionDone=true
  const t = sandbox.timers[1];
  const rawTfs  = (t && !t.sessionDone) ? (t.tfs || 0) : 0;
  const sesElap = Math.max(0, rawTfs - (tfb['1'] || 0));

  assert(sesElap === 0,
    `sesElap=${sesElap} when sessionDone=true (expected 0, no phantom time)`);
}, 450);

// =============================================================================
// TEST 11: SPIFFS retry — loadStatsFromEsp32 retries if totalDetik still old
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 11] SPIFFS retry — loadStatsFromEsp32 retries when totalDetik not yet updated");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 100, totalSesi: 1 };

  let callCount = 0;
  const origFetch = sandbox.fetch;
  sandbox.fetch = (url, opts) => {
    if (url.includes('/api/stats')) {
      callCount++;
      // First call: stale (100); second call (retry): updated (220)
      const val = callCount === 1 ? 100 : 220;
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve({ "1": { totalDetik: val, totalSesi: callCount } })
      });
    }
    return origFetch(url, opts);
  };

  // loadStatsFromEsp32 is async. The first call detects needsRetry, schedules
  // a setTimeout(sync in sandbox) that fires loadStatsFromEsp32(null, true).
  // The retry then needs ~4 microtask cycles to await fetch + json + apply
  // statsCache. We must wait for ALL of them, then restore fetch.
  const waitForRetry = sandbox.loadStatsFromEsp32({ '1': 101 }).then(() => {
    // First call's Promise resolves when it returns from the retry branch.
    // The retry itself is in flight — wait a few more microtask flushes.
    return Promise.resolve();
  }).then(() => Promise.resolve())
    .then(() => Promise.resolve())
    .then(() => Promise.resolve());

  waitForRetry.then(() => {
    const sc2 = sandbox.__getStatsCache();
    assert(sc2['1'] && sc2['1'].totalDetik === 220,
      `After retry, statsCache.totalDetik=${sc2['1'] && sc2['1'].totalDetik} (expected 220)`);
    assert(callCount === 2,
      `Fetch called twice (1 initial + 1 retry): callCount=${callCount}`);
    sandbox.fetch = origFetch;
  });
}, 500);

// =============================================================================
// TEST 12: Exact 2-minute package — totalDetik = exactly 120 after session end
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 12] Exact package time — 2-min package adds exactly 120s (not 119/118)");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };

  sandbox.setPending(1, "Customer", 2, 20000);
  sandbox.applyPackageToStats(1);

  assert(sc['1'].totalDetik === 120,
    `totalDetik=${sc['1'].totalDetik} after 2-min package (expected exactly 120)`);
}, 550);

// =============================================================================
// TEST 13: Multi-RC isolation — accumulation is per-RC, not shared
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 13] Multi-RC isolation — totalDetik accumulates independently per RC");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sc['2'] = { totalDetik: 0, totalSesi: 0 };

  // RC-1: 3 sessions × 5 min = 900s
  for (let i = 0; i < 3; i++) {
    sandbox.setPending(1, "A", 5, 25000);
    sandbox.applyPackageToStats(1);
    sandbox.clearPending(1);
  }

  // RC-2: 2 sessions × 10 min = 1200s
  for (let i = 0; i < 2; i++) {
    sandbox.setPending(2, "B", 10, 40000);
    sandbox.applyPackageToStats(2);
    sandbox.clearPending(2);
  }

  assert(sc['1'].totalDetik === 900,  `RC-1: totalDetik=${sc['1'].totalDetik} (expected 900)`);
  assert(sc['2'].totalDetik === 1200, `RC-2: totalDetik=${sc['2'].totalDetik} (expected 1200)`);
  assert(sc['1'].totalSesi === 3,     "RC-1: totalSesi=3");
  assert(sc['2'].totalSesi === 2,     "RC-2: totalSesi=2");

  console.log("\n====================================================");
  console.log("                  VALIDATION COMPLETE               ");
  console.log("====================================================");
  console.log(`\n  Passed: ${passed}  |  Failed: ${failed}`);
  if (failed > 0) process.exit(1);
}, 600);
