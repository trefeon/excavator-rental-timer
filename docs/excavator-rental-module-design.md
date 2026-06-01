# Dokumen Konsep Modul Timer IoT Mainan Excavator Rental

## 1. Ringkasan

Project ini membuat modul murah untuk dipasang ke mainan excavator remote control. Mainan tetap memakai remote RC bawaan. Modul hanya mengatur akses power mainan berdasarkan timer sewa yang dikontrol dari dashboard Web atau Android lewat Wi-Fi.

Target utama:
- Banyak unit mainan bisa dipantau dari 1 dashboard (Web/Android).
- Mainan bisa ditambah waktu per 5 menit.
- Sisa waktu terlihat langsung di mainan lewat TM1637 4-digit display.
- Timer tidak reset saat battery 18650 diganti (NVS storage).
- Modul murah, gampang dirakit, dan reliable untuk operasional mall.
- Arsitektur Wi-Fi Master-Slave mandiri (tanpa internet, tanpa cloud).

## 2. Asumsi Hardware Mainan

Mainan excavator murah biasanya punya pola power:
```
Battery -> PCB receiver/controller bawaan -> motor track/arm/bucket/turret
Remote RC bawaan -> PCB receiver bawaan
```

Modul tidak mengganti PCB atau remote bawaan. Modul hanya memutus dan menyambung power ke PCB mainan via relay.

## 3. Arsitektur Power

```
18650 holder
  |
  +-> regulator 3.3V always-on -> ESP32 + TM1637 display + buzzer
  |
  +-> fuse/PTC -> relay kontak NO -> PCB mainan excavator
```

Prinsip:
- ESP32 selalu dapat power selama 18650 terpasang (timer tetap jalan).
- Mainan hanya dapat power saat relay ON (timer aktif).
- Saat ESP32 reset atau mati, relay default OFF.
- Putus jalur battery positive ke mainan, bukan ground.

## 4. Relay Murah Sebagai Keputusan MVP

Target jual ke pedagang: relay murah karena lebih mudah dijelaskan, dirakit, dan diservis.

Kelebihan:
- Mudah dimengerti pedagang.
- Mudah dirakit.
- Cocok untuk prototype dan produksi skala kecil.
- Komponen familiar dan tersedia di toko elektronik.

Kekurangan:
- Koil makan arus (~70mA untuk relay 3.3V).
- Umur mekanik terbatas (~100.000 siklus).
- Bisa bunyi klik.
- Perlu diode flyback.

Rekomendasi:
- Pakai relay koil 3V/3.3V, bukan relay 5V.
- Contact rating minimal 3A, lebih aman 5A.
- Relay harus OFF saat ESP32 boot/reset.
- Pakai modul relay siap pakai (sudah ada transistor driver + diode).

## 5. Battery 18650 Hotswap

Battery 18650 memakai holder dan bisa diganti staff.

Masalah: kalau 18650 dicabut, ESP32 ikut mati karena power source jadi satu.

Solusi:
- ESP32 menyimpan timer berkala ke NVS Flash (setiap 30 detik + setiap state change).
- Setelah battery baru dipasang, ESP32 boot ulang.
- ESP32 membaca timer terakhir dari NVS.
- Bunyi peringatan 3x dengan jeda 3 detik (staff menjauhkan tangan).
- Auto-resume ke RUNNING.

Flow:
```
RUNNING
  -> battery dicabut
  -> ESP32 mati, mainan mati
  -> battery baru dipasang
  -> ESP32 boot, connect ke Master
  -> load remaining_seconds dari NVS
  -> bunyi peringatan 3x (3 detik)
  -> auto-resume ke RUNNING
  -> relay ON, mainan hidup lagi
```

Catatan: ada jeda peringatan 3 detik sebelum auto-resume untuk safety. Jika ingin mode PAUSED (tidak auto-resume), ubah state menjadi `STATE_PAUSED` di kode.

## 6. Penyimpanan Timer (NVS)

Data yang disimpan di ESP32 Slave:
```
remaining_seconds
total_paid_seconds
```

Menggunakan NVS ESP32 (Preferences API):
- Gratis, sudah built-in.
- Data persistent saat power mati.
- Write dilakukan tiap 30 detik dan setiap perubahan state.
- Maksimal selisih waktu: ~30 detik (interval save).

Untuk produksi jangka panjang: bisa upgrade ke FRAM I2C (write unlimited, save tiap detik).

## 7. State Machine

```
LOCKED → RUNNING → PAUSED → RUNNING
  ↑         ↓         ↓
  └─── ENDED ◄────────┘
```

