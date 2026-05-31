# Dokumen Konsep Module Timer IoT Mainan Excavator Rental

## 1. Ringkasan

Project ini membuat module murah untuk dipasang ke mainan excavator remote control murah. Mainan tetap memakai remote RC bawaan. Module hanya mengatur akses power mainan berdasarkan timer sewa yang dikontrol dari aplikasi Android lewat BLE.

Target utama:

- Banyak unit mainan bisa dipantau dari 1 dashboard Android.
- Mainan bisa ditambah waktu per 5 menit.
- Sisa waktu terlihat langsung di mainan lewat TM1637 4-digit display.
- Timer tidak reset saat battery 18650 diganti.
- Module murah, gampang dirakit, dan cukup reliable untuk operasional mall.
- Tidak memakai Wi-Fi agar HP/tablet owner tetap bebas pakai internet seluler.

## 2. Asumsi Hardware Mainan

Mainan excavator murah biasanya punya pola power seperti ini:

```text
Battery -> PCB receiver/controller bawaan -> motor track/arm/bucket/turret
Remote RC bawaan -> PCB receiver bawaan
```

Module tidak mengganti PCB atau remote bawaan. Module hanya memutus dan menyambung power ke PCB mainan.

## 3. Arsitektur Power

Desain power:

```text
18650 holder
  |
  +-> regulator 3.3V always-on -> ESP32 + TM1637 display
  |
  +-> fuse/PTC -> relay murah -> PCB mainan excavator
```

Prinsip:

- ESP32 selalu mendapat power selama 18650 terpasang.
- Mainan hanya mendapat power saat timer aktif.
- Saat ESP32 reset atau mati, switch harus default OFF.
- Putus jalur battery positive ke mainan, bukan ground.

## 4. Relay Murah Sebagai Keputusan MVP

Untuk target jual ke pedagang, MVP memakai relay murah karena lebih mudah dijelaskan, mudah dirakit, dan mudah diservis.

Kelebihan:

- Mudah dimengerti.
- Mudah dirakit.
- Cocok untuk prototype.
- Cocok untuk dijual ke pedagang karena komponen familiar.

Kekurangan:

- Coil makan arus kalau bukan latching relay.
- Umur mekanik terbatas.
- Bisa bunyi klik.
- Perlu diode flyback.
- Relay 5V umum tidak reliable jika langsung disupply dari 1x18650.

Rekomendasi relay:

```text
Pakai relay 3V/3.3V coil, bukan relay 5V biasa.
Contact rating minimal 3A, lebih aman 5A.
Relay harus OFF saat ESP32 boot/reset.
Relay coil wajib pakai transistor driver + diode flyback.
```

Catatan:

- Kalau hanya ada relay module 5V murah, perlu boost 5V khusus relay.
- Boost 5V menambah biaya dan boros, jadi relay 3V lebih cocok.
- MOSFET/load switch tetap opsi upgrade, bukan target MVP pedagang.

## 5. Battery 18650 Hotswap

Battery 18650 memakai holder dan bisa diganti oleh staff.

Masalah utama:

```text
Kalau 18650 dicabut, ESP32 ikut mati karena power source jadi satu.
```

Solusi:

- ESP32 menyimpan timer berkala ke flash internal atau FRAM.
- Setelah battery baru dipasang, ESP32 boot ulang.
- ESP32 membaca timer terakhir.
- Mainan masuk state PAUSED, tidak langsung hidup otomatis.
- Staff tekan Resume dari dashboard.

Flow:

```text
RUNNING
  -> battery dicabut
  -> ESP32 mati
  -> mainan mati
  -> battery baru dipasang
  -> ESP32 boot
  -> load remaining_seconds
  -> state = PAUSED
  -> staff tekan Resume
  -> mainan hidup lagi
```

Catatan safety:

- Jangan auto-resume setelah battery baru dipasang.
- Boot selalu relay OFF.
- Resume manual mencegah mainan tiba-tiba hidup saat staff mengganti battery.

## 6. Penyimpanan Timer

Data minimal yang disimpan di ESP32:

```text
toy_id
session_id
state
remaining_seconds
total_paid_seconds
last_command_id
crc
```

Storage pilihan:

### NVS ESP32

Kelebihan:

- Gratis, sudah ada di ESP32.
- Cukup untuk MVP.
- Data tetap ada saat power mati.

Kekurangan:

- Jangan write terlalu sering.
- Save tiap 5-15 detik cukup.

### FRAM I2C

Kelebihan:

- Aman write sering.
- Bisa save tiap detik.
- Lebih tahan untuk produk rental jangka panjang.

Kekurangan:

- Tambah biaya komponen.

Rekomendasi:

```text
MVP murah: NVS, save tiap 10 detik.
Versi produksi: FRAM, save tiap 1 detik.
```

Save dilakukan saat:

