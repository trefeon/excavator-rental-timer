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
- SPIFFS files: `/auth.json`, `/stats.json`, `/transaksi.json`.
- Heap guard on transaksi: if free heap `< 80000` → 503 `LOW_HEAP`.

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

- **Stats double-count is the #1 historical bug.** The fix: master WEBUI never increments `totalSesi` on its own — slave's heartbeat transition to ENDED enqueues a `StatsJob`, drained in `loop()` while holding `statsMutex`. Frontend never writes elapsed. Only `applyPackageToStats` (immediate, optimistic) for instant UI; followed by `loadStatsFromEsp32` (authoritative, from SPIFFS).
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