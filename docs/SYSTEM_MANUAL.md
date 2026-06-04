# Excavator Rental Timer - System Manual

Last Updated: June 4, 2026

Sistem timer rental excavator RC berbasis Wi-Fi dengan arsitektur Master-Slave.

---

## 1. Arsitektur Sistem

```text
┌─────────────────┐                    ┌─────────────────┐
│  Android App    │    Wi-Fi AP        │  Slave ESP32    │
│  (Dashboard +   │◄──────────────────►│  (192.168.4.x)  │
│   Bisnis Logic) │ (ExcavatorMaster)  │                 │
│                 │                    │  - Timer        │
│  - User Auth    │   ┌────────────┐   │  - Relay        │
│  - Pricing      │◄─►│Master ESP32│◄─►│  - TM1637       │
│  - History      │   │(Bridge API)│   │  - Buzzer       │
│  - Revenue      │   │192.168.4.1 │   │  - Button       │
└─────────────────┘   └────────────┘   └─────────────────┘
```

> **PENTING:** Master ESP32 hanya berfungsi sebagai **jembatan (bridge)** antara Android App dan Slave. Master TIDAK menyimpan harga, histori, revenue, atau user data. Semua logika bisnis ditangani oleh aplikasi Android.

### Komponen

| Komponen            | Fungsi                                                   |
| ------------------- | -------------------------------------------------------- |
| Master ESP32 (V1)   | Access Point, Bridge API (proxy command ke slave)        |
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
| GPIO 4 | Relay IN   | Relay Module             |
| GPIO 5 | Buzzer +   | Buzzer aktif             |
| GPIO 6 | TM1637 CLK | Display CLK              |
| GPIO 7 | TM1637 DIO | Display DIO              |
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

## 4. REST API (Master Bridge Endpoints)

**Catatan Penting:** Master hanya meneruskan (proxy) perintah ke Slave. Auth di-handle sepenuhnya oleh aplikasi Android. Semua endpoint terbuka (open access).

### Endpoints

| Method | Endpoint            | Description                                     |
| ------ | ------------------- | ----------------------------------------------- |
| GET    | `/api/slaves`       | Status semua slave (IP, state, time_left, dll)  |
| POST   | `/api/command`      | Kirim command ke slave (proxy)                  |
| POST   | `/api/edit_slave`   | Ubah ID slave                                   |
| POST   | `/api/delete_slave` | Hapus registrasi slave                          |
| GET    | `/api/register`     | Registrasi internal slave (dipanggil oleh slave)|

### Commands (POST `/api/command`)

Body berupa JSON: `{"id": 1, "cmd": "ADD_TIME", "time": 300}`

| cmd        | time     | Keterangan                                    |
| ---------- | -------- | --------------------------------------------- |
| `ADD_TIME` | 1-28800  | Menambah waktu (dalam **detik**)              |
| `PAUSE`    | 0        | Pause waktu berjalan                          |
| `RESUME`   | 0        | Lanjutkan waktu yang di-pause                 |
| `STOP`     | 0        | Reset waktu jadi 0 & lock relay               |
| `IDENTIFY` | 0        | Bunyikan buzzer slave 3x untuk mencari posisi |
| `REBOOT`   | 0        | Restart ESP slave secara software             |

> **Semua value waktu dalam satuan DETIK.** Konversi menit/jam ke detik dilakukan oleh aplikasi Android sebelum dikirim ke API.

---

## 5. Fitur Penting

1. **Powerloss Recovery**: Waktu (`remaining`) pada slave disimpan secara periodik ke dalam EEPROM (ESP8266) atau NVS (ESP32) setiap 30 detik. Jika listrik mati/baterai dicabut, slave akan resume sisa waktunya saat dinyalakan kembali.
2. **Timer Accuracy**: Logic loop non-blocking (ESP32) mencegah _drift_ pada timer dibandingkan mengandalkan `delay()`.
3. **Max Time Limit**: Slave menolak ADD_TIME melebihi 480 menit (28800 detik / 8 jam) per sekali command.
4. **Battery Field**: Slave mengirim field `battery` dengan value `"OK"` (hardcoded). Tidak ada sensor battery fisik — field ini disiapkan untuk pengembangan future.

---

## 6. Build & Test (PlatformIO)

**Build File:** `platformio.ini` memiliki environment yang tersedia: `master`, `slave`, `slave_c3`, dan `slave8266`.

### Build & Upload

```bash
# Compile semua environment
pio run

# Compile & upload individual
pio run -e master -t upload --upload-port COM_MASTER
pio run -e slave -t upload --upload-port COM_SLAVE
pio run -e slave_c3 -t upload --upload-port COM_SLAVE_C3
pio run -e slave8266 -t upload --upload-port COM_SLAVE_8266
```

### Stress Testing

Testing otomatis menggunakan `python scratch.py` (didukung opsi `--stress` untuk concurrent connection flooding, rogue testing, payload fuzzing, dan poll check).
