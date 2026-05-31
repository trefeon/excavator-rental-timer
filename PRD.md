# PRD - Excavator Rental Timer Module

## 1. Summary

Produk ini adalah module timer rental murah untuk mainan excavator remote control. Module dipasang ke mainan, memutus/menyambung power mainan memakai relay murah, dan dikontrol dari Android dashboard lewat BLE direct.

Customer tetap memakai remote RC bawaan. Staff/operator memakai Android app untuk melihat status mainan, menambah waktu per 5 menit, pause/resume, stop, dan provisioning unit baru.

## 2. Product Decision

MVP fokus ke pedagang rental mainan murah.

Keputusan MVP:

- BLE direct, tanpa Wi-Fi.
- Tanpa gateway.
- Tanpa Bluetooth Mesh.
- Android scan semua mainan via BLE advertising.
- Android connect hanya ke satu mainan saat kirim command.
- ESP32 menjadi source of truth untuk timer dan relay.
- Android menjadi dashboard, command sender, dan transaction log.
- 1x18650 shared untuk ESP32 dan mainan.
- Saat battery diganti, ESP32 boleh mati.
- Timer tidak reset karena disimpan ke ESP32 NVS.
- Setelah battery baru, module boot ke `PAUSED`, tidak auto-run.
- Relay murah 3V/3.3V dipakai sebagai power gate.
- TM1637 4-digit display ditempel di mainan untuk menampilkan sisa waktu.

## 3. Problem

Pedagang rental mainan di mall butuh cara murah untuk menyewakan banyak mainan excavator RC tanpa harus memodifikasi remote/motor control utama.

Masalah yang harus diselesaikan:

- Staff perlu mengatur waktu sewa cepat dari HP/tablet.
- Banyak mainan perlu terlihat statusnya sekaligus.
- Mainan harus mati otomatis saat waktu habis.
- Sisa waktu harus tetap aman saat battery 18650 diganti.
- Customer/staff perlu melihat sisa waktu langsung di mainan.
- Solusi harus murah, mudah dipasang, dan mudah dipahami pedagang.

## 4. Goals

- Staff bisa melihat minimal 10 mainan dari Android dashboard.
- Staff bisa menambah waktu per 5 menit.
- Mainan hidup hanya saat session aktif.
- Mainan mati otomatis saat timer habis.
- Timer survive battery hotswap.
- Sisa waktu tampil di TM1637 display pada mainan.
- Battery low mematikan mainan dan menyimpan sisa waktu.
- Command BLE tidak menggandakan waktu saat retry/double tap.
- Hardware bisa dibuat dari komponen murah dan umum.

## 5. Non-Goals

- Tidak mengganti remote RC bawaan.
- Tidak membuat joystick/control motor dari HP.
- Tidak memakai Wi-Fi untuk MVP.
- Tidak memakai cloud/server untuk MVP.
- Tidak memakai gateway untuk MVP.
- Tidak memakai Bluetooth Mesh untuk MVP.
- Tidak menampilkan persen battery presisi.
- Tidak auto-resume setelah battery baru dipasang.

## 6. Users

### Pedagang/Owner

Butuh produk murah, bisa dijual/dipasang ke banyak mainan, minim training, dan mudah diperbaiki.

### Staff Operator

Mengoperasikan rental harian. Butuh dashboard cepat, status jelas, dan tombol utama seperti `+5 menit`, `Pause`, `Resume`, `Stop`.

### Customer

Memainkan excavator memakai remote RC bawaan. Melihat sisa waktu dari display yang ditempel di mainan.

## 7. Core User Flows

### 7.1 Start Rental

```text
Staff buka Android dashboard
Dashboard scan BLE advertising semua mainan
Staff pilih EXC-01
Staff tekan +5 menit
Android connect BLE ke EXC-01
Android kirim ADD_TIME 300
ESP32 validasi command
ESP32 save session ke NVS
ESP32 ON-kan relay
TM1637 tampil MM:SS
Android terima ACK
Android disconnect
Customer pakai remote RC bawaan
```

### 7.2 Add Time While Running

```text
Staff tekan +5 menit
Android connect BLE ke mainan
Android kirim ADD_TIME 300 dengan command_id baru
ESP32 tambah remaining_seconds
ESP32 save NVS
TM1637 update sisa waktu
Android simpan transaction log
Android disconnect
```

### 7.3 Timer Finished

```text
remaining_seconds mencapai 0
ESP32 relay OFF
TM1637 tampil 0000 blink
state = ENDED
ESP32 advertise ENDED
Dashboard update status
```

### 7.4 Battery Hotswap

