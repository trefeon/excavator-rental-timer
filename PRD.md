# PRD — Excavator Rental Timer Module

## 1. Ringkasan

Produk ini adalah module timer rental murah untuk mainan excavator remote control. Module dipasang ke mainan, memutus/menyambung power mainan memakai relay, dan dikontrol secara terpusat dari Web Dashboard atau aplikasi Android via jaringan Wi-Fi lokal.

Sistem menggunakan arsitektur **Master-Slave**:
- **1 Master (ESP32):** Access Point Wi-Fi, Web Server Dashboard, API Gateway.
- **Banyak Slave (ESP32):** Dipasang di setiap mainan. Terkoneksi otomatis ke Master (Zero-Touch Provisioning).

Customer tetap memakai remote RC bawaan. Staff/operator memakai WebUI atau Android app untuk melihat status mainan, menambah waktu, pause/resume, stop, transfer waktu, dan mengelola registry unit.

## 2. Keputusan Produk

MVP fokus ke pedagang rental mainan murah.

Keputusan MVP:
- Wi-Fi Master-Slave, tanpa internet, tanpa cloud.
- Master menjadi Access Point mandiri (`192.168.4.1`).
- Slave otomatis terhubung ke Master saat dinyalakan (Zero-Touch Provisioning).
- Android / Browser hanya connect ke Master (API Proxy Gateway).
- Slave menjadi eksekutor timer dan pengontrol relay lokal.
- 1x 18650 shared untuk ESP32 Slave dan mainan.
- Saat battery diganti, ESP32 Slave boleh mati — timer survive via NVS.
- Setelah battery baru, slave auto-resume setelah 3 detik peringatan bunyi.
- Relay murah 3.3V dipakai sebagai power gate.
- TM1637 4-digit display ditempel di mainan untuk sisa waktu.

## 3. Masalah

Pedagang rental mainan di mall butuh cara murah untuk menyewakan banyak mainan excavator RC tanpa harus memodifikasi remote/motor control utama.

Masalah:
- Staff perlu mengatur waktu sewa cepat dari HP/tablet.
- Banyak mainan perlu terlihat statusnya sekaligus secara real-time.
- Mainan harus mati otomatis saat waktu habis.
- Sisa waktu harus aman saat battery 18650 diganti.
- Customer/staff perlu melihat sisa waktu langsung di mainan.
- Solusi harus murah, terkoneksi tanpa kabel, dan mudah dipahami pedagang.

## 4. Tujuan

- Staff bisa melihat hingga 9 mainan online bersamaan dari satu Dashboard (Web / Android) via Master.
- Staff bisa menambah waktu per 5 menit.
- Mainan hidup hanya saat sesi aktif.
- Mainan mati otomatis saat timer habis.
- Timer survive battery hotswap (simpan ke NVS).
- Sisa waktu tampil di TM1637 display pada mainan.
- Command proxy via Master terkirim dengan mulus ke Slave.
- Hardware bisa dibuat dari komponen murah dan umum.
- Sistem thread-safe (dual-core mutex) dan punya hardware watchdog.

## 5. Non-Tujuan

- Tidak mengganti remote RC bawaan.
- Tidak membuat joystick/control motor dari HP.
- Tidak membutuhkan router internet mall.
- Tidak memakai cloud/server eksternal untuk MVP.
- Tidak memakai Bluetooth/BLE (semua via Wi-Fi).

## 6. Pengguna

### Pedagang/Owner
Butuh produk murah, bisa dipasang ke banyak mainan, minim training, otomatis terdeteksi, mudah diperbaiki.

### Staff Operator
Mengoperasikan rental harian. Butuh dashboard cepat (via browser HP ke IP Master), status jelas, tombol utama: `+5 menit`, `Pause`, `Resume`, `Stop`, `Transfer Waktu`.

### Customer
Memainkan excavator pakai remote RC bawaan. Melihat sisa waktu dari display di mainan.

## 7. Alur Utama

### 7.1 Auto Provisioning (Zero-Touch)
```
Staff menyalakan mainan baru (ESP32 Slave).
Slave otomatis scan dan connect ke Wi-Fi Master.
Slave panggil /api/register dengan MAC address.
Master daftarkan MAC ke NVS, berikan ID (EXC-03).
Slave langsung tampil di Dashboard Master.
```

### 7.2 Mulai Rental
```
Staff buka WebUI Master / Android App (192.168.4.1).
Dashboard auto-refresh tampilkan semua mainan.
Staff pilih EXC-01, tekan +5 menit.
Master teruskan command ke Slave via HTTP lokal.
Slave validasi command, simpan ke NVS, ON-kan relay.
TM1637 tampil MM:SS. Customer pakai remote RC bawaan.
```

### 7.3 Timer Habis
```
remaining_seconds di Slave mencapai 0.
Slave relay OFF, TM1637 tampil ----, state = ENDED.
Master polling status terbaru, Dashboard update.
```

