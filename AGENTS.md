# AGENTS.md — Excavator Rental Timer

Operating guide for AI agents and humans working on this repo.
Start here. Read [`README.md`](README.md) for marketing/overview, then `docs/SYSTEM_MANUAL.md` for end-user docs.

---

## 1. What this project is

Mall-based RC-excavator rental timer. **One Master ESP32 + N Slave ESP32s** on an isolated Wi-Fi AP (no internet, no cloud).

- **Master** = Wi-Fi AP + HTTP API gateway + ESP-NOW controller + dashboard host + SPIFFS persistence.
- **Slaves** = Wi-Fi STA + ESP-NOW client + relay/MOSFET + TM1637 display + buzzer + button + NVS for powerloss recovery.
- **Clients** = browser dashboard (single-file HTML) and/or Android app. Both talk HTTP only to Master at `192.168.4.1`.

Master is a **bridge/proxy** — no business logic, no pricing logic, no history view. The browser/Android owns the UI; SPIFFS owns persistence; firmware owns state machines.

---

## 2. Repository layout

```
excavator-rental-timer/
├── README.md                    Marketing / overview (refer to this for end-user intro)
├── platformio.ini               5 PlatformIO envs (master / master_html / slave / slave_c3 / slave8266)
├── build_release.py             Drives PlatformIO + esptool merge + PyInstaller flash.exe
├── pytest.ini                   pytest config
├── firmware/                    PlatformIO projects — one folder per env
│   ├── build_frontend.py        Gzips frontend/index.html → wifi_master_esp32_WEBUI/index_html.h
│   ├── wifi_master_esp32_WEBUI/  ★ preferred master (deferred SPIFFS writes, embedded dashboard)
│   ├── wifi_master_esp32_nonWEBUI/  master-only API server (smaller, no dashboard)
│   ├── wifi_slave_esp32/         standard ESP32 slave (production pinout, 19.5 dBm)
│   ├── wifi_slave_c3/            ESP32-C3 Super Mini slave (8.5 dBm, USB-CDC)
│   └── wifi_slave_8266/          legacy HTTP-only slave (no ESP-NOW) — keep as fallback only
├── frontend/
│   ├── index.html               ★ single-file dashboard (HTML + CSS + JS, ~113 KB)
│   └── android_reference/       Kotlin OkHttp wrapper for the Android app
├── tests/                       pytest — Python mirror of protocol + master logic
│   ├── conftest.py
│   ├── protocol_lib.py          byte-identical mirror of firmware/.../esp_now_protocol.h
│   ├── test_protocol.py         packet/enum/slave-state-machine tests
│   ├── test_master_persistence.py  MAC key length, transaksi cutoffs, stats add/reset
│   └── test_master_logic.py     heartbeat handler + demo-mode loop
├── scripts/
│   ├── test_dashboard_logic.js  ★ vm-sandboxed E2E tests of frontend/index.html
│   ├── mock_master_server.py    HTTP mock of Master on :8080 (for dashboard dev)
│   ├── monitor.py               3-port serial monitor
│   ├── com_test.py              smoke-test Master command + read 3 COM ports
│   ├── keygen.py                Android app HMAC activation codes
│   ├── test_logic.py / test_real_hardware.py  end-to-end against a real Master
│   ├── test_esptool.py          import smoke test
│   └── scratch.py               load/stress test (registers 9 slaves + 3 clients + attacker)
├── release/                     frozen binaries per version + standalone flash.exe
│   ├── flash.py / flash.spec / flash.bat / README.txt   PyInstaller flasher source
│   ├── v1.1.0/ … v1.2.4/        per-version artifacts (4 merged .bin + flash.exe + ZIP)
│   └── latest/                  symlink-equivalent of newest version
├── docs/                        user-facing docs + API spec (see §8)
├── android_app/                 compiled APK + jadx (binary)
├── test_flasher/                copy of release/latest/ for local use
└── archive/                     ⚠ read-only history — DO NOT use for code work
    ├── old_master.ino, old_slave.ino/.cpp/.txt
    ├── old_docs/  (MVP_SPEC.md, PRD.md, WIFI_API_SPEC.md, MERMAID_DIAGRAMS.md, …)
    ├── DashboardRCNew.html, RCDashboard.html
    ├── arduino-cli/ + arduino-cli.zip  (vendored, not part of current build)
    └── PROPOSAL*.docx, PROPOSAL.md  (commercial/business docs)
```