```text
Saat RUNNING, ESP32 save remaining_seconds tiap 5-10 detik
Staff cabut 18650
ESP32 mati
Relay default OFF
Staff pasang 18650 baru
ESP32 boot
ESP32 load remaining_seconds dari NVS
state = PAUSED
TM1637 tampil sisa waktu paused
Staff tekan Resume
Relay ON lagi jika battery OK
```

### 7.5 Low Battery

```text
ESP32 baca ADC battery
Battery critical/cutoff terdeteksi beberapa detik
ESP32 relay OFF
ESP32 save remaining_seconds
state = LOW_BATT
TM1637 tampil Lo / blink remaining
Dashboard tampil LOW_BATT
Staff ganti battery
ESP32 boot/load state
Staff tekan Resume
```

## 8. System Architecture

Diagram utama: [docs/MERMAID_DIAGRAMS.md](docs/MERMAID_DIAGRAMS.md)

```text
Android dashboard
  |
  | BLE scan + connect-on-command
  |
ESP32 module per mainan
  |
  +-> TM1637 4-digit display
  |
  +-> relay driver -> relay murah
                    |
                    +-> switched battery positive -> PCB mainan RC bawaan
```

Power:

```text
18650 holder
  +-> buck/buck-boost 3.3V -> ESP32 + TM1637
  +-> fuse/PTC -> relay contact -> PCB mainan
```

## 9. Hardware Requirements

Per unit:

| Component | Requirement |
| --- | --- |
| ESP32-C3 / ESP32 | BLE peripheral, NVS, ADC |
| 18650 holder | Staff-serviceable hotswap |
| 3.3V regulator | Buck/buck-boost, not AMS1117 |
| Relay 3V/3.3V | NO contact, 3A minimum, 5A preferred |
| Relay driver | GPIO -> resistor -> transistor/MOSFET |
| Flyback diode | Across relay coil |
| TM1637 4-digit display | Mounted on toy, shows countdown |
| Voltage divider | 220k/100k to ADC |
| Fuse/PTC | 2A-5A based on toy current |
| Capacitor | 100nF ADC, 470uF-1000uF power |
| Service button | Provision/reset mode |
| QR sticker | `toy_id`, `ble_name`, secret |

Relay rules:

- Switch battery positive, not ground.
- Use normally-open contact.
- Relay OFF during boot/reset/fault/timer end.
- GPIO must not drive coil directly.

## 10. Firmware Requirements

Firmware must:

- Start with relay OFF before BLE/timer init.
- Load saved session from NVS.
- Boot to `PAUSED` when saved `remaining_seconds > 0`.
- Boot to `LOCKED` when no active session exists.
- Never auto-run after battery swap.
- Advertise state via BLE manufacturer data.
- Expose GATT State, Command, Ack, Info characteristics.
- Process `ADD_TIME`, `PAUSE`, `RESUME`, `STOP`, `CLEAR_FAULT`, `GET_STATE`.
- Save timer on command.
- Save timer every 5-10 seconds while running.
- Save timer on low battery cutoff.
- Update TM1637 display by state.
- Reject duplicate/stale command ids.
- Validate signed commands in production.
- Keep local timer running when Android disconnects.
- Turn relay OFF when timer reaches zero.

## 11. Firmware State Machine

States:

```text
LOCKED
RUNNING
PAUSED
LOW_BATT
ENDED
FAULT
```

State behavior:

| State | Relay | Display |
| --- | --- | --- |
| `LOCKED` | OFF | `----` |
| `RUNNING` | ON | `MM:SS` |
| `PAUSED` | OFF | remaining time, paused/blink |
| `LOW_BATT` | OFF | `Lo` / blink remaining |
| `ENDED` | OFF | `0000` blink |
| `FAULT` | OFF | `Err` |

## 12. BLE Requirements

BLE mode:

```text
ESP32 = BLE peripheral / GATT server
Android = BLE central / GATT client
```

Android behavior:

- Scan all configured toys.
- Render dashboard from advertising payload.
- Connect only when command is sent.
- Disconnect after ACK.
- Retry with same `command_id` if BLE disconnects before ACK.

Advertising payload must include:

```text
version
toy_id
state
remaining_min
battery_status
fault_code
seq
flags
remaining_sec optional
```

GATT service and characteristic IDs are defined in [docs/BLE_PROTOCOL_SPEC.md](docs/BLE_PROTOCOL_SPEC.md).

## 13. Android Requirements

Android app must provide:

- Dashboard list for at least 10 toys.
- Device detail screen.
- `+5 menit`, `+10 menit`, `Pause`, `Resume`, `Stop`, `Clear Fault`.
- QR provisioning flow.
- Local transaction log.
- BLE scan status and offline detection.
- Command signing using per-device secret.
- Duplicate command safety using `command_id`.
- Clear low battery/fault messaging for staff.

Dashboard row example:

