# Excavator Rental Timer - System Manual (Current State)

Last Updated: June 3, 2026

Sistem timer rental excavator RC berbasis Wi-Fi dengan arsitektur Master-Slave.

---

## 1. Arsitektur Sistem

```text
┌─────────────────┐     Wi-Fi AP       ┌─────────────────┐
│  Master ESP32   │◄──────────────────►│  Slave ESP32    │
│  (192.168.4.1)  │ (ExcavatorMaster)  │  (192.168.4.x)  │
│                 │                    │                 │
│  - DHCP Server  │                    │  - Timer        │
│  - Web Dashboard│                    │  - Relay        │
│  - REST API     │                    │  - TM1637       │
│  - History/Rev  │                    │  - Buzzer       │
└─────────────────┘                    │  - Button       │
        ▲                              └─────────────────┘
        │                                      ▲
   Android App                           Excavator RC
   (http://192.168.4.1)
```

### Komponen

| Komponen            | Fungsi                                                   |
| ------------------- | -------------------------------------------------------- |
| Master ESP32 (V1)   | Access Point, API, Web Dashboard, History Tracking       |
| Slave ESP32 (V1/C3) | Mengatur Timer, Relay, Display, Buzzer                   |
| Slave ESP8266       | Alternatif modul timer, Relay, Display, Buzzer           |
| TM1637              | Display 4-digit MM:SS                                    |
| Relay               | ON/OFF power excavator                                   |
| Buzzer              | Notifikasi suara (contoh saat transfer atau waktu habis) |

---

## 2. Wiring

### Master

Cukup colok USB 5V ke ESP32 DOIT DevKit V1. Tidak ada wiring eksternal.
LED indikator menggunakan GPIO 2.

### Slave (ESP32 DevKit V1)

| Pin     | Fungsi     | Ke                       |
| ------- | ---------- | ------------------------ |
| GPIO 22 | TM1637 CLK | Display CLK              |
| GPIO 23 | TM1637 DIO | Display DIO              |
| GPIO 26 | Relay IN   | Relay Module             |
| GPIO 27 | Buzzer +   | Buzzer aktif             |
| GPIO 32 | Button     | Push button (pull-up)    |
| GPIO 2  | LED        | LED built-in (aktif LOW) |

### Slave (ESP32-C3 Super Mini)

| Pin    | Fungsi     | Ke                       |
| ------ | ---------- | ------------------------ |
| GPIO 4 | TM1637 CLK | Display CLK              |
| GPIO 5 | TM1637 DIO | Display DIO              |
| GPIO 6 | Relay IN   | Relay Module             |
| GPIO 7 | Buzzer +   | Buzzer aktif             |
| GPIO 9 | Button     | Push button (pull-up)    |
| GPIO 8 | LED        | LED built-in (aktif LOW) |

### Slave (ESP8266/NodeMCU)

| Pin         | Fungsi     | Ke              |
| ----------- | ---------- | --------------- |
| D1 (GPIO5)  | Relay      | Relay Module    |
| D2 (GPIO4)  | Buzzer     | Buzzer aktif    |
| D5 (GPIO14) | Button     | Push button     |
| D6 (GPIO12) | TM1637 CLK | Display CLK     |
| D7 (GPIO13) | TM1637 DIO | Display DIO     |
| D4 (GPIO2)  | LED1       | LED relay state |
| D0 (GPIO16) | LED2       | LED activity    |

---

## 3. Wi-Fi Configuration

| Parameter | Nilai                                     |
| --------- | ----------------------------------------- |
| SSID      | `ExcavatorMaster`                         |
| Password  | `12345678`                                |
| Master IP | `192.168.4.1`                             |
| Slave IP  | DHCP (otomatis, misal `192.168.4.2`, dll) |

---

## 4. REST API (Open Endpoints)

**Catatan Penting:** Auth di-handle sepenuhnya oleh sisi aplikasi Android. Master ESP32 saat ini membiarkan seluruh endpoint terbuka (open access) untuk kemudahan integrasi dengan aplikasi Android.

