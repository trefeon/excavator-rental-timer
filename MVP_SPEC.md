# MVP Spec - Excavator Rental Timer BLE Direct

## 1. Product Decision

MVP ini untuk pedagang rental mainan excavator RC murah. Mainan tetap memakai remote RC bawaan. Module hanya menghidupkan atau mematikan power mainan berdasarkan timer sewa.

Keputusan final MVP:

- Android dashboard langsung scan dan command tiap mainan lewat BLE.
- Tidak pakai gateway.
- Tidak pakai Wi-Fi.
- Tidak pakai Bluetooth Mesh.
- ESP32 menyimpan timer sendiri.
- Android hanya dashboard, command sender, dan log transaksi.
- Battery 18650 dipakai bersama untuk ESP32 dan mainan.
- Saat battery diganti, ESP32 boleh mati.
- Setelah battery baru dipasang, mainan tidak auto-run; state menjadi `PAUSED`.
- Relay murah dipakai untuk memutus power ke mainan.
- TM1637 4-digit display ditempel di mainan untuk menampilkan sisa waktu.

## 2. Target User

- Pedagang/operator rental di mall.
- Staff non-teknis harus bisa start, tambah waktu, pause, resume, dan ganti battery.
- Customer memakai remote RC bawaan, tidak memakai app.

## 3. System Overview

Primary diagrams:

- [Mermaid diagrams](docs/MERMAID_DIAGRAMS.md)
- [Relay wiring notes](docs/RELAY_WIRING.md)

```text
Android dashboard
  |
  | BLE scan + connect-on-command
  |
ESP32 module per mainan
  |
  +-> TM1637 4-digit display mounted on toy
  |
  | GPIO relay control
  |
Relay murah
  |
  | switched battery positive
  |
PCB mainan excavator RC bawaan
```

Power:

```text
18650 holder
  +-> buck/buck-boost 3.3V -> ESP32 always-on
  +-> fuse/PTC -> relay contact -> PCB mainan
```

## 4. Hardware MVP

Per mainan:

| Item | Catatan |
| --- | --- |
| ESP32-C3 / ESP32 DevKit | ESP32-C3 lebih murah/hemat jika tersedia |
| 1x 18650 holder | Holder kuat, battery mudah diganti staff |
| 3.3V regulator | Buck/buck-boost, bukan AMS1117 |
| Relay 3V/3.3V | Contact minimal 3A, ideal 5A |
| Transistor driver relay | NPN 2N2222/S8050 atau MOSFET kecil |
| Diode flyback | 1N4148/1N4007 di coil relay |
| TM1637 4-digit display | Countdown `MM:SS`, ditempel di mainan |
| Fuse/PTC | 2A-5A sesuai arus mainan |
| Voltage divider | 220k + 100k ke ADC |
| Capacitor | 100nF ADC, 470uF-1000uF dekat beban |
| LED status | Debug sederhana |
| Button service | Provision/reset manual |
| QR sticker | Pairing app dengan toy_id + secret |

## 5. Relay Rule

Relay hanya memutus jalur positive battery ke PCB mainan.

```text
Battery + -> fuse/PTC -> relay COM
relay NO -> PCB mainan +
Battery - -> PCB mainan -
```

Relay coil tidak boleh langsung dari GPIO.

```text
GPIO -> resistor 1k -> transistor base/gate
transistor -> coil relay
diode flyback across coil
```

Default safety:

- Relay OFF saat boot.
- Relay OFF saat ESP reset.
- Relay OFF saat timer habis.
- Relay OFF saat low battery.
- Relay OFF saat fault.

## 6. Timer Ownership

ESP32 adalah source of truth untuk timer dan relay.

Android:

- Scan state.
- Kirim command.
- Simpan log transaksi.
- Bisa restore manual jika storage ESP rusak.

ESP32:

- Menyimpan `remaining_seconds`.
- Menjalankan countdown lokal.
- Mematikan relay saat waktu habis.
- Tetap aman walau Android disconnect.

## 7. Hotswap Battery

Battery 18650 bisa diganti staff. Karena ESP dan mainan satu power source, ESP akan mati saat battery dicabut.

Flow:

```text
RUNNING
  -> ESP save remaining_seconds tiap 5-10 detik
  -> battery dicabut
  -> ESP mati
  -> relay OFF
  -> battery baru masuk
  -> ESP boot
  -> load remaining_seconds
  -> state = PAUSED
  -> staff tekan Resume
  -> relay ON lagi
```

Maksimal waktu yang "balik" tergantung save interval:

