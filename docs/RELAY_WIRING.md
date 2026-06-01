# Wiring Diagram — Relay Murah MVP

Diagram utama ada di [MERMAID_DIAGRAMS.md](MERMAID_DIAGRAMS.md).

## 1. Skema Power

```
18650 holder
  +-> 3.3V buck/buck-boost regulator -> ESP32 Slave (always-on)
  +-> fuse/PTC 2A-5A -> relay NO contact -> PCB mainan +
  
18650 holder (-) -> PCB mainan -
                 -> ESP32 GND
                 -> regulator GND
```

ESP32 selalu mendapat power selama 18650 terpasang. Mainan hanya dapat power saat relay ON.

## 2. Wiring Kontak Relay

Gunakan kontak Normally-Open (NO) supaya mainan OFF secara default.

```
Battery + -> fuse/PTC -> relay COM
relay NO -> PCB mainan +
Battery - -> PCB mainan -
```

**Jangan switch ground.** Hanya putus jalur positive.

## 3. Driver Koil Relay

GPIO **tidak boleh** langsung menggerakkan koil relay. Gunakan transistor driver + diode flyback.

```
ESP32 GPIO26 ---- 1k resistor ---- transistor base/gate

3.3V ---- relay coil ---- transistor collector/drain
transistor emitter/source ---- GND

diode flyback across relay coil:
diode cathode -> 3.3V side
diode anode   -> transistor side
```

Kebanyakan **modul relay siap pakai** sudah memiliki transistor driver dan diode flyback built-in. Cek spesifikasi modul.

## 4. Pinout Resmi

| Fungsi | GPIO ESP32 | Catatan |
|--------|-----------|---------|
| Relay control | GPIO 26 | Output, default OFF |
| TM1637 CLK | GPIO 22 | 4-digit display clock |
| TM1637 DIO | GPIO 23 | 4-digit display data |
| Buzzer (+) | GPIO 27 | Buzzer aktif |
| Button Resume | GPIO 32 | INPUT_PULLUP |
| Status LED | GPIO 2 | Built-in LED (opsional) |

Pin ini sesuai dengan kode di `firmware/wifi_slave/wifi_slave.ino`.

## 5. TM1637 4-Digit Display

Display ditempel di mainan supaya customer/staff melihat sisa waktu.

```
TM1637 VCC -> ESP32 3.3V atau 5V (5V lebih terang)
TM1637 GND -> GND
TM1637 CLK -> GPIO 22
TM1637 DIO -> GPIO 23
```

Format display:

```
RUNNING:  MM:SS (titik dua berkedip)
PAUSED:   MM:SS (titik dua berkedip)
LOCKED:   ----
ENDED:    ----
FAULT:    ----
```

## 6. Buzzer

Buzzer aktif (bukan passive/piezo speaker). Langsung dikontrol GPIO.

```
Buzzer (+) -> GPIO 27
Buzzer (-) -> GND
```

Feedback audio:
- 1x beep pendek (50ms): setiap command diterima
- 2x beep (200ms): peringatan 1 menit tersisa
- 1x beep (50ms): 10 detik terakhir countdown
- 3x beep (150ms): registrasi sukses
- 1x beep panjang (1000ms): waktu habis
- 3x beep (100ms) + display kedip: IDENTIFY command (cari fisik mainan)

## 7. Button Fisik (Resume)

```
Button -> GPIO 32 (INPUT_PULLUP)
Button -> GND
```

Saat ditekan (LOW): resume timer dari state PAUSED atau FAULT.

Debounce: software 300ms di kode.

## 8. Pemilihan Relay Murah

Gunakan:
- **Relay koil 3V atau 3.3V** (bukan 5V).
- Contact rating **minimal 3A**, ideal **5A**.
- Kontak NO/COM (Normally Open).

Hindari:
- Relay koil 5V langsung dari 1x18650 (tidak reliable).
- Koil relay langsung dari GPIO ESP32 (arus kurang).
- Arus motor mainan lewat breadboard rails.

Jika pakai modul relay murah:
- Cek bisa di-trigger dari logika 3.3V ESP32.
- Cek tegangan suplai modul relay.
- Cek modul sudah punya transistor driver + diode.

## 9. Perilaku Safety Wajib

Firmware harus melakukan ini:

```
setup:
  konfigurasi pin relay sebagai OUTPUT
  set relay OFF sebelum inisialisasi apapun

boot:
  relay OFF
  load timer tersimpan dari NVS
  jika remaining_seconds > 0:
      state = RUNNING (auto-resume setelah peringatan 3 detik)
  lainnya:
      state = LOCKED
```

Relay ON hanya jika:
```
state == RUNNING
remaining_seconds > 0
```