### Endpoints

| Method | Endpoint               | Description                                      |
| ------ | ---------------------- | ------------------------------------------------ |
| GET    | `/api/slaves`          | Status semua slave (IP, timer state, remaining)  |
| GET    | `/api/packages`        | Daftar paket & harga                             |
| GET    | `/api/history`         | Riwayat penggunaan (totalSec, sessions)          |
| POST   | `/api/command`         | Kirim command ke slave                           |
| POST   | `/api/transfer_time`   | Transfer sisa waktu antar slave                  |
| POST   | `/api/edit_slave`      | Ubah ID slave                                    |
| POST   | `/api/delete_slave`    | Hapus registrasi slave                           |
| POST   | `/api/packages/update` | Ubah harga paket                                 |
| GET    | `/api/revenue`         | Pendapatan per slave                             |
| POST   | `/api/history/reset`   | Reset riwayat permainan                          |
| POST   | `/api/revenue/reset`   | Reset pendapatan finansial                       |
| POST   | `/api/reset-all`       | Factory reset semua data history & revenue       |
| GET    | `/api/register?mac=XX` | Registrasi internal slave (dipanggil oleh slave) |

### Commands (POST `/api/command`)

Body berupa JSON: `{"id": 1, "cmd": "ADD_TIME", "val": 5}`

| cmd        | val  | Keterangan                                    |
| ---------- | ---- | --------------------------------------------- |
| `ADD_TIME` | 1-60 | Menambah waktu (dalam menit)                  |
| `PAUSE`    | 0    | Pause waktu berjalan                          |
| `RESUME`   | 0    | Lanjutkan waktu yang di-pause                 |
| `STOP`     | 0    | Reset waktu jadi 0 & lock relay               |
| `IDENTIFY` | 0    | Bunyikan buzzer slave 3x untuk mencari posisi |
| `REBOOT`   | 0    | Restart ESP slave secara software             |

### Default Paket Waktu

| Durasi (val) | Default Harga |
| ------------ | ------------- |
| 1 min        | Rp 5.000      |
| 2 min        | Rp 8.000      |
| 3 min        | Rp 10.000     |
| 5 min        | Rp 15.000     |
| 10 min       | Rp 25.000     |
| 30 min       | Rp 60.000     |
| 60 min       | Rp 100.000    |

---

## 5. Fitur Penting

1. **Powerloss Recovery**: Waktu (`remaining`) pada slave disimpan secara periodik ke dalam EEPROM (ESP8266) atau NVS (ESP32) setiap 30 detik. Jika listrik mati/baterai dicabut, slave akan resume sisa waktunya saat dinyalakan kembali.
2. **Timer Accuracy**: Logic loop non-blocking (ESP32) mencegah _drift_ pada timer dibandingkan mengandalkan `delay()`.
3. **Data Logging (History & Revenue)**: Dicatat secara independen di NVS Master tiap kali ada siklus permainan (berakhirnya state `RUNNING` menjadi `LOCKED`).
4. **Waktu Transfer**: Fitur pemindahan sisa waktu antar slave, otomatis me-reset sumber dan menggantikan sisa waktu yang ada di target.

---

## 6. Build & Test (PlatformIO)

**Build File:** `platformio.ini` memiliki environment yang tersedia: `master`, `slave`, `slave_c3`, dan `slave8266`.

### Build & Upload

```bash
# Compile Master

pio run -e master
pio run -e master -t upload --upload-port COM_MASTER

# Compile Slaves

pio run -e slave       # DevKit V1
pio run -e slave_c3    # C3 Super Mini
pio run -e slave8266   # NodeMCU ESP8266
```

### Stress Testing

Testing otomatis menggunakan `python scratch.py` (didukung opsi `--stress` untuk concurrent connection flooding, rogue testing, payload fuzzing, dan poll check).