```text
EXC-01  RUNNING   03:20  OK
EXC-02  LOCKED    --     OK
EXC-03  PAUSED    02:40  LOW
EXC-04  OFFLINE   --     --
```

## 14. Security Requirements

Provisioning:

```text
QR = toy_id + ble_name + per-device secret
```

Production command signature:

```text
signature = HMAC_SHA256(secret, toy_id|command_id|command|value|session_id|nonce)
```

ESP32 must:

- Reject invalid signature.
- Reject stale `command_id`.
- Treat repeated same `command_id` as duplicate, not a new paid time.
- Store `last_command_id`.
- Keep debug unsigned commands disabled in production.

## 15. Battery Requirements

Battery display is status-based, not percentage-based.

Statuses:

```text
OK
LOW
CRITICAL
```

Initial 1x18650 thresholds:

| Status | Voltage |
| --- | --- |
| `OK` | `> 3.55V` |
| `LOW` | `3.30V - 3.55V` |
| `CRITICAL` | `< 3.30V` |
| cutoff | `< 3.15V for 5-10 seconds` |

When cutoff triggers:

- Relay OFF.
- State `LOW_BATT`.
- Save `remaining_seconds`.
- Advertise low battery.
- Display `Lo` or blink remaining.

## 16. Data Requirements

ESP32 NVS:

```text
state
remaining_seconds
total_paid_seconds
session_id
last_command_id
crc
```

Android local storage:

```text
toy_id
ble_name
secret
last_seen_state
last_seen_remaining
last_seen_battery
last_seen_at
transaction_log
```

Transaction log:

```text
timestamp
toy_id
session_id
command_id
command
value
ack_code
remaining_after
operator_id optional
```

## 17. Acceptance Criteria

MVP is accepted when:

- Android sees at least 10 toys via BLE advertising.
- Android can send command to one toy without keeping connections to other toys.
- `+5 menit` adds 300 seconds.
- `+10 menit` adds 600 seconds.
- `ADD_TIME` starts a locked toy if battery is OK.
- Timer continues after Android disconnect.
- Relay turns OFF when timer ends.
- TM1637 shows remaining time on toy.
- Battery removal does not reset session permanently.
- After battery replacement, toy boots into `PAUSED`, not `RUNNING`.
- `Resume` restarts relay only when battery is OK.
- Low battery cutoff turns relay OFF and saves remaining time.
- Duplicate command id does not add time twice.
- Production command signature rejects unauthorized command.
- Staff can provision toy by scanning QR.

## 18. MVP Milestones

### M1 - Single Toy Bench Prototype

- ESP32 + relay + TM1637 wired.
- Serial/debug command can add time.
- Relay toggles safely.
- NVS survives battery removal.

### M2 - BLE Command Prototype

- Android/BLE client can scan toy.
- `ADD_TIME`, `PAUSE`, `RESUME`, `STOP` work.
- Ack and State characteristics work.

### M3 - 10 Toy Dashboard

- Dashboard shows 10 advertising devices.
- Connect-on-command works reliably.
- Offline/weak status works.
- Transaction log works.

### M4 - Field Pilot

- 10 toys installed in mall booth.
- Battery swap SOP tested.
- Staff can operate without developer help.
- Fault/low-battery cases tested.

## 19. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Relay 5V not reliable on 1x18650 | Toy fails to switch | Use 3V/3.3V relay or proper relay module |
| Motor current spike resets ESP32 | Timer/display glitch | Use proper regulator and capacitors |
| Battery removed before last save | Remaining time may roll back few seconds | Save every 5-10 seconds; use FRAM later if needed |
| BLE scan varies by Android model | Dashboard delays | Use advertising cache and connect-on-command retry |
| QR secret leaks | Unauthorized command risk | Rotate secret via service mode |
| Staff expects auto-resume | Safety issue | Explicit manual Resume after battery swap |

## 20. Open Questions

- Final ESP32 board target: ESP32-C3 module or ESP32 DevKit?
- Exact relay part available locally?
- Mainan uses 1x18650 directly or internal voltage conversion?
- Needed enclosure shape and mounting method?
- App framework: native Android, Flutter, React Native, or Kotlin Multiplatform?
- Production storage: NVS only or FRAM upgrade?

## 21. Reference Docs

- [MVP_SPEC.md](MVP_SPEC.md)
- [docs/MERMAID_DIAGRAMS.md](docs/MERMAID_DIAGRAMS.md)
- [docs/RELAY_WIRING.md](docs/RELAY_WIRING.md)
- [docs/BLE_PROTOCOL_SPEC.md](docs/BLE_PROTOCOL_SPEC.md)
- [docs/ANDROID_APP_FLOW.md](docs/ANDROID_APP_FLOW.md)
- [firmware/esp32_ble_direct_mvp/README.md](firmware/esp32_ble_direct_mvp/README.md)

