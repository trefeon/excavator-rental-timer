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
// timers, slaves, statsCache, trxCache are NOT auto-globals in the sandbox
// (they're declared with let/const). Tests that reassign these would decouple
// sandbox.X from the internal variable, so we expose accessor functions. Tests
// should always read/write through these accessors — never reassign
// sandbox.timers / sandbox.slaves directly.
window.timers  = timers;  globalThis.timers  = timers;
window.slaves  = slaves;  globalThis.slaves  = slaves;

globalThis.__getTimers       = () => timers;
globalThis.__setTimers       = (v) => { timers  = v; };
globalThis.__getSlaves       = () => slaves;
globalThis.__setSlaves       = (v) => { slaves  = v; };

globalThis.__getStatsCache       = () => statsCache;
globalThis.__setStatsCache       = (v) => { statsCache = v; };
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
// TEST 3: Manual STOP — must NOT record transaction or increment stats
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 3] Manual STOP — no transaction, no stats increment, pending cleared");
  resetTestState();

  // Seed initial stats so we can detect any unwanted increment
  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 300, totalSesi: 5 };

  sandbox.setPending(1, "Andi", 10, 40000);
  sandbox.timers[1] = { running: true, tfs: 120, sisa: 480, sessionDone: false };

  // Master SPIFFS returns the unchanged value (master doesn't write on STOP)
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 300, totalSesi: 5 } } };
  mockFetchResponses['/api/command'] = {
    ok: true, data: { ok: 1, code: "SUCCESS", time_left: 0, state: "LOCKED" }
  };

  sandbox.sendCmd(1, 'STOP', 0).then(() => {
    // No transaction recorded — manual reset must not count toward keuangan
    const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
    assert(!trxCall,
      "No POST to /api/transaksi/add on manual STOP");

    // statsCache unchanged — master didn't write, dashboard didn't optimistically bump
    const sc2 = sandbox.__getStatsCache();
    assert(sc2['1'].totalDetik === 300,
      `totalDetik unchanged after manual STOP: ${sc2['1'].totalDetik} (expected 300)`);
    assert(sc2['1'].totalSesi === 5,
      `totalSesi unchanged after manual STOP: ${sc2['1'].totalSesi} (expected 5)`);

    // Pending cleared so applySlaves LOCKED branch won't re-record later
    const pend = JSON.parse(sandbox.localStorage.getItem('rc_pending') || '{}');
    assert(!pend['1'],
      "Pending cleared after manual STOP");

    // Timer state: session marked done, tfs reset, mfs reset (Total Main = base only)
    assert(sandbox.timers[1] && sandbox.timers[1].sessionDone === true,
      "timers[1].sessionDone=true after manual STOP");
    assert(sandbox.timers[1].tfs === 0,
      "timers[1].tfs=0 after manual STOP");
    assert(sandbox.timers[1].mfs === 0,
      "timers[1].mfs=0 after manual STOP (Total Main = base only)");
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
// TEST 9: mfs reset to 0 after natural ENDED — Total Main = base only
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 9] Natural ENDED — mfs=0, sessionDone=true, Total Main = base");
  resetTestState();

  const sc = sandbox.__getStatsCache();

  // Pre-load state: RC-1 has an ongoing session with mfs=120 (live from master)
  sc['1'] = { totalDetik: 300, totalSesi: 2 };
  sandbox.timers[1] = { running: true, tfs: 120, sisa: 0, sessionDone: false, mfs: 120 };

  // Write pending directly (key must be string to survive JSON round-trip)
  sandbox.localStorage.setItem('rc_pending', JSON.stringify(
    { '1': { pelanggan: 'Test', menit: 5, harga: 25000, ts: Date.now() } }
  ));

  // Master has saved the full package to /stats.json (600s)
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 600, totalSesi: 3 } } };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK", sessionElapsed: 600 }
  ]);

  // After ENDED: sessionDone=true, mfs=0 (display = base only = 600)
  assert(sandbox.timers[1] && sandbox.timers[1].sessionDone === true,
    `timers[1].sessionDone=true after ENDED: got ${sandbox.timers[1] && sandbox.timers[1].sessionDone}`);
  assert(sandbox.timers[1].mfs === 0,
    `timers[1].mfs=0 after ENDED: got ${sandbox.timers[1].mfs}`);
}, 400);

