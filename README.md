# Excavator Rental Timer

## Architecture: Wi-Fi Master-Slave (Centralized API) Dashboard + Android Proxy API

Rental timer system for RC excavator toys in malls. Uses a **Wi-Fi Master-Slave ESP32** architecture without internet, without cloud.

---

## Summary

- **1 Master (ESP32):** Wi-Fi Access Point (DHCP), Dashboard Web Server, Central Registry, API Proxy Gateway for Android.
- **Multiple Slaves (ESP32 per toy):** Connected to Master Wi-Fi. Controls relay (power gate), displays remaining time on TM1637, saves session to NVS (powerloss recovery).

## Architecture

```
┌──────────────────┐    Wi-Fi (API)   ┌──────────────────┐    Wi-Fi    ┌───────────────┐
│ Android App /    │◄────────────────►│ ESP32 Master     │◄───────────►│ ESP32 Slave 1 │
│ Web Browser      │   (Single IP)    │ (Access Point +  │             └───────────────┘
│ (Dashboard)      │                  │  API Gateway)    │◄───────────►│ ESP32 Slave 2 │
└──────────────────┘                  └──────────────────┘             └───────────────┘
                                                                       ... (up to 9 Slaves + 1 operator device)
```

**Key Features:**
1. **Centralized API:** Android/browser only needs 1 Master IP (`192.168.4.1`).
2. **Zero-Touch Provisioning:** New slave automatically registers to Master, gets an ID (EXC-01, EXC-02).
3. **Powerloss Recovery:** Slave saves remaining time to NVS every 30 seconds and on every state change.
4. **Thread-Safe Registry:** Mutex protects slave data access between Core 0 (polling) and Core 1 (web server).
5. **WiFi Auto-Reconnect:** Slave uses `WiFi.onEvent()` + `WiFi.setAutoReconnect(true)`, server remains responsive when WiFi drops.
6. **Hardware Watchdog:** 10-second watchdog on both devices, auto-reboot if frozen.
7. **HTTP Timeout:** All HTTPClient calls are limited to a 2-second timeout.

---

## Project Structure

```
excavator-rental-timer/
├── docs/                  ← System documentation, architecture, API, wiring.
├── firmware/              
│   ├── wifi_master/       ← ESP32 Master Firmware (Access Point, Dashboard WebUI, API Gateway)
│   └── wifi_slave/        ← ESP32 Slave Firmware (WiFi Client, Relay, TM1637, NVS)
├── frontend/              
│   └── android_reference/ ← Android App reference code / SDK (Kotlin)
├── .gitignore
└── README.md
```

---

## Hardware

### Master Unit
1 unit ESP32 DevKit V1 (DOIT), powered by 5V USB, placed at the cashier's desk.

### Slave Unit (per toy)

| Component | Est. Price |
|-----------|------------|
| ESP32 DevKit V1 | Rp 35,000-60,000 |
| TM1637 4-Digit 7-Segment | Rp 8,000-15,000 |
| Relay Module 1-Channel 3.3V | Rp 10,000-15,000 |
| Jumper Wires | Rp 4,000 |
| Buck Converter (LM2596) | Rp 8,000-15,000 |
| Box/Enclosure | Rp 20,000-40,000 |
| **Total per unit** | **Rp 80,000-145,000** |

---

## Slave Wiring (per toy)

| Component | ESP32 Slave Pin |
|-----------|-----------------|
| TM1637 CLK | GPIO 22 |
| TM1637 DIO | GPIO 23 |
| TM1637 VCC | 3.3V / 5V |
| TM1637 GND | GND |
| Relay IN | GPIO 26 |
| Buzzer (+) | GPIO 27 |
| Button (Resume) | GPIO 32 (INPUT_PULLUP) |
| Status LED | GPIO 2 (built-in) |

See detailed wiring in [docs/RELAY_WIRING.md](docs/RELAY_WIRING.md).

---

## Toy State

| State | Display | Relay | Description |
|-------|---------|-------|-------------|
| LOCKED | `----` | OFF | Standby, no session |
| RUNNING | `MM:SS` (blink) | ON | Countdown active |
| PAUSED | `MM:SS` (blink) | OFF | Timer paused |
| ENDED | `----` | OFF | Time is up |
| FAULT | `----` | OFF | Error (needs restart) |

**Powerloss Recovery Behavior:**
- When battery is disconnected & reconnected, the slave will **sound 3 warning beeps** then **auto-resume** to RUNNING state (not PAUSED).
- Safety: there is a 3-second delay with warning beeps before auto-resume, giving staff time to move their hands away.
- If you want to change to PAUSED mode (no auto-resume), change `state = STATE_RUNNING` to `state = STATE_PAUSED` in the Powerloss Recovery section of `wifi_slave.ino`.

---

## Master API Endpoints (192.168.4.1)

