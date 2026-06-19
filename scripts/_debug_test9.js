const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML_PATH = path.join(__dirname, '..', 'frontend', 'index.html');
const html = fs.readFileSync(HTML_PATH, 'utf-8');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
const dashboardScript = scriptMatch[1] + `
window.timers  = timers;  globalThis.timers  = timers;
window.slaves  = slaves;  globalThis.slaves  = slaves;
globalThis.__getStatsCache       = () => statsCache;
globalThis.__setStatsCache       = (v) => { statsCache = v; };
globalThis.__getTfsAlreadyInBase = () => tfsAlreadyInBase;
globalThis.__setTfsAlreadyInBase = (v) => { tfsAlreadyInBase = v; };
globalThis.__getTrxCache         = () => trxCache;
`;

class MockLocalStorage {
  constructor() { this.store = {}; }
  getItem(k) { return this.store[k] || null; }
  setItem(k, v) { this.store[k] = String(v); }
  removeItem(k) { delete this.store[k]; }
  clear() { this.store = {}; }
}
class MockElement {
  constructor(id = '') {
    this.id = id; this.textContent = ''; this.value = '';
    this.className = ''; this.style = { display: '' };
    this.classList = { add: () => {}, remove: () => {}, toggle: () => {} };
    this.listeners = {}; this.innerHTML = ''; this.children = [];
    this.parent = null; this.dataset = { id: '' };
  }
  addEventListener(){} trigger(){}
  querySelector(){ return new MockElement(); }
  querySelectorAll(s){ return [new MockElement()]; }
  appendChild(c){ this.children.push(c); }
  remove(){}
}
const mockElements = {};
const getMockElement = (id) => (mockElements[id] = mockElements[id] || new MockElement(id));
const mockDocument = {
  getElementById: (id) => getMockElement(id),
  querySelectorAll: () => [getMockElement('nb-dashboard'), getMockElement('nb-laporan')],
  createElement: () => new MockElement()
};

const sandbox = {
  window: {},
  location: { hostname: 'localhost', search: '?debug=1' },
  document: mockDocument,
  localStorage: new MockLocalStorage(),
  console: { log: (...a) => console.log(...a), warn: () => {}, error: (...a) => console.error(...a) },
  AbortController: class { constructor(){this.signal={aborted:false};} abort(){} },
  toast: () => {},
  fetch: (url, opts = {}) => {
    if (url.includes('/api/stats')) {
      return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ "1": { totalDetik: 600, totalSesi: 3 } }) });
    }
    return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: 1 }) });
  },
  setInterval: (cb, ms) => {}, clearInterval: () => {},
  setTimeout: (cb) => cb(), clearTimeout: () => {},
  Date: class extends Date {
    constructor(...a) { if (!a.length) super(1729000000000); else super(...a); }
    static now() { return 1729000000000; }
  },
  navigator: { userAgent: "mock" },
};

vm.createContext(sandbox);
vm.runInContext(dashboardScript, sandbox);

console.log("=== DEBUG TEST 9 ===\n");

// Setup auth
sandbox.localStorage.setItem('rc_u', 'admin');
sandbox.localStorage.setItem('rc_p', 'admin');
sandbox.localStorage.setItem('rc_r', 'sa');

const sc  = sandbox.__getStatsCache();
const tfb = sandbox.__getTfsAlreadyInBase();

sc['1'] = { totalDetik: 300, totalSesi: 2 };
tfb['1'] = 120;
sandbox.timers[1] = { running: true, tfs: 120, sisa: 0, sessionDone: false };

sandbox.localStorage.setItem('rc_pending', JSON.stringify(
  { '1': { pelanggan: 'Test', menit: 5, harga: 25000, ts: Date.now() } }
));

console.log("BEFORE applySlaves:");
console.log("  pending JSON:", sandbox.localStorage.getItem('rc_pending'));
console.log("  loadPend():", JSON.stringify(JSON.parse(sandbox.localStorage.getItem('rc_pending') || '{}')));
console.log("  pending[1]:", JSON.parse(sandbox.localStorage.getItem('rc_pending'))[1]);
console.log("  pendingObj[1] truthy:", !!JSON.parse(sandbox.localStorage.getItem('rc_pending'))[1]);
console.log("  timers[1]:", JSON.stringify(sandbox.timers[1]));
console.log("  tfb['1']:", tfb['1']);

sandbox.applySlaves([
  { id: 1, mac: "AA:BB:CC:DD:EE:01", online: true, state: "ENDED", time_left: 0, battery: "OK" }
]);

console.log("\nAFTER applySlaves (sync):");
console.log("  timers[1]:", JSON.stringify(sandbox.timers[1]));
console.log("  tfb['1']:", tfb['1']);
console.log("  sc['1']:", JSON.stringify(sc['1']));
console.log("  pending:", sandbox.localStorage.getItem('rc_pending'));