### 7.4 Battery Hotswap
```
Saat RUNNING, Slave simpan remaining_seconds tiap 30 detik.
Staff cabut 18650. Slave mati, Relay OFF.
Staff pasang 18650 baru. Slave boot -> connect Master.
Slave load remaining_seconds dari NVS.
Bunyi peringatan 3x, lalu auto-resume ke RUNNING.
Relay ON lagi.
```

## 8. Arsitektur Sistem

```
Android App / Web Browser
  |
  | HTTP API (192.168.4.1)
  v
Master ESP32 (Access Point + API Gateway)
  | (Core 0: Background Polling, Core 1: Web Server + Mutex)
  |------------------------|
  v                        v
Slave ESP32 (192.168.4.x)  Slave ESP32 (192.168.4.y)
  |                        |
  +-> TM1637 Display       +-> TM1637 Display
  +-> Relay -> Power       +-> Relay -> Power
  +-> Buzzer               +-> Buzzer
  +-> Button Resume        +-> Button Resume
```

## 9. Kebutuhan Hardware

Per unit Slave:

| Komponen | Spesifikasi |
|----------|-------------|
| ESP32 | Wi-Fi client, NVS |
| 18650 holder | Mudah diganti staff |
| 3.3V buck converter | Bukan AMS1117 |
| Relay 3V/3.3V | NO contact, 3A minimum, 5A preferred |
| TM1637 4-digit display | Ditempel di mainan |
| Buzzer aktif | Feedback audio |
| Button | Resume manual |
| Fuse/PTC | 2A-5A sesuai arus mainan |
| Kapasitor | 470uF-1000uF dekat beban |

## 10. Kebutuhan Firmware

**Master:**
- AP mode (DHCP Server) — `192.168.4.1`.
- Core 0: Background task polling setiap Slave via `GET /api/state`.
- Core 1: Web Server + API endpoint.
- Registry MAC -> ID di NVS.
- Mutex dual-core untuk thread safety.
- Hardware watchdog 10 detik.

**Slave:**
- WiFi Station mode, connect ke Master.
- Zero-Touch Provisioning via `/api/register`.
- Start dengan relay OFF.
- Load sesi tersimpan dari NVS.
- Auto-resume ke RUNNING jika ada sisa waktu (dengan peringatan 3 detik).
- Timer countdown lokal.
- TM1637 display update.
- NVS save tiap 30 detik + setiap perubahan state.
- WiFi auto-reconnect via `WiFi.onEvent()`.
- Server tetap berjalan saat WiFi putus.
- Hardware watchdog 10 detik.

## 11. State Machine

| State | Relay | Display | Arti |
|-------|-------|---------|------|
| `LOCKED` | OFF | `----` | Tidak ada sesi |
| `RUNNING` | ON | `MM:SS` (kedip) | Timer aktif |
| `PAUSED` | OFF | `MM:SS` (kedip) | Dijeda |
| `ENDED` | OFF | `----` | Waktu habis |
| `FAULT` | OFF | `----` | Error |

## 12. API Spesifikasi

Referensi penuh: [docs/WIFI_API_SPEC.md](docs/WIFI_API_SPEC.md) dan [docs/openapi.yaml](docs/openapi.yaml).

Endpoint utama Master (`192.168.4.1`):
- `GET /api/slaves` — status seluruh armada.
- `POST /api/command` — proxy command ke slave `{id, cmd, val}`.
- `POST /api/transfer_time` — transfer sisa waktu.
- `POST /api/edit_slave` — ubah ID.
- `POST /api/delete_slave` — hapus dari registry.

Commands: `ADD_TIME`, `PAUSE`, `RESUME`, `STOP`, `IDENTIFY`, `REBOOT`.

## 13. Keamanan

- Command hanya bisa via jaringan Wi-Fi AP Master (terisolasi).
- Baterai dicabut -> ada jeda peringatan 3 detik sebelum auto-resume.
- Hardware watchdog auto-reboot jika sistem hang.
- Mutex mencegah data corruption antar core.

## 14. Kriteria Penerimaan

MVP diterima jika:
- [x] Master memancarkan Wi-Fi AP `ExcavatorMaster`.
- [x] Slave otomatis daftar ke Master saat dinyalakan.
- [x] Web Dashboard di Master menampilkan semua Slave.
- [x] Command Tambah Waktu berhasil menyalakan relay Slave.
- [x] Timer berhitung mundur dan TM1637 hidup.
- [x] Baterai cabut-pasang tidak mereset timer (auto-resume setelah peringatan).
- [x] Master mengetahui jika Slave OFFLINE (threshold 30 detik).
- [x] Master WebUI memiliki fitur ubah ID / Hapus Slave / Transfer Waktu.
- [x] Sistem thread-safe (mutex) + hardware watchdog.
- [x] WiFi auto-reconnect tanpa blokir fungsi Slave.