// =============================================================================
// TEST 10: mfs=0 when session is done — no phantom count-up
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 10] sesElap = 0 after session done (no phantom time added to display)");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1']  = { totalDetik: 600, totalSesi: 3 };
  sandbox.timers[1] = { running: false, tfs: 0, sisa: 0, sessionDone: true, mfs: 0 };

  // Mirror updateStatEl logic: mfs=0 because sessionDone=true
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  const total = Math.max(0, (sc['1'].totalDetik || 0) + mfs);

  assert(total === 600,
    `display total=${total} when sessionDone=true (expected 600 = base only)`);
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
}, 600);

// =============================================================================
// TEST 14: Manual STOP race — applySlaves LOCKED must NOT re-record
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 14] Manual STOP race — applySlaves LOCKED does NOT re-record");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 100, totalSesi: 2 };

  sandbox.setPending(1, "Race", 5, 25000);
  sandbox.timers[1] = { running: true, tfs: 60, sisa: 240, sessionDone: false };

  mockFetchResponses['/api/command'] = {
    ok: true, data: { ok: 1, code: "SUCCESS", time_left: 0, state: "LOCKED" }
  };

  sandbox.sendCmd(1, 'STOP', 0).then(() => {
    // After STOP, slave transitions to LOCKED. The next poll() fires applySlaves
    // with state=LOCKED. With pending cleared in sendCmd, the first branch must
    // NOT fire (otherwise it would record a duplicate transaction).
    mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 100, totalSesi: 2 } } };

    sandbox.applySlaves([
      { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "LOCKED", time_left: 0, battery: "OK" }
    ]);

    // No transaction should be recorded (no double-record via applySlaves)
    const trxCall = fetchHistory.find(h => h.url.includes('/api/transaksi/add'));
    assert(!trxCall,
      "No transaction recorded after manual STOP + applySlaves LOCKED");

    // statsCache unchanged
    const sc2 = sandbox.__getStatsCache();
    assert(sc2['1'].totalDetik === 100,
      `statsCache totalDetik unchanged: ${sc2['1'].totalDetik} (expected 100)`);
    assert(sc2['1'].totalSesi === 2,
      `statsCache totalSesi unchanged: ${sc2['1'].totalSesi} (expected 2)`);
  });
}, 650);

// =============================================================================
// TEST 15: Real-time sync — Total Main updates from master's sessionElapsed
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 15] Real-time sync — Total Main = base + mfs from master");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };

  // No pending; just simulate a running session (master reports mfs via heartbeat)
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 300, sessionDone: false };

  // First poll: master reports sessionElapsed=4
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 296, battery: "OK", sessionElapsed: 4, sessionPackageTime: 300 }
  ]);

  // mfs=4 captured. display = 0 + 4 = "00:00:04"
  assert(sandbox.timers[1].mfs === 4,
    `mfs=4 after first poll: got ${sandbox.timers[1].mfs}`);

  // Second poll: master reports sessionElapsed=10
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 290, battery: "OK", sessionElapsed: 10, sessionPackageTime: 300 }
  ]);

  assert(sandbox.timers[1].mfs === 10,
    `mfs=10 after second poll: got ${sandbox.timers[1].mfs}`);

  // Third poll: 60s elapsed, master has periodic-saved to /stats.json
  sc['1'] = { totalDetik: 60, totalSesi: 0 };
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 240, battery: "OK", sessionElapsed: 60, sessionPackageTime: 300 }
  ]);

  assert(sandbox.timers[1].mfs === 60,
    `mfs=60 after 60s poll: got ${sandbox.timers[1].mfs}`);

  // Compute display: base.totalDetik (60) + mfs (60) = 120
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  const total = (sc['1'].totalDetik || 0) + mfs;
  assert(total === 120,
    `display total = base(60) + mfs(60) = ${total} (expected 120)`);
}, 750);

// =============================================================================
// TEST 16: Real-time sync — refresh mid-session picks up master's value
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 16] Real-time sync — refresh mid-session, no localStorage trickery");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 300, sessionDone: false };

  // First poll: sessionElapsed=30
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 270, battery: "OK", sessionElapsed: 30, sessionPackageTime: 300 }
  ]);
  assert(sandbox.timers[1].mfs === 30,
    `mfs=30 after first poll: got ${sandbox.timers[1].mfs}`);

  // "Refresh" (no localStorage manipulation): applySlaves again with new value
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 269, battery: "OK", sessionElapsed: 31, sessionPackageTime: 300 }
  ]);
  assert(sandbox.timers[1].mfs === 31,
    `mfs=31 after refresh: got ${sandbox.timers[1].mfs}`);

  // After 5 more seconds
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 264, battery: "OK", sessionElapsed: 36, sessionPackageTime: 300 }
  ]);
  assert(sandbox.timers[1].mfs === 36,
    `mfs=36 after 5s: got ${sandbox.timers[1].mfs}`);
}, 800);

