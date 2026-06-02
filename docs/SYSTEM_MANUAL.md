# Excavator Rental Timer - System Manual

Sistem timer rental excavator RC berbasis Wi-Fi dengan arsitektur Master-Slave.

---

## 1. Arsitektur Sistem

```
┌─────────────────┐     Wi-Fi AP      ┌─────────────────┐
│  Master ESP32   │◄──────────────────►│  Slave ESP32    │
│  (192.168.4.1)  │    (ExcavatorMaster)│  (192.168.4.x)  │
│                 │                    │                 │
│  - DHCP Server  │                    │  - Timer        │
│  - Web Dashboard│                    │  - Relay        │
│  - REST API     │                    │  - TM1637       │
│  - User Auth    │                    │  - Buzzer       │
│  - History/Rev  │                    │  - Button       │
└─────────────────┘                    └─────────────────┘
        ▲                                      ▲
        │                                      │
   Android App                           Excavator RC
   (http://192.168.4.1)
```

### Komponen
| Komponen | Fungsi | Lokasi |
|----------|--------|--------|
| Master ESP32 | AP, API, Dashboard, Auth, History | Meja kasir |
| Slave ESP32 | Timer, Relay, Display, Buzzer | Dalam excavator |
| TM1637 | Display 4-digit MM:SS | Dalam excavator |
| Relay | ON/OFF power excavator | Dalam excavator |
| Buzzer | Notifikasi suara | Dalam excavator |
| Button | Resume manual | Dalam excavator |

---

## 2. Wiring

### Master
Tidak perlu wiring. Cukup colok USB 5V ke ESP32.

### Slave (ESP32 DevKit V1)
| Pin | Fungsi | Ke |
|-----|--------|-----|
| GPIO 22 | TM1637 CLK | Display CLK |
| GPIO 23 | TM1637 DIO | Display DIO |
| GPIO 26 | Relay IN | Relay Module |
| GPIO 27 | Buzzer + | Buzzer aktif |
| GPIO 32 | Button | Push button (pull-up) |
| GPIO 2 | LED | LED built-in (aktif LOW) |

### Slave (ESP8266/NodeMCU)
| Pin | Fungsi | Ke |
|-----|--------|-----|
| D1 (GPIO5) | Relay | Relay Module |
| D2 (GPIO4) | Buzzer | Buzzer aktif |
| D5 (GPIO14) | Button | Push button |
| D6 (GPIO12) | TM1637 CLK | Display CLK |
| D7 (GPIO13) | TM1637 DIO | Display DIO |
| D4 (GPIO2) | LED1 | LED relay state |
| D0 (GPIO16) | LED2 | LED activity |

---

## 3. Wi-Fi Configuration

| Parameter | Nilai |
|-----------|-------|
| SSID | `ExcavatorMaster` |
| Password | `12345678` |
| Master IP | `192.168.4.1` |
| Slave IP | DHCP (`192.168.4.2`, `.3`, dst) |

**Cara akses:**
1. Sambungkan HP/laptop ke WiFi `ExcavatorMaster`
2. Buka browser → `http://192.168.4.1`

---

## 4. Login & User Management

### Default Credentials
| Username | Password | Role |
|----------|----------|------|
| `superadmin` | `super123` | SuperAdmin |

### Role Hierarchy
| Role | Fitur |
|------|-------|
| **SuperAdmin (0)** | Semua fitur + kelola user + reboot device |
| **Admin (1)** | Semua fitur + reset data + edit package |
| **Staff (2)** | Add time, pause, resume, stop |

### Login Flow
1. Buka `http://192.168.4.1`
2. Masukkan username & password
3. Token disimpan di browser (localStorage)
4. Token berlaku 24 jam

---

## 5. Paket Waktu & Harga

### Paket yang Tersedia
| Set | Durasi | Harga Default |
|-----|--------|---------------|
| 1 | 1 menit | Rp 5.000 |
| 2 | 2 menit | Rp 8.000 |
| 3 | 3 menit | Rp 10.000 |
| 5 | 5 menit | Rp 15.000 |
| 10 | 10 menit | Rp 25.000 |
| 30 | 30 menit | Rp 60.000 |
| 60 | 60 menit | Rp 100.000 |

