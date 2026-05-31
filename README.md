# Excavator Timer Rental — ESP32 Firmware

> Sistem timer sewa excavator RC di mall. ESP32 + TM1637 4-digit display + BLE.  
> Legacy Wokwi sketch. MVP terbaru memakai ESP32 sebagai source of truth timer.

# Current MVP - BLE Direct + Relay Murah + TM1637

Dokumen MVP terbaru:

- [MVP_SPEC.md](MVP_SPEC.md)
- [Client proposal](PROPOSAL.md)
- [Mermaid diagrams](docs/MERMAID_DIAGRAMS.md)
- [Relay wiring](docs/RELAY_WIRING.md)
- [BLE protocol](docs/BLE_PROTOCOL_SPEC.md)
- [ESP32 firmware skeleton](firmware/esp32_ble_direct_mvp/esp32_ble_direct_mvp.ino)
- [Android app flow](docs/ANDROID_APP_FLOW.md)
- [Android dashboard mockup](docs/android-dashboard-mockup.html)

Catatan: repo lama memakai TM1637 4-digit display. MVP baru tetap memakai display itu, ditambah relay murah untuk memutus power mainan.

---

## Overview

Produk timer rental untuk mainan excavator RC dewasa di mall. Setiap unit excavator dipasang modul ESP32 dengan:

- **TM1637 4-digit 7-segment display** — countdown MM:SS
- **Buzzer piezo** — bunyi saat timer habis
- **2 tombol fisik** — reset + test buzzer
- **BLE GATT server** — kontrol dari Android app
- **NVS storage** — buffer state sementara
- **Battery monitoring** — ADC baca tegangan LiPo via voltage divider

## Arsitektur

```
┌─────────────┐     BLE      ┌─────────────────┐    WiFi     ┌──────────┐
│  ESP32 ×10  │◄────────────►│  Android App     │───────────►│  Cloud   │
│  (EX-01..10)│              │  (Source of Truth)│            │ (Optional)│
└─────────────┘              └─────────────────┘             └──────────┘
```

**Android App = Source of Truth.** ESP32 NVS hanya buffer sementara.

## Workspace Structure

```
excavator-timer/
├── wokwi/                 ← Wokwi simulation
│   ├── sketch.ino         ← ESP32 firmware (v2.1)
│   ├── diagram.json       ← Wokwi circuit diagram
│   ├── libraries.txt      ← Arduino library deps
│   └── wokwi.toml         ← VS Code Wokwi extension config
├── docs/                  ← Documentation (HTML diagrams)
│   ├── business-flow.html
│   └── wiring-diagram.html
├── .gitignore
└── README.md
```

## Quick Start — Wokwi Simulation

1. Install [Wokwi for VS Code](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode)
2. Open `wokwi/sketch.ino` in VS Code
3. Press `F1` → `Wokwi: Start Simulator`
4. Use Serial Monitor to control timer:

```
SET 300      → set timer 5 menit (300 detik)
START        → mulai countdown
PAUSE        → pause timer
STOP         → stop (= pause)
RESET        → reset ke IDLE
CLEAR        → clear NVS + reset
STATUS       → print JSON state
```

> **Note:** BLE disabled in Wokwi simulation (`#define WOKWI_SIMULATION`).  
> Comment out line tersebut untuk build production ke ESP32 asli.

## Wiring (Real Hardware)

| Komponen | Pin | ESP32 |
|----------|-----|-------|
| TM1637 CLK | CLK | GPIO 18 |
| TM1637 DIO | DIO | GPIO 19 |
| TM1637 VCC | VCC | 3.3V |
| TM1637 GND | GND | GND |
| Buzzer (+) | VCC | GPIO 13 |
| Buzzer (−) | GND | GND |
| Button Reset | SIG | GPIO 14 (pullup) |
| Button Test | SIG | GPIO 27 (pullup) |
| Battery ADC | SIG | GPIO 34 (via divider R1=100k, R2=33k) |

## BLE Protocol

| Characteristic | UUID | Type | Description |
|---------------|------|------|-------------|
| TimerValue | `beb5483e-...-26a8` | Write | Set duration (seconds) |
| TimerStatus | `beb5483e-...-26a9` | Read/Notify | `{"s":remaining,"t":"STATE"}` |
| Command | `beb5483e-...-26aa` | Write | START/STOP/RESET/PAUSE/CLEAR |
| DeviceName | `beb5483e-...-26ab` | Read | "EX-01" |
| DeviceInfo | `beb5483e-...-26ac` | Read | Full JSON state dump |

Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

## States

| State | Display | Buzzer | Description |
|-------|---------|--------|-------------|
| IDLE | `----` | OFF | Menunggu perintah |
| RUNNING | `MM:SS` (colon on) | OFF | Countdown aktif |
| PAUSED | `MM:SS` (colon off) | OFF | Timer di-pause |
| FINISHED | `0000` kedip | ON (30s) | Timer habis |
| TIMEOUT | `----` | OFF | Power loss recovery, tunggu app |

## Paket Harga (Mall)

| Durasi | Harga |
|--------|-------|
| 5 menit | Rp 25.000 |
| 10 menit | Rp 40.000 |
| 15 menit | Rp 55.000 |

## BOM (Bill of Materials) — Per Unit

| Komponen | Harga Est. |
|----------|-----------|
| ESP32 DevKit V1 | Rp 35-60rb |
| TM1637 4-Digit 7-Segment | Rp 8-15rb |
| Buzzer Piezo Active | Rp 3-5rb |
| Push Button 6×6mm × 2 | Rp 2rb |
| Kabel Jumper × 8 | Rp 4rb |
| Buck Converter (LM2596) | Rp 8-15rb |
| Kapasitor 1000µF | Rp 1rb |
| Resistor 100k + 33k (divider) | Rp 1rb |
| Box/Enclosure | Rp 20-40rb |
| **Total per unit** | **Rp 80-145rb** |
| **Total 10 unit** | **Rp 800rb - 1.45jt** |

## Production Build

Untuk flash ke ESP32 asli:

1. Comment out `#define WOKWI_SIMULATION` di sketch.ino
2. Ganti `DEVICE_NAME` per unit: `EX-01`, `EX-02`, dst.
3. Upload via Arduino IDE atau PlatformIO
4. Board: ESP32 Dev Module, Upload Speed: 921600

## License

Proprietary — Excavator Timer Rental