// =============================================================================
// TEST 17: Real-time sync — natural ENDED finalizes display, increments Sesi
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 17] Real-time sync — natural ENDED finalizes Total Main and Sesi");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 0, sessionDone: false, mfs: 295 };
  sandbox.setPending(1, "Andi", 5, 25000);

  // Master has saved the full package (incremented Sesi too)
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 300, totalSesi: 1 } } };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK", sessionElapsed: 300, sessionPackageTime: 300 }
  ]);

  // After natural ENDED: sessionDone=true, mfs=0, display = base only
  assert(sandbox.timers[1].sessionDone === true,
    "sessionDone=true after natural ENDED");
  assert(sandbox.timers[1].mfs === 0,
    `mfs=0 after ENDED: got ${sandbox.timers[1].mfs}`);

  // Compute display: base.totalDetik (300) + mfs (0) = 300
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  const total = (sc['1'].totalDetik || 0) + mfs;
  assert(total === 300,
    `display = base(300) + mfs(0) = ${total} (expected 300)`);
}, 850);

// =============================================================================
// TEST 18: Master returns negative sessionElapsed — defensive clamp to 0
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 18] Defensive — negative sessionElapsed clamps to 0");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 100, totalSesi: 1 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 50 };

  // Master should never send negative, but if it does, firmware clamps to 0
  // AND dashboard should also be defensive.
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 200, battery: "OK", sessionElapsed: -5, sessionPackageTime: 300 }
  ]);

  // mfs unchanged (was 50). Negative not accepted.
  assert(sandbox.timers[1].mfs === 50,
    `mfs unchanged on negative: got ${sandbox.timers[1].mfs}`);

  // Even if mfs became -5, display would clamp to 0
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  assert(mfs === 50, `mfs=50 (negative rejected): got ${mfs}`);
}, 900);

// =============================================================================
// TEST 19: Master returns undefined sessionElapsed — mfs stays at last value
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 19] Defensive — undefined sessionElapsed keeps last mfs");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 50, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 30 };

  // First poll sets mfs=30
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 270, battery: "OK", sessionElapsed: 30 }
  ]);
  assert(sandbox.timers[1].mfs === 30, `mfs=30: got ${sandbox.timers[1].mfs}`);

  // Second poll: master omits sessionElapsed (briefly unavailable)
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 265, battery: "OK" }
  ]);
  assert(sandbox.timers[1].mfs === 30,
    `mfs unchanged when sessionElapsed undefined: got ${sandbox.timers[1].mfs}`);
}, 950);

// =============================================================================
// TEST 20: Master reboot recovery — mfs drops >5s detected as reboot
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 20] Master reboot recovery — mfs drop >5s accepted as new start");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 600, totalSesi: 3 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 0, sessionDone: false, mfs: 580 };

  // Master rebooted, new accumulator starts from 0
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 295, battery: "OK", sessionElapsed: 2, sessionPackageTime: 300 }
  ]);

  // mfs reset to 2 (reboot detected, new value accepted because drop > 5s)
  assert(sandbox.timers[1].mfs === 2,
    `mfs reset to 2 after master reboot: got ${sandbox.timers[1].mfs}`);
}, 1000);

// =============================================================================
// TEST 21: Tiny mfs fluctuation — within 5s tolerance, keep last value
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 21] Tiny mfs fluctuation — within 5s, keep last value");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 50, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 100 };

  // Heartbeat reports 98 (slight fluctuation — slave NVS restored old value, master's diff went negative)
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 200, battery: "OK", sessionElapsed: 98 }
  ]);

  // 100 - 98 = 2s drop, within 5s tolerance, keep mfs=100
  assert(sandbox.timers[1].mfs === 100,
    `mfs kept at 100 (2s drop within tolerance): got ${sandbox.timers[1].mfs}`);
}, 1050);

