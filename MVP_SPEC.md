# Spesifikasi MVP — Excavator Rental Timer Wi-Fi Master-Slave

## 1. Keputusan Produk

MVP ini untuk pedagang rental mainan excavator RC di mall. Mainan tetap pakai remote RC bawaan. Module hanya menghidupkan/mematikan power mainan berdasarkan timer sewa.

Keputusan final MVP:

- Arsitektur Wi-Fi Master-Slave mandiri (tanpa internet, tanpa cloud).
- Master (ESP32) = Access Point + Web Server + API Proxy Gateway.
- Slave (ESP32) per mainan = kontrol relay + timer + NVS + TM1637 display.
- Zero-Touch Provisioning: Slave otomatis cari Master dan daftarkan diri.
- Android / Web Browser hanya komunikasi ke 1 IP Master (`192.168.4.1`).
- Battery 18650 dipakai bersama ESP32 Slave dan mainan.
- Saat battery diganti, ESP32 Slave boleh mati — timer survive via NVS.
- Setelah battery baru, slave **auto-resume** setelah 3 detik peringatan bunyi (safety: staff punya waktu menjauhkan tangan).
- Relay murah 3.3V sebagai power gate ke PCB mainan.
- TM1637 4-digit display ditempel di mainan untuk sisa waktu.

## 2. Target User

- Pedagang/operator rental di mall.
- Staff non-teknis harus bisa: tambah waktu, pause, resume, stop, ganti battery, rename/hapus unit dari Dashboard.
- Customer pakai remote RC bawaan, tidak pakai app.

## 3. Arsitektur Sistem

```
Android App / Web Browser
  |
  | HTTP API GET/POST (192.168.4.1)
  |
Master ESP32 (Access Point & API Gateway)
  |
  | Background Polling (Core 0) & Proxy Command
  |
Slave ESP32 (per mainan, IP DHCP dari Master)
  |
  +-> TM1637 4-digit display ditempel di mainan
  +-> GPIO relay control -> Relay -> Power Mainan
  +-> Buzzer (feedback audio)
  +-> Button fisik (resume manual)
```

**Master (Core 0 — Polling Task):**
- Polling GET `/api/state` ke setiap slave yang online.
- Update RAM registry dengan state, sisa waktu, baterai.
- LED indikator polling aktif.

**Master (Core 1 — Web Server):**
- Dashboard WebUI di `/`.
- API `/api/slaves` (JSON array status semua mainan).
- API `/api/command` (proxy command ke slave).
- API `/api/transfer_time`, `/api/edit_slave`, `/api/delete_slave`.
- API `/api/register` (zero-touch provisioning + heartbeat).
- **Thread safety:** Mutex (`SemaphoreHandle_t`) melindungi akses array `slaves[]` antara Core 0 dan Core 1.

**Slave:**
- WiFi Station mode, connect ke Master.
- Web server lokal (`/api/state`, `/api/command`).
- Timer countdown lokal — independen dari Master.
- NVS save setiap 30 detik dan setiap perubahan state.
- WiFi auto-reconnect via `WiFi.onEvent()` + `WiFi.setAutoReconnect(true)`.
- Server tetap berjalan meski WiFi putus.
- Hardware watchdog 10 detik.

## 4. Hardware

### Master Unit
1x ESP32 DevKit V1, USB 5V, di meja kasir.

### Slave Unit (per mainan)

| Item | Catatan |
|------|---------|
| ESP32 DevKit V1 | Otak timer + WiFi client |
| 1x 18650 holder | Battery mudah diganti staff |
| 3.3V buck converter | Bukan AMS1117 (inefisien) |
| Relay module 3.3V 1-ch | Contact min 3A, ideal 5A |
| TM1637 4-digit display | Countdown MM:SS, ditempel |
| Buzzer aktif | Feedback audio (GPIO 27) |
| Button fisik | Resume manual (GPIO 32, pull-up) |
| Fuse/PTC | 2A-5A sesuai arus mainan |

## 5. Wiring Slave

| Komponen | Pin ESP32 |
|----------|----------|
| TM1637 CLK | GPIO 22 |
| TM1637 DIO | GPIO 23 |
| Relay IN | GPIO 26 |
| Buzzer (+) | GPIO 27 |
| Button (Resume) | GPIO 32 |
| LED (built-in) | GPIO 2 |

Relay memutus jalur **positive** battery ke PCB mainan:
```
Battery + -> fuse/PTC -> relay COM
relay NO -> PCB mainan +
Battery - -> PCB mainan -
```

Default safety: relay OFF saat boot, reset, timer habis, atau fault.

## 6. Timer Ownership

**Slave adalah source of truth** untuk timer dan relay.

Master:
- Proxy perintah (API Gateway).
- Menyimpan registry (MAC -> ID) di NVS.
- Polling status semua slave secara periodik.

Slave:
- Menyimpan `remaining_seconds` di NVS.
- Countdown lokal.
- Mematikan relay saat waktu habis.
- Tetap aman walau Master mati sementara.

## 7. Hotswap Battery (Powerloss Recovery)

