# Excavator Rental Timer - System Manual

Last Updated: June 4, 2026

Wi-Fi based RC excavator rental timer system with Master-Slave architecture.

---

## 1. System Architecture

```text
┌─────────────────┐                    ┌─────────────────┐
│  Android App    │    Wi-Fi AP        │  Slave ESP32    │
│  (Dashboard +   │◄──────────────────►│  (192.168.4.x)  │
│   Business Logic)│ (ExcavatorMaster)  │                 │
│                 │                    │  - Timer        │
│  - User Auth    │   ┌────────────┐   │  - Relay        │
│  - Pricing      │◄─►│Master ESP32│◄─►│  - TM1637       │
│  - History      │   │(Bridge API)│   │  - Buzzer       │
│  - Revenue      │   │192.168.4.1 │   │  - Button       │
└─────────────────┘   └────────────┘   └─────────────────┘
```

> **IMPORTANT:** Master ESP32 acts ONLY as a **bridge** between the Android App and the Slaves. The Master DOES NOT store pricing, history, revenue, or user data. All business logic is handled by the Android application.

### Components

| Component            | Function                                                 |
| ------------------- | -------------------------------------------------------- |
| Master ESP32 (V1)   | Access Point, Bridge API (proxies commands to slaves)    |
| Slave ESP32 (V1/C3) | Controls Timer, Relay, Display, Buzzer                   |
| Slave ESP8266       | Alternative timer module, Relay, Display, Buzzer         |
| TM1637              | 4-digit MM:SS Display                                    |
| Relay               | Power ON/OFF for the excavator                           |
| Buzzer              | Sound notifications (e.g. time transfer or time up)      |

---

## 2. Wiring

### Master

Just plug a 5V USB cable into the ESP32 DOIT DevKit V1. No external wiring needed.
The indicator LED uses GPIO 2.

### Slave (ESP32 DevKit V1)

| Pin     | Function   | Connects To               |
| ------- | ---------- | ------------------------- |
| GPIO 22 | TM1637 CLK | Display CLK               |
| GPIO 23 | TM1637 DIO | Display DIO               |
| GPIO 26 | Relay IN   | Relay Module              |
| GPIO 27 | Buzzer +   | Active Buzzer             |
| GPIO 32 | Button     | Push button (pull-up)     |
| GPIO 2  | LED        | Built-in LED (active LOW) |

### Slave (ESP32-C3 Super Mini)

| Pin    | Function   | Connects To               |
| ------ | ---------- | ------------------------- |
| GPIO 4 | Relay IN   | Relay Module              |
| GPIO 5 | Buzzer +   | Active Buzzer             |
| GPIO 6 | TM1637 CLK | Display CLK               |
| GPIO 7 | TM1637 DIO | Display DIO               |
| GPIO 9 | Button     | Push button (pull-up)     |
| GPIO 8 | LED        | Built-in LED (active LOW) |

### Slave (ESP8266/NodeMCU)

| Pin         | Function   | Connects To     |
| ----------- | ---------- | --------------- |
| D1 (GPIO5)  | Relay      | Relay Module    |
| D2 (GPIO4)  | Buzzer     | Active Buzzer   |
| D5 (GPIO14) | Button     | Push button     |
| D6 (GPIO12) | TM1637 CLK | Display CLK     |
| D7 (GPIO13) | TM1637 DIO | Display DIO     |
| D4 (GPIO2)  | LED1       | Relay state LED |
| D0 (GPIO16) | LED2       | Activity LED    |

---

## 3. Wi-Fi Configuration

| Parameter | Value                                     |
| --------- | ----------------------------------------- |
| SSID      | `ExcavatorMaster`                         |
| Password  | `12345678`                                |
| Master IP | `192.168.4.1`                             |
| Slave IP  | DHCP (automatic, e.g. `192.168.4.2`, etc.)|

---

## 4. REST API (Master Bridge Endpoints)

**Important Note:** Master only forwards (proxies) commands to the Slave. Auth is handled entirely by the Android application. All endpoints are open access.

### Endpoints

| Method | Endpoint            | Description                                     |
| ------ | ------------------- | ----------------------------------------------- |
| GET    | `/api/slaves`       | Status of all slaves (IP, state, time_left, etc.)|
| POST   | `/api/command`      | Send command to slave (proxied)                 |
| POST   | `/api/edit_slave`   | Change slave ID                                 |
| POST   | `/api/delete_slave` | Remove slave registration                       |
| GET    | `/api/register`     | Internal slave registration (called by slave)   |

### Commands (POST `/api/command`)

JSON Body: `{"id": 1, "cmd": "ADD_TIME", "time": 300}`

| cmd        | time     | Description                                   |
| ---------- | -------- | --------------------------------------------- |
| `ADD_TIME` | 1-28800  | Add time (in **seconds**)                     |
| `PAUSE`    | 0        | Pause running time                            |
| `RESUME`   | 0        | Resume paused time                            |
| `STOP`     | 0        | Reset time to 0 & lock relay                  |
| `IDENTIFY` | 0        | Beep slave buzzer 3x to locate its physical position |
| `REBOOT`   | 0        | Software restart the ESP slave                |

> **All time values are in SECONDS.** Conversion from minutes/hours to seconds is done by the Android app before sending to the API.

---

## 5. Important Features

1. **Powerloss Recovery**: The remaining time on the slave is saved periodically to the EEPROM (ESP8266) or NVS (ESP32) every 30 seconds. If power fails or the battery is unplugged, the slave will resume its remaining time when turned back on.
2. **Timer Accuracy**: Non-blocking logic loop (ESP32) prevents timer drift compared to relying on `delay()`.
3. **Max Time Limit**: Slave rejects ADD_TIME exceeding 480 minutes (28800 seconds / 8 hours) per single command.
4. **Battery Field**: Slave sends `battery` field with `"OK"` value (hardcoded). There is no physical battery sensor — this field is prepared for future development.

---

## 6. Build & Test (PlatformIO)

**Build File:** `platformio.ini` has the following available environments: `master`, `slave`, `slave_c3`, and `slave8266`.

### Build & Upload

```bash
# Compile all environments
pio run

# Compile & upload individually
pio run -e master -t upload --upload-port COM_MASTER
pio run -e slave -t upload --upload-port COM_SLAVE
pio run -e slave_c3 -t upload --upload-port COM_SLAVE_C3
pio run -e slave8266 -t upload --upload-port COM_SLAVE_8266
```

### Stress Testing

Automated testing uses `python scratch.py` (supports `--stress` option for concurrent connection flooding, rogue testing, payload fuzzing, and poll checks).