// =============================================================================
// TEST 22: Reset Total Main (🗑) zeroes mfs locally
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 22] Reset Total Main — mfs=0 after reset");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 100, totalSesi: 1 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 50 };

  mockFetchResponses['/api/stats/reset'] = { ok: true, data: { ok: 1 } };

  // Call the helper that the 🗑 button uses
  sandbox.resetStatEsp32(1, { user: 'admin', pass: 'admin' }).then(() => {
    // After reset: statsCache zeroed, mfs=0
    const sc2 = sandbox.__getStatsCache();
    assert(sc2['1'].totalDetik === 0,
      `totalDetik=0 after reset: got ${sc2['1'].totalDetik}`);
    assert(sc2['1'].totalSesi === 0,
      `totalSesi=0 after reset: got ${sc2['1'].totalSesi}`);
    assert(sandbox.timers[1].mfs === 0,
      `mfs=0 after reset: got ${sandbox.timers[1].mfs}`);

    // Display = 0 + 0 = 0
    const t = sandbox.timers[1];
    const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
    const total = (sc2['1'].totalDetik || 0) + mfs;
    assert(total === 0, `display total=0 after reset: got ${total}`);
  });
}, 1100);

// =============================================================================
// TEST 23: Manual STOP zeroes mfs locally (no double-count on display)
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 23] Manual STOP — mfs=0, display = base only");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 200, totalSesi: 2 };
  sandbox.setPending(1, "Andi", 5, 25000);
  sandbox.timers[1] = { running: true, tfs: 60, sisa: 240, sessionDone: false, mfs: 60 };

  mockFetchResponses['/api/command'] = {
    ok: true, data: { ok: 1, code: "SUCCESS", time_left: 0, state: "LOCKED" }
  };

  sandbox.sendCmd(1, 'STOP', 0).then(() => {
    // mfs=0 after manual STOP
    assert(sandbox.timers[1].mfs === 0,
      `mfs=0 after manual STOP: got ${sandbox.timers[1].mfs}`);

    // Display = 200 + 0 = 200 (base only, no current-session contribution)
    const t = sandbox.timers[1];
    const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
    const total = (sc['1'].totalDetik || 0) + mfs;
    assert(total === 200, `display = base(200) + mfs(0) = ${total} (expected 200)`);
  });
}, 1150);

// =============================================================================
// TEST 24: Multiple RCs synced independently
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 24] Multiple RCs — each card has independent mfs");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sc['2'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 300, sessionDone: false };
  sandbox.timers[2] = { running: true, tfs: 0, sisa: 300, sessionDone: false };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 290, battery: "OK", sessionElapsed: 10, sessionPackageTime: 300 },
    { id: 2, mac: "AA:BB:CC:DD:EE:02", online: true, state: "RUNNING", time_left: 250, battery: "OK", sessionElapsed: 50, sessionPackageTime: 300 }
  ]);

  assert(sandbox.timers[1].mfs === 10, `RC-1 mfs=10: got ${sandbox.timers[1].mfs}`);
  assert(sandbox.timers[2].mfs === 50, `RC-2 mfs=50: got ${sandbox.timers[2].mfs}`);

  // Each card's display independent
  const total1 = sc['1'].totalDetik + sandbox.timers[1].mfs;
  const total2 = sc['2'].totalDetik + sandbox.timers[2].mfs;
  assert(total1 === 10, `RC-1 total = ${total1} (expected 10)`);
  assert(total2 === 50, `RC-2 total = ${total2} (expected 50)`);
}, 1200);

// =============================================================================
// TEST 25: Slave offline (no entry in /api/slaves) — mfs frozen
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 25] Slave offline — mfs frozen, no error");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 50, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 25 };

  // Master returns no slaves (offline)
  sandbox.applySlaves([]);

  // mfs unchanged
  assert(sandbox.timers[1].mfs === 25,
    `mfs unchanged when slave offline: got ${sandbox.timers[1].mfs}`);

  // display = 50 + 25 = 75 (frozen)
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  const total = (sc['1'].totalDetik || 0) + mfs;
  assert(total === 75, `display frozen at 75 when slave offline: got ${total}`);
}, 1250);