```text
save tiap 10 detik -> selisih maksimal sekitar 10 detik
save tiap 5 detik  -> selisih maksimal sekitar 5 detik
```

Jangan auto-resume setelah battery baru karena mainan bisa hidup saat staff memegang battery/casing.

## 8. State Machine

```text
LOCKED
RUNNING
PAUSED
LOW_BATT
ENDED
FAULT
```

State behavior:

| State | Relay | Meaning |
| --- | --- | --- |
| `LOCKED` | OFF | Tidak ada sesi aktif |
| `RUNNING` | ON | Timer berjalan |
| `PAUSED` | OFF | Ada sisa waktu, menunggu resume |
| `LOW_BATT` | OFF | Battery harus diganti |
| `ENDED` | OFF | Waktu habis |
| `FAULT` | OFF | Butuh clear dari staff |

Display behavior:

| State | Display |
| --- | --- |
| `LOCKED` | `----` |
| `RUNNING` | `MM:SS` with colon |
| `PAUSED` | `MMSS` or blinking colon |
| `LOW_BATT` | `Lo` / blink remaining |
| `ENDED` | `0000` blink |
| `FAULT` | `Err` |

Boot rule:

```text
if saved remaining_seconds > 0:
    state = PAUSED
else:
    state = LOCKED
relay = OFF
```

## 9. Android Dashboard Scope

MVP screens:

- Device list 10 mainan.
- Device detail.
- QR provisioning.
- Transaction log simple.

Main action:

- `+5 menit`
- `+10 menit`
- `Pause`
- `Resume`
- `Stop`
- `Clear Fault`

List display:

```text
EXC-01  RUNNING   03m   OK
EXC-02  LOCKED    --    OK
EXC-03  PAUSED    02m   LOW
EXC-04  OFFLINE   --    --
```

## 10. BLE Direct Scope

BLE model:

```text
ESP32 = BLE peripheral
Android = BLE central
```

Android behavior:

- Scan BLE advertising terus saat dashboard aktif.
- Tidak connect ke semua mainan terus menerus.
- Connect hanya saat kirim command.
- Setelah ACK, disconnect.

ESP32 advertising payload:

```text
toy_id
state
remaining_minutes
battery_status
fault_code
seq
```

## 11. Command Scope

Command MVP:

```text
ADD_TIME 300
PAUSE
RESUME
STOP
CLEAR_FAULT
GET_STATE
```

Rule `ADD_TIME`:

- Increment wajib kelipatan 300 detik.
- Jika state `LOCKED`, `ADD_TIME 300` membuat sesi baru dan langsung `RUNNING`.
- Jika state `RUNNING`, waktu bertambah.
- Jika state `PAUSED`, waktu bertambah tapi relay tetap OFF sampai `RESUME`.
- Jika state `LOW_BATT`, waktu boleh bertambah tapi relay tetap OFF sampai battery OK dan `RESUME`.

## 12. Security MVP

Provisioning:

```text
QR = toy_id + BLE name + per-device secret
```

Command signed:

```text
payload = toy_id + command_id + command + value + session_id + nonce
signature = HMAC_SHA256(secret, payload)
```

ESP validates:

- Signature valid.
- `command_id` lebih besar dari `last_command_id`.
- Command valid untuk state sekarang.

Debug build boleh mengaktifkan unsigned command, tapi produksi harus signed.

## 13. Battery Detection MVP

Tidak perlu persen battery. Pakai 3 status:

```text
OK
LOW
CRITICAL
```

Threshold 1x18650 awal:

| Status | Voltage |
| --- | --- |
| `OK` | `> 3.55V` |
| `LOW` | `3.30V - 3.55V` |
| `CRITICAL` | `< 3.30V` |
| cutoff | `< 3.15V selama 5-10 detik` |

Saat cutoff:

```text
relay OFF
state = LOW_BATT
save remaining_seconds
advertise LOW_BATT
```

## 14. Acceptance Criteria

MVP selesai jika:

- Android bisa melihat 10 mainan via BLE advertising.
- Android bisa command 1 mainan tanpa connect ke 9 lainnya.
- `+5 menit` menambah timer dan menyalakan relay jika battery OK.
- Timer tetap berjalan walau Android disconnect.
- TM1637 display menampilkan sisa waktu di mainan.
- Relay mati saat timer habis.
- Timer tidak reset setelah battery dicabut/diganti.
- Setelah battery baru, state menjadi `PAUSED`, bukan auto-run.
- Low battery mematikan relay dan menyimpan sisa waktu.
- Command duplikat tidak menambah waktu dua kali.
- Storage survive power loss.
