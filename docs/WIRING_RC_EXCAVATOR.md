# Panduan Wiring & Instalasi Modul Slave pada RC Excavator

Dokumen ini berisi panduan *brainstorming* dan skema pemasangan (wiring) untuk mengintegrasikan modul ESP32-C3 Super Mini (sebagai Slave Timer) ke dalam mainan RC Excavator.

## 1. Konsep Pemutusan Daya (Cut-Off Strategy)
Tujuan utama modul ini adalah mematikan RC Excavator ketika waktu sewa habis. Cara paling efektif dan aman (Fail-Safe) untuk mainan rental adalah **memutus jalur daya utama (Baterai) ke Mainboard RC** menggunakan Relay.

**Keuntungan mode Normally Open (NO):** 
Jika modul ESP mati, rusak, atau error, Relay tidak akan aktif. RC akan otomatis mati (tidak bisa dimainkan diam-diam atau *loss control*).

---

## 2. Komponen yang Dibutuhkan
1. Modul ESP32-C3 Super Mini (sudah di-flash firmware `slave_c3`).
2. Relay Module 1-Channel (5V, sanggup menahan beban 10A).
3. Step-down / Buck Converter (misal: LM2596 atau Mini360) untuk menurunkan voltase baterai RC ke 5V.
4. TM1637 4-Digit Display.
5. Active Buzzer 3.3V/5V.
6. Push Button (Tactile Switch).
7. Kabel jumper / kabel tembaga awg 22.

---

## 3. Skema Wiring Utama

### A. Skema Catu Daya Bercabang (Y-Splitter)
Karena kita mau menjadikan Baterai RC sebagai **sumber daya tunggal** untuk ESP32 dan Mainboard RC, kita harus membuat kabelnya "bercabang" (paralel) dari konektor baterai.

Kabel dari konektor baterai RC dibuat menjadi **dua cabang**:
**Cabang 1 (Untuk menghidupkan ESP32):**
- Baterai RC (+) ➔ **IN (+)** Buck Converter.
- Baterai RC (-) ➔ **IN (-)** Buck Converter.
- **OUT (+)** Buck Converter *(disetel ke 5V)* ➔ **Pin 5V** di ESP32-C3.
- **OUT (-)** Buck Converter ➔ **Pin GND** di ESP32-C3.

**Cabang 2 (Untuk menghidupkan RC yang dikontrol Relay):**
- Baterai RC (-) ➔ Langsung masuk ke **Kabel (-) Mainboard RC** tanpa diputus.
- Baterai RC (+) ➔ Masuk dulu ke baut **COM** di Relay.
- Baut **NO (Normally Open)** di Relay ➔ Masuk ke **Kabel (+) Mainboard RC**.

*Skema Aliran Listriknya:*
`Baterai (+)` ──┬──> `Buck Converter` ──> `ESP32` (Selalu Hidup)
              │
              └──> `Relay (COM)` ──[Buka/Tutup]──> `Relay (NO)` ──> `Mainboard RC (+)`

### B. Wiring Kontrol Relay dari ESP32
Agar ESP32 bisa mengontrol *switch* relay di atas:
- **Pin VCC Relay** ➔ Pin 5V di ESP32-C3
- **Pin GND Relay** ➔ Pin GND di ESP32-C3
- **Pin IN / Signal Relay** ➔ **Pin GPIO 4** di ESP32-C3

> [!NOTE]
> Firmware `slave_c3` dikonfigurasi menggunakan logika **Active LOW (Low Trigger Relay)**. Sinyal LOW (0V) dari GPIO 4 akan mengaktifkan relay, dan sinyal HIGH (3.3V) akan mematikan relay. Jika Anda menggunakan MOSFET (misalnya P-channel dengan pull-up, atau rangkaian driver MOSFET inverter), pastikan logika ini disesuaikan.

### C. Wiring Peripheral (Layar, Buzzer, Tombol)
Sesuai dengan kodingan di firmware:

**1. TM1637 Display (Layar Timer):**
* VCC ➔ 5V ESP32
* GND ➔ GND ESP32
* CLK ➔ **GPIO 6**
* DIO ➔ **GPIO 7**

**2. Active Buzzer:**
* Kutub Positif (+) ➔ **GPIO 5**
* Kutub Negatif (-) ➔ GND ESP32

**3. Push Button (Tombol Resume Fisik):**
* Kaki 1 Tombol ➔ **GPIO 9**
* Kaki 2 Tombol ➔ GND ESP32

---

## 4. Tips Instalasi di Fisik Excavator

1. **Penempatan Layar (Display):** 
   Cari posisi datar di *body* atas excavator (misal di kaca samping kabin masinis atau di penutup mesin belakang). Lubangi plastik body seukuran kotak angka TM1637 agar dari luar terlihat rapi (angka timbul). Segel dengan lem bakar (Hot Glue) agar tahan cipratan air.
   
2. **Posisi Tombol & Buzzer:**
   * **Buzzer** sebaiknya dihadapkan ke ventilasi plastik RC agar suaranya nyaring keluar saat waktu habis.
   * **Tombol Resume** bisa disembunyikan di bagian bawah RC (dekat switch on/off bawaan pabrik) atau di belakang kabin, agar mudah dijangkau operator tapi tidak mudah dipencet asal-asalan oleh anak-anak.

3. **Perlindungan Modul ESP32 & Relay:**
   Bungkus ESP32 dan Buck Converter menggunakan solasi bakar (Heat Shrink Tube) transparan ukuran besar atau masukkan ke dalam kotak plastik kecil (misal kotak permen) lalu di-lem bakar sebelum disembunyikan di dalam lambung RC. Ini untuk menghindari korsleting akibat debu pasir atau air di arena bermain.

4. **Kekuatan Kabel Relay:**
   Karena arus motor excavator lumayan besar saat RC menanjak atau mengeruk pasir (bisa tembus 2-4 Ampere), **pastikan kabel yang masuk ke port COM dan NO di Relay cukup tebal**, jangan pakai kabel jumper dupont biasa.

---

## 5. Flow Kerja Hardware (Hardware Flow)
- RC dinyalakan (Switch ON pabrik ditekan).
- Baterai menyuplai Buck Converter ➔ ESP32 nyala.
- ESP32 mencari sinyal WiFi Master.
- Relay posisi **OFF (Mati)**, RC belum bisa digerakkan (Mainboard belum dapat setrum).
- Operator menambah waktu dari aplikasi Android ➔ Master mengirim instruksi ke ESP32.
- ESP32 men-trigger Relay (Relay bunyi klik) ➔ COM menyambung ke NO ➔ RC Excavator menyala dan bisa dikendalikan.
- Waktu habis ➔ Relay kembali **OFF** ➔ Mainboard mati seketika.