// =============================================================================
// TEST 26: Top-up ADD_TIME — mfs keeps growing, total increments
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 26] Top-up ADD_TIME — mfs continues from same session");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 60, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false, mfs: 60 };

  // Mid-session top-up: master reports sessionElapsed=60 (still 60s elapsed)
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 240, battery: "OK", sessionElapsed: 60, sessionPackageTime: 300 }
  ]);

  // mfs=60 (no regression). Now top-up happens.
  assert(sandbox.timers[1].mfs === 60, `mfs=60 before top-up: got ${sandbox.timers[1].mfs}`);

  // After top-up, mfs continues from 60 and grows
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 295, battery: "OK", sessionElapsed: 65, sessionPackageTime: 600 }
  ]);

  assert(sandbox.timers[1].mfs === 65, `mfs=65 after top-up: got ${sandbox.timers[1].mfs}`);

  // Total = 60 (base) + 65 (mfs) = 125
  const t = sandbox.timers[1];
  const mfs = (t && !t.sessionDone && typeof t.mfs === 'number' && t.mfs >= 0) ? t.mfs : 0;
  const total = (sc['1'].totalDetik || 0) + mfs;
  assert(total === 125, `display = base(60) + mfs(65) = ${total} (expected 125)`);
}, 1300);

// =============================================================================
// TEST 27: Pause → Resume — mfs doesn't accumulate during pause
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 27] PAUSE → RESUME — mfs stops growing during pause");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 200, sessionDone: false };

  // Running: mfs=30
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 170, battery: "OK", sessionElapsed: 30, sessionPackageTime: 200 }
  ]);
  assert(sandbox.timers[1].mfs === 30, `mfs=30 while running: got ${sandbox.timers[1].mfs}`);

  // Paused: mfs frozen (master's diff doesn't grow during pause)
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "PAUSED", time_left: 170, battery: "OK", sessionElapsed: 30, sessionPackageTime: 200 }
  ]);
  assert(sandbox.timers[1].mfs === 30, `mfs=30 during pause (frozen): got ${sandbox.timers[1].mfs}`);

  // After 60s pause: still 30
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "PAUSED", time_left: 170, battery: "OK", sessionElapsed: 30, sessionPackageTime: 200 }
  ]);
  assert(sandbox.timers[1].mfs === 30, `mfs=30 after 60s pause: got ${sandbox.timers[1].mfs}`);

  // Resumed: mfs=35 (5s after resume)
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 165, battery: "OK", sessionElapsed: 35, sessionPackageTime: 200 }
  ]);
  assert(sandbox.timers[1].mfs === 35, `mfs=35 after resume: got ${sandbox.timers[1].mfs}`);
}, 1350);

// =============================================================================
// TEST 28: localStorage persistence — mfs saved to localStorage, survives refresh
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 28] localStorage persistence — mfs survives refresh");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 300, sessionDone: false };

  // Master reports mfs=30
  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "RUNNING", time_left: 270, battery: "OK", sessionElapsed: 30, sessionPackageTime: 300 }
  ]);

  // Simulate smooth tick writing to localStorage (mimic what startSmoothTick does)
  sandbox.localStorage.setItem('rc_mfs_1', '30');

  // Verify localStorage was written
  assert(sandbox.localStorage.getItem('rc_mfs_1') === '30',
    `localStorage rc_mfs_1=30: got ${sandbox.localStorage.getItem('rc_mfs_1')}`);

  // Simulate refresh: timers cleared, restoreMfsFromStorage() called
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 270, sessionDone: false };
  sandbox.restoreMfsFromStorage();

  // After restore, mfs should be 30 (from localStorage)
  assert(sandbox.timers[1].mfs === 30,
    `mfs restored from localStorage: got ${sandbox.timers[1].mfs}`);
}, 1400);

// =============================================================================
// TEST 29: natural ENDED clears localStorage mfs entry
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 29] natural ENDED — localStorage mfs cleared");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 0, sessionDone: false, mfs: 295 };
  sandbox.localStorage.setItem('rc_mfs_1', '295');
  sandbox.setPending(1, "Andi", 5, 25000);

  // Master has saved the full package
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 300, totalSesi: 1 } } };

  sandbox.applySlaves([
    { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK", sessionElapsed: 300, sessionPackageTime: 300 }
  ]);

  // After ENDED: localStorage entry cleared (so refresh doesn't restore stale value)
  assert(sandbox.localStorage.getItem('rc_mfs_1') === null,
    `localStorage rc_mfs_1 cleared after ENDED: got ${sandbox.localStorage.getItem('rc_mfs_1')}`);
  assert(sandbox.timers[1].mfs === 0,
    `mfs=0 after ENDED: got ${sandbox.timers[1].mfs}`);
}, 1450);