**Build-tracked only what `.gitignore` whitelists.** `archive/`, `test_flasher/`, `android_app/*.zip`, build artifacts are either history or binaries.

---

## 3. Architecture at a glance

```
[Browser/Android]                                   [Master ESP32 @ 192.168.4.1]
        │  HTTP (only)                                       │
        ├──────────────────────────────────────────────────►│ AP + WebServer(80)
        │  GET /api/slaves                                  │ DNS captive portal :53
        │  POST /api/command                                │ SPIFFS: /auth /stats /transaksi
        │  GET/POST /api/auth, /api/stats, /api/transaksi   │ NVS: registry, id_map, lastDate
        │  GET /                                            │
        │     ▼ gzipped index_html_gz[]                     │
        └──────────────────────────────────────────────────┘
                                                            │ ESP-NOW (channel 1, broadcast)
                                                            │ 24-byte packed packets
                                                            │ PKT_REGISTER_REQ/RESP, COMMAND,
                                                            │ COMMAND_RESP, HEARTBEAT
                                                            ▼
                                                    [Slave ESP32 #1..N]
                                                    TM1637 + MOSFET + Buzzer + Button
                                                    NVS powerloss recovery
                                                    core 0: networkTask (heartbeat)
                                                    core 1: loop() (timer + flash save)
```

Two masters exist. **Always prefer WEBUI** (`wifi_master_esp32_WEBUI/`):
- WEBUI defers all SPIFFS writes via a FreeRTOS `statsQueue` drained in `loop()`. NonWEBUI writes inside the ESP-NOW callback (race-prone).
- WEBUI has `statsMutex` protecting `/stats.json` RMW.
- WEBUI embeds the gzipped dashboard via `index_html_gz[]`.

The nonWEBUI variant exists only as a smaller-footprint alternative.

---

## 4. Key constants (don't reinvent)

| Constant | Value | Where |
|---|---|---|
| Master AP SSID / pass / IP | `ExcavatorMaster` / `12345678` / `192.168.4.1` | master main.cpp |
| ESP-NOW channel | `1` | `esp_now_protocol.h` |
| ESP-NOW broadcast MAC | `FF:FF:FF:FF:FF:FF` | `esp_now_protocol.h` |
| Packet size | **24 bytes** (packed struct, enforced by test) | protocol.h |
| Slave ID range | 1..50 | master |
| `MAX_ADD_TIME_MINUTES` | 480 (8 h) | slave + protocol_lib |
| `MAX_REMAINING` | 28800 (8 h) | slave + protocol_lib |
| `FLASH_SAVE_INTERVAL_S` (slave) | 10 | slave |
| `HEARTBEAT_INTERVAL_MS` (slave) | 2000 | slave |
| `ONLINE_THRESHOLD_MS` (master) | 30000 | master |
| `CMD_RESPONSE_TIMEOUT_MS` (master) | 500 | master |
| Stats: `/stats.json` per slave | `{totalDetik, totalSesi}` | master |
| Transaksi cutoff | **7 days, max 300 records** | master (note: `tests/test_master_persistence.py` mirrors older 3-day/500 values — legacy) |
| Stats `sesi` flag | `1` = auto-increment (default), `0` = manual STOP (no increment) | master `handleAddStats` |

### ESP-NOW packet types
```
PKT_REGISTER_REQ   = 1
PKT_REGISTER_RESP  = 2
PKT_COMMAND        = 3
PKT_COMMAND_RESP   = 4
PKT_HEARTBEAT      = 5
```
Commands: `ADD_TIME=1, PAUSE=2, RESUME=3, STOP=4, REBOOT=5, IDENTIFY=6`.
Response codes: `OK=0, BAD_STATE=1, EXCEEDS_LIMIT=2, UNKNOWN_COMMAND=3, REBOOTING=6`.
States: `LOCKED=0, RUNNING=1, PAUSED=2, ENDED=4, FAULT=5` (gap at 3 is intentional).

### Master HTTP API (full live surface)
```
GET   /api/slaves                  fleet status — includes sessionElapsed, sessionPackageTime
POST  /api/command                 body {id, cmd, time}
... (see below)
```
**`/api/slaves` per-element fields** (live, updated every 2 s via heartbeat):
```json
{
  "id": 1, "ip": "", "mac": "AA:BB:CC:DD:EE:01",
  "online": true,
  "state": "RUNNING" | "PAUSED" | "LOCKED" | "ENDED",
  "time_left": 596, "battery": "OK",
  "sessionElapsed": 4,          ← live accumulator, 2s heartbeat cadence
  "sessionPackageTime": 600      ← full package set on ADD_TIME
}
```
Dashboard uses `sessionElapsed` directly for live `Total Main` display (no math, no slave NVS).