```text
START
ADD_TIME
PAUSE
RESUME
STOP
LOW_BATTERY
setiap 5-10 detik saat RUNNING
```

Contoh kehilangan waktu terburuk:

```text
Save interval 10 detik
remaining 04:40 tersimpan
battery dicabut saat 04:36
battery baru -> remaining balik ke 04:40
```

Timer tidak reset. Selisih maksimal sekitar interval save.

## 7. State Machine

State module:

```text
BOOT
LOCKED
RUNNING
PAUSED
PAUSED_LOW_BATT
ENDED
FAULT
```

Aturan:

- `BOOT`: relay OFF.
- `LOCKED`: tidak ada session aktif.
- `RUNNING`: relay ON, timer berkurang.
- `PAUSED`: relay OFF, remaining_seconds tetap.
- `PAUSED_LOW_BATT`: battery rendah, butuh ganti battery.
- `ENDED`: timer habis.
- `FAULT`: error butuh staff clear.

Boot logic:

```text
boot:
  switch_mainan_off()
  session = load_storage()

  if session.valid && session.remaining_seconds > 0:
      state = PAUSED
  else:
      state = LOCKED
```

Timer logic:

```text
if state == RUNNING:
    remaining_seconds--
    if remaining_seconds <= 0:
        switch_mainan_off()
        state = ENDED
        save_storage()
```

## 8. Battery Detection

Target murah: tidak perlu persen battery. Cukup status:

```text
OK
LOW
CRITICAL
```

Rangkaian termurah:

```text
Battery + ---- R1 ----+---- ADC ESP32
                      |
                     R2
                      |
GND ------------------+
```

Nilai awal untuk 1x18650:

```text
R1 = 220k
R2 = 100k
C  = 100nF dari ADC ke GND
```

Threshold awal:

```text
OK:       > 3.55V
LOW:      3.30V - 3.55V
CRITICAL: < 3.30V
CUTOFF:   < 3.15V selama 5-10 detik
```

Saat low battery:

```text
matikan mainan
save remaining_seconds
state = PAUSED_LOW_BATT
dashboard tampil "ganti battery"
```

Catatan:

- Jangan tampil persen karena voltage 18650 tidak linear.
- Baca ADC beberapa kali dan ambil average/median.
- Abaikan drop singkat saat motor start.

## 9. BLE Model Untuk 10 Mainan

Setiap mainan menjadi BLE peripheral.

Android menjadi BLE central.

Model koneksi:

```text
Android scan banyak mainan
Android connect hanya saat kirim command
Setelah ACK, Android disconnect
```

Jangan maintain 10 koneksi aktif karena:

- Batas koneksi Android beda tiap device.
- BLE stack bisa drop.
- Timer sudah jalan lokal di ESP32.
- Dashboard cukup dapat status dari advertising.

## 10. BLE Advertising State

Setiap mainan broadcast status tanpa koneksi:

```text
toy_id
state
remaining_minutes
battery_status
fault_code
seq
```

Contoh dashboard:

```text
EXC-01 RUNNING 03m OK
EXC-02 LOCKED  -- OK
EXC-03 PAUSED  02m LOW
EXC-04 FAULT   -- CRITICAL
```

Update rate:

```text
LOCKED:  1-2 detik
RUNNING: 0.5-1 detik
LOW/FAULT: 0.3-0.5 detik
```

Offline detection di Android:

```text
last_seen < 5 detik   -> ONLINE
last_seen 5-15 detik  -> WEAK
last_seen > 15 detik  -> OFFLINE
```

## 11. BLE Command Flow

Command hanya dikirim saat Android connect.

Command utama:

```text
START
ADD_TIME
PAUSE
RESUME
STOP
LOCK
CLEAR_FAULT
GET_STATE
```

Tambah waktu:

```text
ADD_TIME 300
```

Aturan:

- Increment waktu = 300 detik atau 5 menit.
- Jika state LOCKED, `ADD_TIME 300` bisa langsung membuat session dan start.
- Jika state RUNNING, remaining_seconds bertambah.
- Jika state LOW_BATT, waktu boleh ditambah, tapi mainan tidak start sampai battery diganti.

Flow `+5 menit`:

```text
staff tekan +5 menit
Android connect EXC-01
Android write ADD_TIME 300
ESP32 validasi command
ESP32 tambah remaining_seconds
ESP32 save storage
ESP32 kirim ACK
Android simpan log
Android disconnect
```

## 12. Security Pairing

Customer tidak pairing. Hanya owner/staff app.

Provisioning awal:

```text
QR di mainan:
toy_id=EXC-01
ble_name=EXC-01
secret=random_16_or_32_bytes
```

Owner app scan QR dan menyimpan secret.

Command tidak boleh hanya `unlock=true`. Command harus signed:

```text
payload = toy_id + command + value + session_id + counter + nonce
signature = HMAC_SHA256(secret, payload)
```

ESP32 validasi:

```text
signature valid
counter > last_counter
command allowed by state
```

Anti double tap:

```text
last_command_id disimpan
command_id sama -> balas ACK lama, jangan execute ulang
command_id baru -> execute
```

Pairing/reset mode:

```text
tekan tombol kecil 5 detik
LED blink
provisioning aktif 60 detik
app tulis secret baru
ESP32 save secret
keluar provisioning
```

## 13. Android Dashboard

Tampilan list:

```text
EXC-01 RUNNING 03m OK
EXC-02 LOCKED  -- OK
EXC-03 PAUSED  02m LOW
EXC-04 OFFLINE -- --
```

Detail mainan:

```text
Toy: EXC-01
Status: RUNNING
Remaining: 03:20
Battery: OK

[+5 menit] [+10 menit]
[Pause] [Resume] [Stop]
[Clear Fault]
```

Data Android:

```text
toy_id
session_id
paid_seconds
last_seen_remaining
last_seen_state
last_command_id
updated_at
transaction_log
```

Peran Android:

- Dashboard.
- Command sender.
- Backup log.
- Restore manual jika storage ESP rusak.

Peran ESP32:

- Source of truth timer.
- Source of truth relay.
- Tetap mematikan mainan saat timer habis walau Android disconnect.

## 14. Error Handling

Default aman:

```text
relay OFF saat boot/reset
```

Error handling wajib:

- Brownout/reset: boot ke PAUSED atau LOCKED, bukan auto-run.
- Low battery: matikan mainan dan simpan timer.
- Command invalid: ignore dan increment invalid counter.
- Terlalu banyak command invalid: lock command 30 detik.
- Storage CRC invalid: masuk FAULT, butuh restore dari Android.
- Timer habis: matikan mainan dan state ENDED.
- BLE disconnect: timer tetap jalan lokal.

## 15. MVP BOM

Komponen minimal per mainan:

```text
ESP32-C3 module atau ESP32 module
18650 holder
3.3V buck/buck-boost regulator
relay murah 3V/3.3V dengan contact rating 3A-5A
transistor driver relay
diode flyback relay
fuse/PTC 2A-5A
voltage divider: 220k + 100k
capacitor ADC: 100nF
capacitor power mainan: 470uF - 1000uF
capacitor regulator ESP: 100uF - 470uF
tombol provisioning/reset
LED status
casing kecil
QR sticker
```

Optional produksi:

```text
FRAM I2C
supercap 1F-5F untuk save saat power drop
MOSFET high-side/load switch sebagai upgrade
latching relay sebagai upgrade hemat daya
reverse polarity protection
TVS diode
```

## 16. MVP Firmware Scope

Firmware minimum:

- BLE advertising status.
- BLE GATT command write.
- HMAC command validation.
- Timer local.
- NVS storage.
- Relay control.
- ADC battery detection.
- State machine.
- Command ACK.
- Provisioning mode.

## 17. MVP Android Scope

App minimum:

- Scan BLE advertising semua mainan.
- List dashboard 10 mainan.
- Detail mainan.
- Tombol `+5 menit`.
- Tombol `Pause`, `Resume`, `Stop`.
- QR provisioning.
- Local transaction log.
- Warning offline/low battery/fault.

## 18. Roadmap

### Prototype 1

- 1 mainan.
- Relay switch.
- NVS save tiap 10 detik.
- Android command manual.
- Test hotswap battery.

### Prototype 2

- 3 mainan.
- Dashboard scan banyak unit.
- Advertising state.
- `+5 menit`, pause, resume.
- Battery status OK/LOW/CRITICAL.

### Pilot Mall

- 10 mainan.
- QR provisioning.
- Staff operation flow.
- Casing rapi.
- Battery swap SOP.
- Log transaksi.

### Produksi Murah Untuk Pedagang

- PCB custom.
- Relay 3V/3.3V tetap dipakai jika target biaya paling rendah.
- FRAM.
- Better enclosure.
- Gateway optional jika unit > 20.

## 19. Keputusan Desain Saat Ini

Keputusan utama:

```text
Mainan tetap pakai remote RC bawaan.
Module hanya gate power mainan dengan relay murah.
Battery 18650 shared untuk mainan dan ESP32.
Saat battery diganti, ESP32 boleh mati.
Timer disimpan di ESP32.
Setelah battery baru, state PAUSED dan staff tekan Resume.
BLE advertising dipakai untuk report state 10 mainan.
Android connect hanya saat command.
Waktu ditambah per 5 menit dari dashboard.
TM1637 4-digit display ditempel di mainan untuk menampilkan sisa waktu.
```

Desain ini paling murah, mudah dirakit, dan cukup aman untuk MVP rental mainan excavator.