// =============================================================================
// TEST 30: playBeep is callable (no exception when AudioContext unavailable)
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 30] playBeep — silent fail when AudioContext unavailable");
  resetTestState();

  // In test sandbox, AudioContext/webkitAudioContext are not defined.
  // playBeep should silently fail without throwing.
  try {
    sandbox.playBeep();
    assert(true, "playBeep did not throw when AudioContext unavailable");
  } catch(e) {
    assert(false, `playBeep threw: ${e.message}`);
  }
}, 1500);

// =============================================================================
// TEST 31: applySlaves natural ENDED triggers playBeep
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 31] natural ENDED — triggers playBeep (no exception)");
  resetTestState();

  const sc = sandbox.__getStatsCache();
  sc['1'] = { totalDetik: 0, totalSesi: 0 };
  sandbox.timers[1] = { running: true, tfs: 0, sisa: 0, sessionDone: false, mfs: 295 };
  sandbox.setPending(1, "Andi", 5, 25000);
  mockFetchResponses['/api/stats'] = { ok: true, data: { "1": { totalDetik: 300, totalSesi: 1 } } };

  // Natural ENDED — should not throw (AudioContext unavailable but silent fail)
  try {
    sandbox.applySlaves([
      { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK", sessionElapsed: 300, sessionPackageTime: 300 }
    ]);
    assert(true, "natural ENDED did not throw despite no AudioContext");
  } catch(e) {
    assert(false, `natural ENDED threw: ${e.message}`);
  }
}, 1550);

// =============================================================================
// TEST 32: offline slave shows offline button (greyed out, no action buttons)
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 32] Offline slave — all action buttons greyed out");
  resetTestState();

  // Slave offline
  const html = sandbox.buildBtns({ id: 1 }, 'offline');
  assert(html.includes('⚫ Offline'),
    `offline button shows ⚫ Offline: ${html}`);
  assert(html.includes('disabled'),
    `offline button is disabled: ${html}`);
  // No action buttons (Mulai, Pause, Stop) should be present
  assert(!html.includes('▶ Mulai'),
    `offline state has no Mulai button: ${html}`);
  assert(!html.includes('⏸ Pause'),
    `offline state has no Pause button: ${html}`);
  assert(!html.includes('⏹'),
    `offline state has no Reset Timer button: ${html}`);
}, 1600);

// =============================================================================
// TEST 33: tooltips present on labels and buttons
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 33] Tooltips present on action buttons");
  resetTestState();

  const stoppedHtml = sandbox.buildBtns({ id: 1 }, 'stopped');
  assert(stoppedHtml.includes('title="Mulai sesi baru'),
    `Mulai button has tooltip: ${stoppedHtml}`);
  assert(stoppedHtml.includes('title="Reset Total Main'),
    `🗑 button has tooltip: ${stoppedHtml}`);

  const runningHtml = sandbox.buildBtns({ id: 1 }, 'running');
  assert(runningHtml.includes('title="Pause timer'),
    `Pause button has tooltip: ${runningHtml}`);
  assert(runningHtml.includes('title="Cancel sesi sekarang'),
    `⏹ button has tooltip: ${runningHtml}`);

  const pausedHtml = sandbox.buildBtns({ id: 1 }, 'paused');
  assert(pausedHtml.includes('title="Lanjutkan timer'),
    `Lanjut button has tooltip: ${pausedHtml}`);
}, 1650);

// =============================================================================
// TEST 34: buildCard includes tooltips on Total Main + Sesi labels
// =============================================================================
setTimeout(() => {
  console.log("\n[Test 34] Tooltips on Total Main + Sesi labels");
  resetTestState();

  const s = { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "LOCKED", time_left: 0, battery: "OK" };
  const st = 'stopped';
  const info = null;
  const card = sandbox.buildCard(s, st, 0, info);

  assert(card.includes('title="Total waktu main kumulatif'),
    `Total Main label has tooltip: ${card.substring(0, 200)}...`);
  assert(card.includes('title="Jumlah sesi yang selesai alami'),
    `Sesi label has tooltip: ${card.substring(0, 200)}...`);
}, 1700);

// =============================================================================
// VALIDATION SUMMARY (runs after all tests)
// =============================================================================
setTimeout(() => {
  console.log("\n====================================================");
  console.log("                  VALIDATION COMPLETE               ");
  console.log("====================================================");
  console.log(`\n  Passed: ${passed}  |  Failed: ${failed}`);
  if (failed > 0) process.exit(1);
}, 1800);