```
RUNNING
  -> Slave simpan remaining_seconds ke NVS tiap 30 detik
  -> Battery dicabut
  -> Slave mati, relay OFF
  -> Battery baru dipasang
  -> Slave boot & connect ke Master
  -> Load remaining_seconds dari NVS
  -> BUNYI PERINGATAN 3x (3 detik jeda) — staff menjauhkan tangan
  -> Auto-resume ke RUNNING, relay ON
```

**Mengapa auto-resume (bukan PAUSED)?**
Di lapangan, staff ganti battery saat mainan sedang dimainkan customer. Auto-resume dengan jeda peringatan 3 detik lebih praktis daripada harus buka dashboard dan tekan Resume setiap kali ganti battery. Jika ingin mode PAUSED (lebih konservatif), ubah `state = STATE_RUNNING` menjadi `state = STATE_PAUSED` di bagian Powerloss Recovery pada `wifi_slave.ino`.

## 8. State Machine (Slave)

```
LOCKED → RUNNING → PAUSED → RUNNING
  ↑         ↓         ↓
  └─── ENDED ◄────────┘
```

| State | Relay | Display | Arti |
|-------|-------|---------|------|
| `LOCKED` | OFF | `----` | Tidak ada sesi |
| `RUNNING` | ON | `MM:SS` (kedip) | Timer berjalan |
| `PAUSED` | OFF | `MM:SS` (kedip) | Dijeda, ada sisa waktu |
| `ENDED` | OFF | `----` | Waktu habis |
| `FAULT` | OFF | `----` | Error, butuh restart |

Boot rule:
```
if saved remaining_seconds > 0:
    state = RUNNING (auto-resume setelah 3 detik peringatan)
else:
    state = LOCKED
relay = OFF saat pertama boot
```

## 9. Dashboard Scope (WebUI & Android)

### WebUI Master (bawaan — `http://192.168.4.1`)

- Dropdown pilih mainan (hanya yang online).
- Tampilan sisa waktu besar + status + indikator warna.
- Tombol: `+5 menit`, `+10 menit`, `Pause/Resume`, `STOP` (dengan konfirmasi).
- Modal Transfer Waktu (pindah sisa waktu ke mainan lain).
- Modal Manage Slaves (edit ID, hapus, ping/identify).
- Modal API Docs (OpenAPI spec + copy to clipboard).
- Toast notifikasi (ganti alert).
- Loading indicator saat API call.
- ESC key + klik luar untuk tutup modal.
- Polling otomatis setiap 2 detik.

### Android App (via HTTP API)

- Cukup panggil `GET /api/slaves` dan `POST /api/command` ke Master.
- Tampilkan daftar mainan dengan status, sisa waktu, baterai.
- Tombol aksi per mainan.

## 10. Command Scope

| Command | Val | Deskripsi |
|---------|-----|-----------|
| `ADD_TIME` | detik | Tambah waktu. Jika LOCKED/ENDED → auto RUNNING |
| `PAUSE` | 0 | Jeda timer, relay OFF |
| `RESUME` | 0 | Lanjutkan timer, relay ON |
| `STOP` | 0 | Reset ke 0, state LOCKED |
| `IDENTIFY` | 0 | Buzzer 3x + layar kedip (cari fisik mainan) |
| `REBOOT` | 0 | Restart ESP32 slave |

## 11. Keamanan

- Tidak terekspos internet (LAN Wi-Fi AP tertutup).
- Password AP: `12345678` (ganti untuk produksi).
- MAC address sebagai identitas unik.
- Command hanya bisa via jaringan AP Master lokal.
- Battery dicabut → mainan tidak langsung hidup (ada jeda peringatan 3 detik).

## 12. Kriteria Penerimaan (Acceptance Criteria)

MVP selesai jika:

- [x] Master memancarkan AP `ExcavatorMaster` (192.168.4.1).
- [x] Slave otomatis daftar ke Master saat dinyalakan.
- [x] Dashboard WebUI menampilkan semua slave yang terdaftar.
- [x] `+5 menit` menambah timer dan menyalakan relay.
- [x] Timer berjalan lokal di Slave (independen dari Master).
- [x] TM1637 menampilkan sisa waktu.
- [x] Relay mati saat timer habis.
- [x] Timer survive battery hotswap (Powerloss Recovery via NVS).
- [x] Master mengetahui jika Slave OFFLINE (threshold 30 detik).
- [x] Operator bisa ubah ID / hapus Slave dari Dashboard.
- [x] Transfer sisa waktu antar mainan.
- [x] Hardware watchdog auto-reboot jika hang.
- [x] WiFi auto-reconnect tanpa blokir loop.
- [x] Thread-safe registry (mutex dual-core).

## 13. Catatan Produksi

### Yang sudah diimplementasi
- Mutex dual-core, watchdog, HTTP timeout, WiFi auto-reconnect, JSON parser yang robust.
- UI: toast notifikasi, STOP konfirmasi, loading indicator, ESC key, status indicator warna.

### Yang belum (roadmap selanjutnya)
- Monitoring baterai real (ADC) — saat ini `bat` selalu "OK".
- OTA firmware update.
- Enkripsi / command signing.
- Persistent transaction log.
- Static IP assignment untuk slave.
- Mode PAUSED sebagai opsi konfigurasi powerloss recovery.