### Cara Mengubah Harga
1. Login sebagai SuperAdmin/Admin
2. Buka tab **Admin**
3. Klik paket yang ingin diubah
4. Masukkan harga baru (Rupiah)
5. Klik **Simpan**

---

## 6. Dashboard Features

### Dashboard Tab (Semua Role)
- **Device Cards:** Status real-time setiap excavator
- **Package Buttons:** Klik untuk tambah waktu
- **Pause/Resume/Stop:** Kontrol timer
- **History Panel:** Total waktu main, jumlah sesi, sesi terakhir, total pendapatan

### Pendapatan Tab (Admin+)
- **Total Pendapatan:** Semua slave digabung
- **Per-Slave:** Waktu main, sesi, pendapatan per excavator
- **Reset Buttons:** Reset Waktu, Reset Pendapatan, Reset Semua

### Admin Tab (SuperAdmin)
- **Package Management:** Ubah harga paket
- **User Management:** Tambah/hapus user
- **Change Password:** Ubah password sendiri

---

## 7. Powerloss Recovery

### Cara Kerja
1. Saat timer aktif, `remaining` disimpan ke NVS/EEPROM setiap 30 detik
2. Saat daya hilang, slave mati
3. Saat baterai diganti, slave boot
4. Cek NVS/EEPROM → ada sisa waktu → auto-resume RUNNING
5. Timer melanjutkan dari sisa waktu

### Safety Warning
- Buzzer berbunyi 3x (3 detik) sebelum auto-resume
- Staff harus cabut tangan dari excavator

---

## 8. Transfer Waktu

### Cara Kerja
1. Pilih excavator sumber (yang rusak)
2. Klik **Transfer**
3. Pilih excavator target (cadangan)
4. Master akan:
   - Stop sumber → ambil sisa waktu
   - Stop target (replace waktu yang ada)
   - Kirim waktu ke target

### Error Handling
| Error | Penyebab |
|-------|----------|
| Cannot transfer to self | Sumber = target |
| No time to transfer | Sumber rem = 0 |
| Slaves not found | Salah satu offline |
| Target is unreachable | Target tidak merespon |

---

## 9. History & Revenue Tracking

### Data yang Disimpan
| Field | Deskripsi |
|-------|-----------|
| `totalSec` | Total waktu bermain (detik) |
| `sessions` | Jumlah sesi bermain |
| `lastSec` | Durasi sesi terakhir |
| `revenueIDR` | Total pendapatan (Rupiah) |

### Kapan Dicatat
- Setiap kali session berakhir (RUNNING/PAUSED → LOCKED/ENDED)
- Master mendeteksi perubahan state via poll task (1 detik)
- Harga dicari dari paket terdekat (closest match)

### Reset Data
| Endpoint | Reset Apa | Auth |
|----------|-----------|------|
| `POST /api/history/reset` | totalSec, sessions | Admin + Password |
| `POST /api/revenue/reset` | revenueIDR | Admin + Password |
| `POST /api/reset-all` | Semua | Admin + Password |

---

## 10. LED Indicators

### Master ESP32
| LED | Status |
|-----|--------|
| ON solid | AP aktif |
| Blink singkat (80ms) | Polling/registrasi/request |

### Slave ESP32
| LED | Status |
|-----|--------|
| Blink lambat | Menghubungkan WiFi |
| Blink cepat | Terdaftar, mengirim heartbeat |
| Flash saat command | Menerima perintah |

### Slave ESP8266
| LED | Fungsi |
|-----|--------|
| LED1 (D4) | Relay state — ON steady saat relay aktif |
| LED2 (D0) | Activity — flash saat ada aktivitas |

---

## 11. API Endpoints