### Master HTTP API (full live surface — `openapi.yaml` is stale, only documents 4 legacy endpoints)
```
GET   /                            serves gzipped dashboard
GET   /api/slaves                  fleet status
POST  /api/command                 body {id, cmd, time}
GET   /api/register?mac=...        legacy HTTP-slave heartbeat
POST  /api/edit_slave              SA only — body {mac, id}
POST  /api/delete_slave            SA only — body {mac}
POST  /api/reset_registry          SA only — clear NVS + in-memory
GET   /api/auth                    {exists:bool}
POST  /api/auth/register           create SA (only if none exists)
POST  /api/auth/login              {ok, role} — rate-limited 5 fails → 30 s
POST  /api/auth/verify-sa          verify SA password only (for kr to escalate)
POST  /api/auth/change-pass
GET   /api/karyawan                SA only
POST  /api/karyawan/add            SA only
POST  /api/karyawan/delete         SA only
GET   /api/stats
POST  /api/stats/add               body {id, detik, sesi?}  sesi defaults 1
POST  /api/stats/reset             SA only — body {id}
POST  /api/sync-time               body {date:"YYYY-MM-DD"}; on date change zeros totalSesi for all
GET   /api/transaksi
POST  /api/transaksi/add           body {id, rcId, rcNama, pelanggan, menit, harga, paket, ts}
```
All non-`/api/auth` routes require `X-Admin-User` + `X-Admin-Pass` headers. CORS enabled.

### NVS / SPIFFS keys (master)
- namespace `registry`: MAC-without-colons (12 chars, fits 15-char NVS limit) → uint ID, plus `"lastDate"` key.
- namespace `id_map`: string `"1"`.."50" → MAC (reverse lookup for ID swap).
- LittleFS files: `/auth.json`, `/stats.json`, `/transaksi.json`. (formerly SPIFFS, migrated in v1.2.5+ for wear-levelling + atomic writes — see §8.)
- Heap guard on transaksi: if free heap `< 80000` → 503 `LOW_HEAP`.
- Periodic stats save during `RUNNING` session: every **30 s**, enqueue `StatsJob{sessionElapsed - sessionSavedElapsed, false}`. Tracked per slave via `sessionSavedElapsed` and `lastStatsSaveMs` fields. Wear-budget check: 9 slaves × 30 s × 8 h ≈ 8 600 erases/day across the wear-levelled partition = ~30 year lifetime with LittleFS.

---

## 5. Files that must stay byte-identical