| State | Relay | Display | Trigger |
|-------|-------|---------|---------|
| LOCKED | OFF | `----` | Default, STOP |
| RUNNING | ON | `MM:SS` kedip | ADD_TIME, RESUME, powerloss recovery |
| PAUSED | OFF | `MM:SS` kedip | PAUSE |
| ENDED | OFF | `----` | Timer habis (rem=0) |
| FAULT | OFF | `----` | Error (butuh restart) |

Boot logic:
```
boot:
  relay OFF
  load NVS
  if remaining_seconds > 0:
      bunyi peringatan 3x
      state = RUNNING (auto-resume)
  else:
      state = LOCKED
```

Timer logic:
```
if state == RUNNING:
    remaining_seconds--
    if remaining_seconds == 60: beep 2x (warning 1 menit)
    if remaining_seconds <= 10: beep 1x/detik (countdown)
    if remaining_seconds == 0:
        relay OFF
        state = ENDED
        beep panjang 1x
        save NVS
```

## 8. Arsitektur Wi-Fi Master-Slave

```
Android / Browser ──HTTP──► Master ESP32 (AP + API Gateway)
                               │
                    ┌──────────┼──────────┐
                    ▼          ▼          ▼
               Slave 1    Slave 2    Slave N
              (ESP32)    (ESP32)    (ESP32)
```

**Master:**
- Access Point `ExcavatorMaster` (192.168.4.1).
- DHCP server (IP dinamis untuk slave).
- Web server + Dashboard WebUI.
- API Gateway (proxy command ke slave).
- Registry MAC→ID di NVS.
- Core 0: Background task polling state semua slave.
- Core 1: Web server + API handler.
- Mutex: thread safety antara Core 0 dan Core 1.
- Hardware watchdog 10 detik.

**Slave:**
- WiFi Station, connect ke Master.
- Zero-Touch Provisioning (auto register).
- Web server lokal (menerima command dari Master).
- Timer countdown independen.
- Relay control (power gate).
- TM1637 display.
- Buzzer feedback.
- Button fisik resume.
- NVS storage.
- WiFi event handler (auto-reconnect).
- Hardware watchdog 10 detik.

## 9. Keamanan

- Command hanya via jaringan Wi-Fi AP Master (terisolasi dari internet).
- MAC address sebagai identitas unik hardware.
- Tidak ada enkripsi data (MVP) — network terisolasi.
- Untuk produksi selanjutnya: tambahkan PIN/password di API Master.

## 10. Error Handling

Default aman:
- Relay OFF saat boot/reset.
- Relay OFF saat timer habis.
- Relay OFF saat fault.

Error handling:
- Brownout/reset: boot ke RUNNING dengan peringatan 3 detik (jika ada sisa waktu).
- Command invalid: ignore, return error code.
- Storage corrupt: masuk FAULT, butuh restart.
- Timer habis: relay OFF, state ENDED.
- WiFi disconnect: server tetap berjalan, timer tetap berjalan lokal.
- Master mati: slave tetap operasikan timer dan relay secara mandiri.

## 11. MVP BOM (per mainan)

```
ESP32 DevKit V1
18650 holder
3.3V buck converter
Relay module 3.3V 1-channel
Transistor driver relay (built-in di modul)
Diode flyback (built-in di modul)
TM1637 4-digit display
Buzzer aktif
Button taktil
Fuse/PTC 2A-5A
Kapasitor 470uF-1000uF (dekat beban)
Kabel jumper
Casing/kotak
```

## 12. Ruang Lingkup Firmware MVP

- WiFi Station + AP mode.
- Zero-Touch Provisioning (auto register ke Master).
- Web server lokal (menerima API command).
- Timer countdown lokal.
- NVS storage (powerloss recovery).
- Relay control.
- TM1637 display driver.
- Buzzer feedback.
- Button fisik.
- State machine.
- WiFi auto-reconnect.
- Hardware watchdog.
- Thread-safe registry (Master, dual-core mutex).

## 13. Keputusan Desain Final

```
Mainan tetap pakai remote RC bawaan.
Modul hanya gate power mainan dengan relay murah.
Battery 18650 shared untuk mainan dan ESP32.
Saat battery diganti, ESP32 boleh mati — timer survive via NVS.
Setelah battery baru, auto-resume dengan peringatan 3 detik.
Wi-Fi Master-Slave untuk report state dan command proxy.
Android/Browser hanya connect ke 1 IP Master (192.168.4.1).
Waktu ditambah per 5 menit dari dashboard.
TM1637 4-digit display ditempel di mainan.
Buzzer + button fisik untuk feedback dan kontrol manual.
Mutex dual-core + hardware watchdog untuk reliability.
```

Desain ini paling murah, mudah dirakit, dan cukup aman untuk MVP rental mainan excavator.