### Public (Tanpa Auth)
| Method | Endpoint | Deskripsi |
|--------|----------|-----------|
| GET | `/api/slaves` | Status semua slave |
| GET | `/api/packages` | Daftar paket & harga |
| GET | `/api/history` | Riwayat penggunaan |
| GET | `/api/register?mac=XX` | Registrasi slave (internal) |

### Protected (Perlu Token)
| Method | Endpoint | Auth | Deskripsi |
|--------|----------|------|-----------|
| POST | `/api/login` | Public | Login |
| POST | `/api/logout` | Token | Logout |
| POST | `/api/command` | Staff+ | Kirim command ke slave |
| POST | `/api/transfer_time` | Admin+ | Transfer waktu |
| POST | `/api/edit_slave` | Admin+ | Ubah ID slave |
| POST | `/api/delete_slave` | Admin+ | Hapus slave |
| GET | `/api/users` | Admin+ | Daftar user |
| POST | `/api/users` | SuperAdmin | Tambah user |
| POST | `/api/users/delete` | SuperAdmin | Hapus user |
| POST | `/api/users/change-password` | Token | Ubah password |
| POST | `/api/packages/update` | Admin+ | Ubah harga paket |
| GET | `/api/revenue` | Admin+ | Pendapatan per slave |
| POST | `/api/history/reset` | Admin+ | Reset riwayat |
| POST | `/api/revenue/reset` | Admin+ | Reset pendapatan |
| POST | `/api/reset-all` | Admin+ | Reset semua |

### ADD_TIME Set Numbers
| val | Durasi | Harga Contoh |
|-----|--------|---------------|
| 1 | 1 menit | Rp 5.000 |
| 2 | 2 menit | Rp 8.000 |
| 3 | 3 menit | Rp 10.000 |
| 5 | 5 menit | Rp 15.000 |
| 10 | 10 menit | Rp 25.000 |
| 30 | 30 menit | Rp 60.000 |
| 60 | 60 menit | Rp 100.000 |

---

## 12. Troubleshooting

| Masalah | Solusi |
|---------|--------|
| Slave tidak muncul di dashboard | Pastikan slave menyala dan terhubung ke WiFi ExcavatorMaster |
| Timer tidak jalan | Cek apakah state = RUNNING, coba PAUSE lalu RESUME |
| Relay tidak aktif | Cek wiring relay, pastikan GPIO 26 benar |
| Display tidak menyala | Cek wiring TM1637, pastikan power 5V |
| Login gagal | Pastikan username/password benar, cek console browser |
| Token expired | Login ulang, token berlaku 24 jam |
| WiFi putus | Slave otomatis reconnect, tunggu 5-10 detik |
| Transfer gagal | Pastikan kedua slave online, source punya sisa waktu |

---

## 13. File Struktur

```
firmware/
├── wifi_master/main.cpp      # Master firmware (ESP32)
├── wifi_slave/main.cpp       # Slave firmware (ESP32)
└── wifi_slave_8266/main.cpp  # Slave firmware (ESP8266/NodeMCU)

docs/
├── WIFI_API_SPEC.md          # API documentation
├── SYSTEM_MANUAL.md          # Dokumen ini
├── ANDROID_APP_FLOW.md       # Flow aplikasi Android
├── RELAY_WIRING.md           # Diagram wiring relay
├── MERMAID_DIAGRAMS.md       # Diagram alur
└── api_documentation.md      # API docs (alternatif)

platformio.ini                # Build configuration
scratch.py / stress_test.py   # Test scripts
```

---

## 14. Build & Upload

### Build
```bash
pio run -e master      # Build master
pio run -e slave       # Build slave ESP32
pio run -e slave8266   # Build slave ESP8266
```

### Upload
```bash
pio run -e master -t upload --upload-port COM27
pio run -e slave -t upload --upload-port COM26
pio run -e slave8266 -t upload --upload-port COM6
```

### Serial Monitor
```bash
pio device monitor --port COM27 --baud 115200  # Master
pio device monitor --port COM26 --baud 115200  # Slave ESP32
pio device monitor --port COM6 --baud 115200   # Slave ESP8266
```
