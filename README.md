# Excavator Rental Timer

## Arsitektur: Wi-Fi Master-Slave (Centralized API) Dashboard + Android Proxy API

Sistem timer rental untuk mainan excavator RC di mall. Menggunakan arsitektur **Master-Slave berbasis Wi-Fi ESP32** tanpa internet, tanpa cloud.

---

## Ringkasan

- **1 Master (ESP32):** Access Point Wi-Fi (DHCP), Web Server Dashboard, Registry Pusat, API Proxy Gateway untuk Android.
- **Banyak Slave (ESP32 per mainan):** Terhubung ke Wi-Fi Master. Kontrol relay (power gate), tampilkan sisa waktu di TM1637, simpan sesi ke NVS (powerloss recovery).

## Arsitektur

```
┌──────────────────┐    Wi-Fi (API)   ┌──────────────────┐    Wi-Fi    ┌───────────────┐
│ Android App /    │◄────────────────►│ ESP32 Master     │◄───────────►│ ESP32 Slave 1 │
│ Web Browser      │   (Single IP)    │ (Access Point +  │             └───────────────┘
│ (Dashboard)      │                  │  API Gateway)    │◄───────────►│ ESP32 Slave 2 │
└──────────────────┘                  └──────────────────┘             └───────────────┘
                                                                       ... (hingga 9 Slave + 1 perangkat operator)
```

**Fitur Utama:**
1. **Centralized API:** Android/browser hanya perlu 1 IP Master (`192.168.4.1`).
2. **Zero-Touch Provisioning:** Slave baru otomatis daftar ke Master, dapat ID (EXC-01, EXC-02).
3. **Powerloss Recovery:** Slave simpan sisa waktu ke NVS tiap 30 detik dan setiap perubahan state.
4. **Thread-Safe Registry:** Mutex melindungi akses data slave antara Core 0 (polling) dan Core 1 (web server).
5. **WiFi Auto-Reconnect:** Slave pakai `WiFi.onEvent()` + `WiFi.setAutoReconnect(true)`, server tetap responsif saat WiFi putus.
6. **Hardware Watchdog:** 10 detik watchdog di kedua perangkat, auto-reboot jika hang.
7. **HTTP Timeout:** Semua panggilan HTTPClient dibatasi 2 detik timeout.

---

## Struktur Proyek

```
excavator-rental-timer/
├── docs/                  ← Dokumentasi sistem, arsitektur, API, wiring.
├── firmware/              
│   ├── wifi_master/       ← Firmware ESP32 Master (Access Point, Dashboard WebUI, API Gateway)
│   └── wifi_slave/        ← Firmware ESP32 Slave (WiFi Client, Relay, TM1637, NVS)
├── frontend/              
│   └── android_reference/ ← SDK / kode referensi aplikasi Android (Kotlin)
├── .gitignore
└── README.md
```

---

## Hardware

### Master Unit
1 buah ESP32 DevKit V1 (DOIT), ditenagai USB 5V, disimpan di meja kasir.

### Slave Unit (per mainan)

| Komponen | Harga Est. |
|----------|-----------|
| ESP32 DevKit V1 | Rp 35.000-60.000 |
| TM1637 4-Digit 7-Segment | Rp 8.000-15.000 |
| Module Relay 1-Channel 3.3V | Rp 10.000-15.000 |
| Kabel Jumper | Rp 4.000 |
| Buck Converter (LM2596) | Rp 8.000-15.000 |
| Box/Enclosure | Rp 20.000-40.000 |
| **Total per unit** | **Rp 80.000-145.000** |

---

## Wiring Slave (per mainan)

| Komponen | Pin ESP32 Slave |
|----------|----------------|
| TM1637 CLK | GPIO 22 |
| TM1637 DIO | GPIO 23 |
| TM1637 VCC | 3.3V / 5V |
| TM1637 GND | GND |
| Relay IN | GPIO 26 |
| Buzzer (+) | GPIO 27 |
| Button (Resume) | GPIO 32 (INPUT_PULLUP) |
| LED Status | GPIO 2 (built-in) |

Lihat detail wiring di [docs/RELAY_WIRING.md](docs/RELAY_WIRING.md).

---

## State Mainan

| State | Display | Relay | Deskripsi |
|-------|---------|-------|-----------|
| LOCKED | `----` | OFF | Standby, tidak ada sesi |
| RUNNING | `MM:SS` (kedip) | ON | Countdown aktif |
| PAUSED | `MM:SS` (kedip) | OFF | Timer dijeda |
| ENDED | `----` | OFF | Waktu habis |
| FAULT | `----` | OFF | Error (butuh restart) |

**Powerloss Recovery Behavior:**
- Saat battery dicabut & dipasang kembali, slave akan **bunyi 3x peringatan** lalu **auto-resume** ke state RUNNING (bukan PAUSED).
- Safety: ada jeda 3 detik dengan bunyi peringatan sebelum auto-resume, memberi waktu staff menjauhkan tangan.
- Jika ingin mengubah ke mode PAUSED (tidak auto-resume), ubah `state = STATE_RUNNING` menjadi `state = STATE_PAUSED` di bagian Powerloss Recovery pada `wifi_slave.ino`.

---

## API Endpoint Master (192.168.4.1)

