# Excavator Rental Timer — Audit & Test Suite

## Bugs found during recheck (June 2026)

Comparing `RCDashboard.html` (new live dashboard) against
`firmware/wifi_master_esp32_WEBUI/main.cpp`, `firmware/wifi_master_esp32_nonWEBUI/main.cpp`,
`firmware/wifi_slave_c3/main.cpp`, and `esp_now_protocol.h`.

The two master firmwares (`WEBUI` and `nonWEBUI`) are functionally identical
except for `DEMO_MODE` flag and the root handler. Both share the same bugs.

### 🐛 Critical — fixed

1. **Slave master-timeout guard can never fire** (`wifi_slave_c3/main.cpp:519-521`)
   The same `millis()` was assigned to `lastMasterContactMs` and then compared
   to itself, so the elapsed time was always 0 and the slave would never
   notice a dead master. **Fix:** snapshot once and use the snapshot for both
   sides of the comparison.

2. **ADD_TIME with `value == 0` is silently treated as success**
   (`wifi_slave_c3/main.cpp:380-389`). The slave falls into the `else if
   (pkt.value > 0)` branch when value is 0, never sets `cmdOk`, and the
   fallback at line 443 reports `RESP_BAD_STATE` — but only because of the
   guard. The branch ordering makes the intent invisible. **Fix:** add an
   explicit `else if (pkt.value == 0)` branch so the code reads "we only
   accept positive values, and any non-positive is BAD_STATE" without relying
   on the catch-all below.

3. **Stale ESP-NOW response looks like success** (`*master*/main.cpp` ~L873)
   `if (gotResponse && cmdResponsePkt.senderId == (uint8_t)targetId)` only
   handled the matching case. A response from a different slave with the
   right `ok` byte would still be treated as "slave offline/timeout" — fine —
   but the cast on `respCode` was implicit. **Fix:** explicit
   `(RespCode)cmdResponsePkt.respCode` cast and a dedicated branch for the
   "stale response" case so the dashboard can show a more specific error.

4. **`/api/transaksi/add` accepts unbounded fields** (`*master*/main.cpp`
   `handleAddTransaksi`). A misbehaving (or malicious) client could post
   `menit: 99999999` or `pelanggan: "<long string>"` and corrupt the SPIFFS
   JSON. **Fix:** validate `rcId 1..50`, `menit 1..480`, `harga 0..10M`,
   truncate `rcNama` and `pelanggan` to 64 chars.

### 🟡 Minor — not fixed (known / low risk)

- `handleAddKaryawan` allows duplicate `kr` users if they share the SA
  username (the `auth["sa"]["user"] == u` check) — the inner loop is
  redundant. Cosmetic.
- `handleResetStats` does not check `authCheck()` results, just sends
  response — actually fine, it does call it.
- Master loop's `goto doneHb` is the only `goto` in the codebase. It's
  correct but a refactor into a helper function would be cleaner.
- `getMacKey` returns 12-char NVS keys, safe for NVS (15-char limit) but
  no defensive prefix (e.g. `m_`) for namespace hygiene.
- `handleEditSlave` allows the SA to "steal" another slave's ID — the
  `if (owner != "" && owner != mac)` check only blocks explicit
  self-conflict. Intentional or bug? Unclear.

### ✅ What's correct

- ESP-NOW protocol: 24-byte packed struct, channel 1, broadcast
  FF:..:FF — matches across all three firmwares.
- Auth: SPIFFS-mounted `/auth.json`, X-Admin-User/X-Admin-Pass headers,
  SA + karyawan split.
- Stats auto-save: triggered on `RUNNING→ENDED` transition via heartbeat,
  plus DEMO_MODE simulated countdown in master loop. Both covered by tests.
- Powerloss recovery: NVS save every 10s (C3) / 30s (ESP32 slave), 3-beep
  warning, auto-resume RUNNING. Out of scope for unit tests.
- Heartbeat auto-register on unknown MAC: works after master reboot as
  long as Preferences (NVS) wasn't wiped.

## Unit tests

`pytest` suite, 94 tests, runs in ~0.2s without any hardware.

```
pytest tests/ -q
```

Three test files:

- `tests/protocol_lib.py` — Python mirror of `esp_now_protocol.h`
  (struct layout, enums, MAC helpers, slave state machine).
- `tests/test_protocol.py` — wire-format round-trip, enum mapping,
  state-machine transitions, master response contract, mall pricing
  sanity, 3-day transaksi cutoff.
- `tests/test_master_logic.py` — heartbeat → auto-save, transition
  detection, DEMO_MODE countdown, drift protection.
- `tests/test_master_persistence.py` — transaksi append/cutoff/cap,
  stats add/reset, edit-slave ID-owner check, registry reset.

## Bug-fix commit summary

```
firmware/wifi_master_esp32_WEBUI/main.cpp
firmware/wifi_master_esp32_nonWEBUI/main.cpp
  - handleCommandProxy: explicit RespCode cast + stale-response branch
  - handleAddTransaksi: validate rcId/menit/harga, truncate strings

firmware/wifi_slave_c3/main.cpp
  - networkTask: snapshot millis() for master-timeout guard
  - PKT_COMMAND ADD_TIME: explicit zero-value branch
```