`firmware/wifi_master_esp32_WEBUI/esp_now_protocol.h` ≡ `wifi_master_esp32_nonWEBUI/esp_now_protocol.h` ≡ `wifi_slave_esp32/esp_now_protocol.h` ≡ `wifi_slave_c3/esp_now_protocol.h`. (The 8266 slave does NOT use this header — it's HTTP-based.) If you change enums, struct layout, or helpers, copy to all 4.

`tests/protocol_lib.py` is a faithful Python mirror of the same header — `EspNowPacket.pack()/unpack()` must produce byte-identical output. If you change one, change the other in the same commit. The 24-byte size is load-bearing (`test_protocol.py::test_packet_size_is_24`).

---

## 6. Build, test, run

### Build firmware

```bash
# 1. (WEBUI master only) gzip the dashboard into a C header
python firmware/build_frontend.py
# → firmware/wifi_master_esp32_WEBUI/index_html.h

# 2. Build one or more envs
~/.platformio/penv/bin/platformio run -e master_html       # WEBUI master (recommended)
~/.platformio/penv/bin/platformio run -e master            # non-WEBUI master
~/.platformio/penv/bin/platformio run -e slave             # ESP32 slave
~/.platformio/penv/bin/platformio run -e slave_c3          # ESP32-C3 slave
# Outputs → .pio/build/<env>/{bootloader.bin, partitions.bin, firmware.bin, ...}
```

### Run tests

```bash
# Python — protocol + master logic
pytest                                                    # all
pytest tests/test_protocol.py -v
pytest tests/test_master_persistence.py -v
pytest tests/test_master_logic.py -v

# Node — dashboard logic (vm-sandboxed E2E)
node scripts/test_dashboard_logic.js
# Exit code 0 = all pass; 1 = any fail. Prints Passed/Failed counts.

# Mock master (for dashboard UI dev against desktop)
python scripts/mock_master_server.py
# Open http://localhost:8080

# Hardware smoke tests
python scripts/test_logic.py            # mock slave + real Master @ 192.168.4.1
python scripts/test_real_hardware.py    # real ESP32 slaves
python scripts/com_test.py              # POST /api/command + read 3 COM ports
python scripts/monitor.py               # 3-port serial monitor
```

### Cut a release

```bash
python build_release.py v1.2.5
# → release/v1.2.5/{flash.exe, master_WEBUI_merged.bin, master_nonWEBUI_merged.bin,
#                    slave_esp32_merged.bin, slave_c3_merged.bin, README.txt,
#                    Excavator_Firmware_Flasher.zip}
# → release/latest/  (mirror)
```
Offsets are ESP32: `0x1000/0x8000/0xe000/0x10000`; ESP32-C3: `0x0000/0x8000/0xe000/0x10000`.

---

## 7. Common tasks — recipes

### Add a new HTTP endpoint on master
1. Add route in `setup()` (search `server.on(` in master `main.cpp`).
2. Add handler `handleXxx()` matching existing style (auth check via `authCheck(needsSA)`; CORS headers; `server.sendHeader("Access-Control-Allow-Origin", "*")`).
3. Mirror logic in `tests/test_master_persistence.py` or `tests/test_master_logic.py` if it has non-trivial semantics.
4. Update `docs/openapi.yaml` (or note in commit message that it's intentionally stale — most endpoints only live in C++ source).
5. If frontend uses it, add to `frontend/index.html` and re-run `python firmware/build_frontend.py`.

### Change a packet enum / wire format
1. Update `firmware/wifi_master_esp32_WEBUI/esp_now_protocol.h` (master) — it's the canonical copy.
2. Copy to the other 3 `esp_now_protocol.h` files.
3. Mirror in `tests/protocol_lib.py` (constants + struct).
4. Update `tests/test_protocol.py` if enum semantics changed.
5. Update master/slave `main.cpp` switch statements.

### Change dashboard UI
1. Edit `frontend/index.html` only. Don't touch `firmware/wifi_master_esp32_WEBUI/index_html.h` — it's auto-generated.
2. Run `python firmware/build_frontend.py` to regenerate the header.
3. Run `node scripts/test_dashboard_logic.js` to confirm the JS still passes.
4. Rebuild master_html env.

### Add a new slave hardware variant
- Copy `firmware/wifi_slave_esp32/` to a new folder.
- Add `[env:your_slave]` block in `platformio.ini` with `build_src_filter = -<*> +<your_slave_folder/>` and your board + platform settings.
- Edit pinout lines 1–37 of your new `main.cpp` and any platform-specific flags (e.g. C3 needs `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`).
- Copy `esp_now_protocol.h` from `wifi_slave_esp32/`.
- Add to `TARGETS` list in `build_release.py` with correct `chip` + offsets.
- Document pinout in `docs/WIRING_RC_EXCAVATOR.md`.

### Debug a stuck master
- Watch serial @ 115200 baud (LED on GPIO 2 blinks every 3 s = alive).
- `DEMO_MODE = 1` injects 5 fake slaves (`FF:FF:FF:00:00:01..03` online, `EE:EE:EE:00:00:04..05` offline) — useful to validate UI without real hardware.
- Hardcoded AP creds (`ExcavatorMaster` / `12345678`) — if you change them, also update Android reference and `docs/ANDROID_APP_GUIDE.md`.

### Add tests for new dashboard logic
- `scripts/test_dashboard_logic.js` uses `vm.runInContext()` with mocked `localStorage`/`document`/`fetch`/`Date`/`setInterval`/`setTimeout`.
- Internal `let` variables (`statsCache`, `tfsAlreadyInBase`, `trxCache`) must be exposed via accessor functions at the bottom of the appended script block (already done — see `__getStatsCache` etc.).
- Each test runs in its own `setTimeout(..., 50*i)` to keep order deterministic. Always call `resetTestState()` at the top.
- See existing tests for patterns: state seeding, mock fetch, sync asserts, and `Promise.resolve().then(...)` chains for async settle.

---

## 8. Gotchas — read before editing firmware

Distilled from `docs/AUDIT.md` and current code:

- **Total Main is master-driven, real-time.** The master's heartbeat handler accumulates `sessionElapsed` per slave (already in `SlaveRecord` from v1.2.6). The `/api/slaves` response exposes two new fields: `sessionElapsed` (live, 2s cadence) and `sessionPackageTime` (full package set on `ADD_TIME`). The dashboard reads `sessionElapsed` on every 3 s `poll`, stores it in `timers[id].mfs`, and `updateStatEl` displays `Math.max(0, (base.totalDetik || 0) + mfs)`. **No `tfs` subtraction, no `tfsAlreadyInBase`, no `rc_base_at_session_start_<id>`, no `rc_tfs_<id>` for Total Main.** Single source of truth: the master. Refresh just waits for the next poll (≤3 s lag). Test 15-17 cover the live sync; tests 18-27 cover defensive cases (negative, undefined, master reboot, manual STOP, reset, offline, top-up, pause/resume).
- **Master reboot recovery.** When master's `sessionElapsed` drops >5 s below last known, the dashboard accepts the new value (reboot detected — accumulator reset). Within 5 s drift, the dashboard keeps the last value (handles slave NVS race). See test 20.
- **Manual ⏹ Reset Timer must NOT count** (laporan keuangan or stats). Invariant: only clean natural ENDED (slave timer hits 0) increments `totalDetik`/`totalSesi` and creates a transaksi entry. Two enforcement points:
  - **Frontend** `sendCmd` STOP path: skip `applyPackageToStats` + `recordTrx`, call `clearPending(id)` so the applySlaves LOCKED-with-pending branch doesn't re-record the next poll. Set `timers[id].mfs = 0` so display drops to base only.
  - **Firmware** (`wifi_master_esp32_WEBUI/main.cpp`): the `CMD_STOP` handler must NOT call `autoSaveStats(sid, packageTime)`. Master must still set `stoppedManually = true` so the heartbeat handler skips any late-arriving ENDED transition (race protection).
  - **Firmware** nonWEBUI achieves the same effect differently: line ~871 zeros `sessionElapsed` before broadcasting STOP, so when the heartbeat ENDED transition fires, `autoSaveStats(0)` is a no-op. The nonWEBUI `gotResponse` block sets `sessionPackageTime = 0` + `stoppedManually = true` and the heartbeat handler checks `!stoppedManually` before saving.
  - **Reset Total Main** (🗑 button) sets `timers[id].mfs = 0` so display = base (zeroed) + 0 = 0.
  - Test 3 asserts NO POST to `/api/transaksi/add`, statsCache unchanged, pending cleared, mfs=0. Test 14 asserts the LOCKED-after-STOP race doesn't re-record. Test 22 (🗑) and Test 23 (manual STOP) verify display drops to base.
- **Slave `lastMasterContactMs` guard is intentionally a no-op** (assigns `nowMs` then immediately compares). Documented in `docs/AUDIT.md`. Recovery is via ESP-NOW re-register on heartbeat.
- **Powerloss recovery auto-RESUMES** a paid rental (not PAUSED) — intentional design. 3 warning beeps give staff time to move hands away. Change at slave `main.cpp` if you want paused.
- **`cmdFromString` is case-sensitive in C** (`ADD_TIME` etc.) — Python mirror `cmd_from_string` is case-insensitive. Tests reflect Python semantics.
- **ESP8266 slave does NOT use ESP-NOW** — it polls Master over HTTP. Don't merge its protocol assumptions with the others.
- **`index_html.h` is auto-generated** — never edit by hand. `.h.bak` and `.cpp.bak` files are old, ignore. Legacy `script.py` files in master folders are superseded by `firmware/build_frontend.py`.
- **NVS MAC key is 12 chars** (no colons). Sits at the edge of NVS's 15-char limit. Test `test_mac_key_no_colons_means_passes_nvs_15_char_limit` documents this.
- **`openapi.yaml` is stale.** It documents 4 legacy endpoints. The full live surface is in `wifi_master_esp32_WEBUI/main.cpp` (handler definitions) and `wifi_master_esp32_nonWEBUI/main.cpp`. Use the C++ source as the source of truth.
- **`tests/test_master_persistence.py::apply_transaksi_add` and `apply_stats_add` mirror the master's older 3-day/500-record semantics.** Master is now 7 days / 300. Treat the test mirror as a regression guard, not a spec.
- **`ESP-NOW MAC matching** uses `rawMac[6]` (binary), not the colon string. Helpers `macToString`, `isValidMac`, `getMacKey` are duplicated in C++ and Python — keep them identical.

---

## 9. Documentation map

| Doc | Purpose |
|---|---|
| `README.md` | Marketing / architecture overview / hardware BOM |
| `docs/SYSTEM_MANUAL.md` | Full end-user system manual (architecture, wiring, troubleshooting) |
| `docs/WIRING_RC_EXCAVATOR.md` | RC toy wiring + Y-splitter power + XY-MOS fail-safe |
| `docs/MOSFET_3V7_WIRING_GUIDE.md` | 3.7 V Li-ion + JST modular build (production) |
| `docs/MANUAL_FLASHING_GUIDE.md` | Windows: CH340/CP210x drivers, manual BOOT-mode for C3 Super Mini |
| `docs/Panduan_Web_Flasher.md` | Indonesian: browser-based flashing via Web Serial |
| `docs/ANDROID_APP_GUIDE.md` | Master-as-bridge principle; raw API values; connection setup |
| `docs/AUDIT.md` | **Bug list from June 2026 — best gotcha summary before editing firmware** |
| `docs/openapi.yaml` | OpenAPI 3.0 — **stale**, only 4 legacy endpoints; live surface is in C++ source |
| `docs/Excavator_API_Postman_Collection.json` | Older Postman collection (same coverage as `openapi.yaml`) |

---

## 10. Quick style/conventions

- **No CLAUDE.md or AGENTS.md yet** — you are reading the first one. If you add conventions, append below.
- **C++** uses `snake_case` for vars/funcs, `UPPER_SNAKE_CASE` for constants, `PascalCase` for structs/enums (`SlaveRecord`, `RentalState`, `EspNowPacket`). Arduino framework.
- **Python** uses `snake_case`. Tests use pytest functions, no classes (despite `python_classes = Test*` in `pytest.ini`).
- **JS (frontend)** uses `camelCase`, `const`/`let` only (no `var`), single-file `index.html` with everything inline.
- **Kotlin (Android reference)** uses `ExcavatorWifiManager` as the wrapper class; OkHttp + 2 s timeouts.
- **Comments** in C++ are extensive and Indonesian-flavored English. Preserve them when refactoring — they explain *why* not just *what*.
- **Indentation:** 2 spaces across all files (C++, Python, JS, HTML, CSS). Tabs are not used.

---

## 11. When you make a release

1. Bump code, run `node scripts/test_dashboard_logic.js` and `pytest`.
2. Rebuild firmware with PlatformIO for each env.
3. Run `python build_release.py vX.Y.Z` → outputs to `release/vX.Y.Z/` and mirrors `release/latest/`.
4. Commit `release/vX.Y.Z/` artifacts (note: `.gitignore` whitelists `release/` and `release/*.bin`).
5. Update `docs/openapi.yaml` if you added endpoints (or note it's still stale).
6. Tag the commit `vX.Y.Z` if desired.

---

## 12. Pointers to deeper context

- Master state machine + auto-save logic: `firmware/wifi_master_esp32_WEBUI/main.cpp` (`autoSaveStats`, `saveElapsedOnly`, `handleAddStats`, `handleAddTransaksi`, `handleSyncTime`).
- Slave state machine + powerloss: `firmware/wifi_slave_esp32/main.cpp` (lines 579–606 for NVS restore).
- Dashboard entry points: `frontend/index.html` — search for `function applySlaves`, `function sendCmd`, `function applyPackageToStats`, `function loadStatsFromEsp32`.
- Recent commits worth reading: `git log --oneline -10` — the project has been iterating v1.2.x with dashboard E2E test additions.

---

## 13. Dev Tools Reference

Every tool a developer or tester might run, grouped by purpose. Each entry: **what it is / when to reach for it / how to invoke / what it returns**.

### 13.1 Test runners (no hardware needed)

| Tool | What it does | When | How |
|---|---|---|---|
| `pytest` | Runs the Python test suite. Mirrors the C++ ESP-NOW protocol (`protocol_lib.py`) and the master's heartbeat / demo-mode logic in Python. | Every commit. Required by the release flow (§11). | `pytest` (all) / `pytest tests/test_protocol.py -v` (one file) |
| `node scripts/test_dashboard_logic.js` | Extracts `<script>` from `frontend/index.html`, runs it in a `vm` sandbox with mocked `localStorage`/`fetch`/`document`/`Date`. 13 cases, 23 assertions covering session start / ENDED / STOP / closed-tab recovery / command failure / `applyPackageToStats` / multi-session / paket field / `tfsAlreadyInBase` cleanup / `sesElap` / SPIFFS retry / exact package time / multi-RC isolation. | Every dashboard change. Required by the release flow. | `node scripts/test_dashboard_logic.js` — exits 1 on any FAIL |
| `python scripts/test_esptool.py` | One-liner that imports `esptool` and runs `esptool --chip esp32 flash_id`. | Diagnose broken Python env / CH340 wiring / esptool install before doing a real flash. | `python scripts/test_esptool.py` |

### 13.2 Mock servers (no hardware needed)

| Tool | What it does | When | How |
|---|---|---|---|
| `scripts/mock_master_server.py` | Standalone Python HTTP server on `:8080` that **emulates the master**: `/` serves `frontend/index.html`; `/api/{auth,slaves,stats,transaksi,karyawan,command,sync-time,verify-sa}` all implemented in-process. A background thread ticks fake timers so RUNNING → ENDED transitions happen naturally. | Develop the dashboard UI against a desktop mock (no ESP32 needed). Auth: `admin/admin` (SA) or any karyawan listed in `karyawan = ['karyawan1', 'karyawan2']` with password `1234`. | `python scripts/mock_master_server.py` → open `http://localhost:8080` |
| `scripts/test_logic.py` | Runs a **mock slave** HTTP server on `:80` (so the master can poll it via `/api/state`/`/api/command`) AND drives the **real master** at `192.168.4.1`. Asserts the master observes RUNNING after ADD_TIME, then ENDED after the mock flips its state. | Smoke-test master↔slave plumbing when you have a real master but no real slaves. **Requires your PC to be on `ExcavatorMaster` AP and to occupy port 80 (run as admin or use `netsh http add iplisten`).** | `python scripts/test_logic.py` |
| `scripts/test_real_hardware.py` | Drives the **real master** at `192.168.4.1`. Sends `ADD_TIME(60)` then `STOP` to the first online slave and asserts `LOCKED`. | End-to-end hardware check that the master → ESP-NOW → slave → state-poll loop still works. | `python scripts/test_real_hardware.py` |
| `scripts/scratch.py` | **Load + resilience test.** Spawns a mock slave on `:80`, factory-resets the master, registers 9 slaves, then runs 4 parallel threads: dashboard polling (`/api/slaves`), admin history polling, an operator hammering ADD_TIME/PAUSE/RESUME/STOP at random IDs every 0.8 s, and an **attacker** sending broken JSON / 5 KB payloads / bogus endpoints every 5 s. Prints RPS / errors / slave-hits every 3 s. | Stress-test the master before a mall opens. Watch `Errors` rising = master is choking. | `python scripts/scratch.py --verbose --duration 60` |

### 13.3 Hardware diagnostics

| Tool | What it does | When | How |
|---|---|---|---|
| `scripts/monitor.py` | Spawns 3 daemon threads reading `COM6`, `COM26`, `COM27` at 115200 baud simultaneously; prefixes each line with its port. | Watch 1 master + 2 slaves side-by-side during a live session. **Edit `ports = [...]` to match your setup.** | `python scripts/monitor.py` |
| `scripts/com_test.py` | Same 3-port reader (COM6/COM26/COM27) **plus** a single `POST /api/command {id:2, cmd:ADD_TIME, val:60}` to the master after 2 s, so you can correlate the command dispatch against each slave's serial log. | One-shot: confirm a slave's ESP-NOW path end-to-end without touching the dashboard. | `python scripts/com_test.py` |

### 13.4 Dashboard's built-in Dev Debugger Panel

The dashboard itself has a hidden dev panel — no extra tool needed. Enable with **`?debug=1`** on the URL (e.g. `http://192.168.4.1/?debug=1`). A floating 🪲 button appears in the top-right; tapping it opens `dlgDebug` with:

- **Simulasi Jaringan** — `Simulate Offline` / `Simulate Online` buttons. Toggles the conn-pill without disconnecting; useful to test the retry / back-off UX without unplugging.
- **Simulasi Tanggal (YYYY-MM-DD)** — type a date and `Trigger Reset` to hit `POST /api/sync-time` with that date. If it differs from the last synced date, master zeros `totalSesi` for all RCs.
- **Simulasi Sesi Cepat (Harga Rp 99)** — pick an RC, then 5/10/15/20/25/30-second buttons. These call `setPending(rcId, 'Test Ns', N/60, 99, 'Nd')` then `sendCmd(rcId, 'ADD_TIME', N)`. `paket: 'Nd'` is what triggers the `applyPackageToStats` exact-package-time path. Use to validate ENDED → transaksi + stats → tfs-cleanup flow without waiting minutes for a real 5-minute package.
- **Inspeksi State LocalStorage** — live JSON dump of `rc_pending` + every `rc_tfs_*` snapshot.
- **Inspeksi State Timers** — live JSON dump of `timers[id].{sisa,running,tfs,sessionDone}` per RC.

The panel is gated by `isDev` (`location.hostname === 'localhost' || '127.0.0.1' || new URLSearchParams(location.search).has('debug')`). In production it won't render even if you ship the URL parameter.

### 13.5 Build & release

| Tool | What it does | When | How |
|---|---|---|---|
| `firmware/build_frontend.py` | Reads `frontend/index.html`, `gzip.compress`es it, writes `wifi_master_esp32_WEBUI/index_html.h` as a `uint8_t[] PROGMEM`. | Any time `frontend/index.html` changes. Required before `pio run -e master_html`. | `python build_frontend.py` (run from `firmware/`) — or pass a custom HTML path as arg 1. |
| `build_release.py` | The full release pipeline: builds 4 PlatformIO envs → `esptool merge_bin` for each (offsets per chip) → PyInstaller bundles `release/flash.py` → `flash.exe` → copies README → zips into `Excavator_Firmware_Flasher.zip` → mirrors to `release/latest/`. | Cutting a release tag. | `python build_release.py v1.2.5` (output → `release/v1.2.5/`); with no arg → `release/latest/` only. |
| `release/flash.py` (source) / `release/flash.exe` (built) | **End-user flasher.** Interactive menu of 4 firmware options (Master WEBUI / Master non-WEBUI / Slave C3 / Slave ESP32). Auto-detects ESP ports by USB descriptor (CH340 / CH341 / CP210 / FTDI / JTAG / UART), prompts for full-erase (y/N), then runs `esptool write_flash`. Catches `SystemExit` so a user can flash multiple boards in one session. Calls `esptool.main()` directly (PyInstaller can't `subprocess -m esptool`). | Distribute to end users. Inside `release/vX.Y.Z/`, alongside the 4 `.bin` files. | Double-click `flash.exe`, choose option, choose port, answer erase prompt. |

### 13.6 App provisioning

| Tool | What it does | When | How |
|---|---|---|---|
| `scripts/keygen.py` | Generates HMAC-SHA256 activation codes for the Android app's licensing scheme. Two tiers: **`PEMULA`** (16 chars) and **`CUAN`** (20 chars). Secret is hardcoded (`RC7T!m3r@K3y#2024$S3cr3t&S4f3Key`); input is `<device_id>\|<package_type>`. | When a customer pays and needs an unlock code. The device_id is shown in the Android app. | `python keygen.py <device_id>` — prints both `PEMULA` and `CUAN` codes. |

---

## 14. Tool selection cheat sheet

| I want to … | Reach for |
|---|---|
| Validate a protocol/wire-format change without flashing | `pytest tests/test_protocol.py` |
| Validate a dashboard UI change without flashing | `node scripts/test_dashboard_logic.js` (logic) + `python scripts/mock_master_server.py` (visual) |
| Confirm the master can talk to slaves (any slaves, real or mock) | `python scripts/test_logic.py` |
| Confirm the master can talk to **real** slaves | `python scripts/test_real_hardware.py` |
| Watch 3 boards' serial output side-by-side | `python scripts/monitor.py` |
| One-shot: dispatch one command and read serial | `python scripts/com_test.py` |
| Stress-test the master (9 slaves + 3 clients + attacker) | `python scripts/scratch.py --verbose --duration 120` |
| Sanity-check esptool install + CH340 driver | `python scripts/test_esptool.py` |
| Inspect dashboard in-memory state from a real device | open `http://192.168.4.1/?debug=1` → tap 🪲 |
| Generate an Android app activation code | `python scripts/keygen.py <device_id>` |
| Rebuild the embedded dashboard header | `python firmware/build_frontend.py` (from `firmware/`) |
| Cut a new release tag | `python build_release.py vX.Y.Z` |
| Flash one board to one device (end-user flow) | `release/flash.exe` (built) or `python release/flash.py` (from source) |