| Method | Endpoint | Fungsi |
|--------|----------|--------|
| GET | `/` | Dashboard WebUI |
| GET | `/api/slaves` | Status seluruh armada (JSON array) |
| POST | `/api/command` | Proxy command ke slave `{id, cmd, val}` |
| POST | `/api/transfer_time` | Transfer sisa waktu antar slave `{from_id, to_id}` |
| POST | `/api/edit_slave` | Ubah ID slave `{mac, id}` |
| POST | `/api/delete_slave` | Hapus slave dari registry `{mac}` |
| GET | `/api/register?mac=...` | Registrasi / heartbeat slave |

### Command List

| Command | Val | Deskripsi |
|---------|-----|-----------|
| ADD_TIME | detik | Tambah waktu (300 = 5 menit) |
| PAUSE | 0 | Jeda timer |
| RESUME | 0 | Lanjutkan timer |
| STOP | 0 | Kunci / reset timer ke 0 |
| IDENTIFY | 0 | Buzzer 3x + layar kedip (cari mainan) |
| REBOOT | 0 | Restart ESP32 slave |

Lihat spesifikasi lengkap di [docs/WIFI_API_SPEC.md](docs/WIFI_API_SPEC.md) dan [docs/openapi.yaml](docs/openapi.yaml).

---

## Paket Harga (Mall)

| Durasi | Harga |
|--------|-------|
| 5 menit | Rp 25.000 |
| 10 menit | Rp 40.000 |
| 15 menit | Rp 55.000 |

---

## Production Build

### Prasyarat
- **Board:** ESP32 (bukan Arduino Uno). Gunakan ESP32 DevKit V1 / DOIT.
- **Arduino IDE:** Install ESP32 board via Boards Manager (URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`)
- **Library yang dibutuhkan:**
  - `TM1637Display` (by Avishay Orpaz) — via Library Manager
  - `WiFi`, `WebServer`, `HTTPClient`, `Preferences` — bawaan ESP32 core

### Upload Master
1. Buka `firmware/wifi_master/wifi_master.ino` di Arduino IDE.
2. Pilih board: `ESP32 Dev Module` atau `DOIT ESP32 DEVKIT V1`.
3. Upload ke ESP32 yang akan jadi router/server pusat.
4. Buka Serial Monitor (115200 baud) untuk melihat log.
5. Dashboard tersedia di `http://192.168.4.1`.

### Upload Slave
1. Buka `firmware/wifi_slave/wifi_slave.ino` di Arduino IDE.
2. Sesuaikan SSID/password WiFi di kode jika Master diubah.
3. Upload ke ESP32 di masing-masing excavator.
4. Slave akan otomatis daftar ke Master (Zero-Touch Provisioning).

---

## Troubleshooting

### Slave tidak muncul di Dashboard
1. Pastikan Master sudah menyala dan memancarkan Wi-Fi `ExcavatorMaster`.
2. Cek Serial Monitor slave (115200 baud) — lihat log `[WIFI]` dan `[API]`.
3. Pastikan slave dalam jangkauan Wi-Fi Master.
4. Jika MAC sudah terdaftar tapi ID salah, gunakan fitur **Edit** di modal Manage Slaves.

### Timer tidak jalan setelah battery diganti
- Slave akan auto-resume setelah 3 detik peringatan bunyi.
- Jika tetap LOCKED, cek NVS — sisa waktu mungkin sudah 0 sebelum battery dicabut.

### Master mati sementara
- Slave tetap menghitung timer lokal walau master/AP mati.
- Saat Wi-Fi kembali, slave akan register ulang lalu master membaca state terbaru dari `/api/state`.
- Dashboard master akan menandai slave offline setelah 30 detik tanpa heartbeat.

### Master hang / tidak responsif
- Hardware watchdog akan auto-reboot Master setelah 10 detik hang.
- Cek Serial Monitor untuk error `[PROXY]` atau `[REGISTRY]`.

### Kompilasi gagal
- Pastikan ESP32 core versi 3.x terinstall.
- Library TM1637Display harus terinstall via Library Manager.
- Jika error `esp_task_wdt_init`, pastikan kode menggunakan config struct (sudah diperbaiki untuk core 3.3.0+).

---

## Dokumen Terkait

| Dokumen | Isi |
|---------|-----|
| [MVP_SPEC.md](MVP_SPEC.md) | Spesifikasi MVP lengkap |
| [PRD.md](PRD.md) | Product Requirement Document |
| [docs/WIFI_API_SPEC.md](docs/WIFI_API_SPEC.md) | Spesifikasi API untuk Android |
| [docs/openapi.yaml](docs/openapi.yaml) | OpenAPI 3.0 spec (Swagger) |
| [docs/ANDROID_APP_FLOW.md](docs/ANDROID_APP_FLOW.md) | Flow aplikasi Android |
| [docs/RELAY_WIRING.md](docs/RELAY_WIRING.md) | Wiring relay & pinout |
| [docs/MERMAID_DIAGRAMS.md](docs/MERMAID_DIAGRAMS.md) | Diagram arsitektur & flow |
| [docs/excavator-rental-module-design.md](docs/excavator-rental-module-design.md) | Desain konsep modul |

---

## Keamanan

- Sistem **tidak terhubung internet** — hanya LAN Wi-Fi AP Master lokal.
- Slave terhubung via Wi-Fi dengan password.
- MAC address digunakan sebagai identitas unik hardware.
- Tidak ada enkripsi data (MVP) — network terisolasi dari internet.

## Lisensi

Proprietary — Excavator Timer Rental