| Method | Endpoint | Function |
|--------|----------|----------|
| GET | `/` | Dashboard WebUI |
| GET | `/api/slaves` | Fleet status (JSON array) |
| POST | `/api/command` | Proxy command to slave `{id, cmd, val}` |
| POST | `/api/transfer_time` | Transfer remaining time between slaves `{from_id, to_id}` |
| POST | `/api/edit_slave` | Change slave ID `{mac, id}` |
| POST | `/api/delete_slave` | Delete slave from registry `{mac}` |
| GET | `/api/register?mac=...` | Slave registration / heartbeat |

### Command List

| Command | Val | Description |
|---------|-----|-------------|
| ADD_TIME | seconds | Add time (300 = 5 minutes) |
| PAUSE | 0 | Pause timer |
| RESUME | 0 | Resume timer |
| STOP | 0 | Lock / reset timer to 0 |
| IDENTIFY | 0 | Buzzer 3x + screen blink (find toy) |
| REBOOT | 0 | Restart ESP32 slave |

See full specifications in [docs/WIFI_API_SPEC.md](docs/WIFI_API_SPEC.md) and [docs/openapi.yaml](docs/openapi.yaml).

---

## Pricing Packages (Mall)

| Duration | Price |
|----------|-------|
| 5 minutes | Rp 25,000 |
| 10 minutes | Rp 40,000 |
| 15 minutes | Rp 55,000 |

---

## Production Build

### Prerequisites
- **Board:** ESP32 (not Arduino Uno). Use ESP32 DevKit V1 / DOIT.
- **Arduino IDE:** Install ESP32 board via Boards Manager (URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`)
- **Required Libraries:**
  - `TM1637Display` (by Avishay Orpaz) — via Library Manager
  - `WiFi`, `WebServer`, `HTTPClient`, `Preferences` — built into ESP32 core

### Upload Master
1. Open `firmware/wifi_master/wifi_master.ino` in Arduino IDE.
2. Select board: `ESP32 Dev Module` or `DOIT ESP32 DEVKIT V1`.
3. Upload to the ESP32 that will act as the central router/server.
4. Open Serial Monitor (115200 baud) to view logs.
5. Dashboard is available at `http://192.168.4.1`.

### Upload Slave
1. Open `firmware/wifi_slave/wifi_slave.ino` in Arduino IDE.
2. Adjust WiFi SSID/password in the code if Master was changed.
3. Upload to the ESP32 on each excavator.
4. Slave will automatically register to Master (Zero-Touch Provisioning).

---

## Troubleshooting

### Slave doesn't appear in Dashboard
1. Ensure Master is turned on and broadcasting `ExcavatorMaster` Wi-Fi.
2. Check slave Serial Monitor (115200 baud) — look at `[WIFI]` and `[API]` logs.
3. Ensure slave is within Master's Wi-Fi range.
4. If MAC is registered but ID is wrong, use the **Edit** feature in the Manage Slaves modal.

### Timer doesn't run after battery replacement
- Slave will auto-resume after 3 seconds of warning beeps.
- If it remains LOCKED, check NVS — remaining time might have reached 0 before battery disconnection.

### Master temporarily down
- Slaves continue to run local timers even if master/AP goes down.
- When Wi-Fi returns, slaves will re-register and master will read latest state from `/api/state`.
- Master dashboard will mark slave offline after 30 seconds without a heartbeat.

### Master frozen / unresponsive
- Hardware watchdog will auto-reboot Master after 10 seconds of freeze.
- Check Serial Monitor for `[PROXY]` or `[REGISTRY]` errors.

### Compilation failed
- Ensure ESP32 core version 3.x is installed.
- TM1637Display library must be installed via Library Manager.
- If `esp_task_wdt_init` error occurs, ensure code uses config struct (already fixed for core 3.3.0+).

---

## Related Documents

| Document | Content |
|----------|---------|
| [MVP_SPEC.md](MVP_SPEC.md) | Complete MVP specifications |
| [PRD.md](PRD.md) | Product Requirement Document |
| [docs/WIFI_API_SPEC.md](docs/WIFI_API_SPEC.md) | API specification for Android |
| [docs/openapi.yaml](docs/openapi.yaml) | OpenAPI 3.0 spec (Swagger) |
| [docs/ANDROID_APP_FLOW.md](docs/ANDROID_APP_FLOW.md) | Android app flow |
| [docs/RELAY_WIRING.md](docs/RELAY_WIRING.md) | Relay wiring & pinout |
| [docs/MERMAID_DIAGRAMS.md](docs/MERMAID_DIAGRAMS.md) | Architecture & flow diagrams |
| [docs/excavator-rental-module-design.md](docs/excavator-rental-module-design.md) | Module concept design |

---

## Security

- System is **not connected to the internet** — only local Wi-Fi LAN from Master AP.
- Slaves connect via Wi-Fi with password.
- MAC address is used as unique hardware identity.
- No data encryption (MVP) — network is isolated from the internet.

## License

Proprietary — Excavator Timer Rental
