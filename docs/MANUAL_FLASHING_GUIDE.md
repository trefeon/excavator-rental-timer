# Panduan Flash Firmware — Excavator Rental Timer

Panduan ini untuk flash firmware ke board controller excavator langsung dari browser.

**Yang dibutuhkan:**
- Komputer / Laptop (Windows, Mac, atau Linux)
- Browser **Google Chrome** atau **Microsoft Edge**
- Kabel USB data (bukan kabel charge-only)
- File firmware `.bin` yang sudah diberikan

---

## Step 1: Install Driver USB

Kalau pertama kali colok board ke komputer, install driver dulu agar board terdeteksi:

| Chip USB | Driver | Download |
|---|---|---|
| **CH340** (paling umum) | CH340 Driver | https://sparks.gogo.co.nz/ch340.html |
| **CP2102** | CP210x Driver | https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers |

Setelah install, restart komputer jika perlu.

---

## Step 2: Colok Board + Masuk Boot Mode (Jika Perlu)

1. Colok board ke USB komputer.

2. **Master ESP32 (Board Besar)** biasanya **langsung terdeteksi dan bisa di-flash otomatis** tanpa menekan tombol apapun.

3. **Slave ESP32-C3 Super Mini (Board Kecil)** tidak memiliki sirkuit auto-reset. Jika gagal terhubung, masukkan ke **Boot Mode** secara manual:
   * Cabut kabel USB dari board C3 Super Mini.
   * Tahan tombol **BOOT** (tombol kecil di board).
   * Colokkan kembali kabel USB ke komputer.
   * Lepaskan tombol **BOOT** setelah 1 detik.

---

## Step 3: Buka Web Flasher

Buka link ini di **Chrome** atau **Edge**:

### 👉 https://esptool.spacehuhn.com/

> ❌ **Tidak bisa** pakai Firefox, Safari, atau browser di HP/tablet.

---

## Step 4: Connect ke Board

1. Klik tombol **Connect** di pojok kanan atas
2. Muncul popup — pilih port USB board kamu (contoh: `COM5` atau `USB Serial Device`)
3. Kalau sudah connect, akan muncul info chip di layar

> Kalau tidak ada port yang muncul:
> - Cek kabel USB (harus kabel **data**, bukan charge-only)
> - Install driver di Step 1
> - Coba masuk Boot Mode di Step 2

---

## Step 5: Upload Firmware

### Opsi A: File Merged (1 file — paling gampang)

Kalau kamu dapat **1 file** (contoh: `master.bin` atau `slave.bin`):

1. Di baris pertama, masukkan alamat: **`0x0`**
2. Klik **Choose File** → pilih file `.bin` yang diberikan
3. Klik **Program**
4. Tunggu sampai selesai ✅

---

### Opsi B: 3 File Terpisah

Kalau kamu dapat **3 file** (bootloader, partitions, firmware), alamatnya **BEDA** tergantung board:

#### Untuk Master (ESP32 — board besar):

| Baris | Alamat | File |
|---|---|---|
| 1 | **`0x1000`** | `bootloader.bin` |
| 2 | `0x8000` | `partitions.bin` |
| 3 | `0x10000` | `firmware.bin` |

#### Untuk Slave (ESP32-C3 Super Mini — board kecil):

| Baris | Alamat | File |
|---|---|---|
| 1 | **`0x0`** | `bootloader.bin` |
| 2 | `0x8000` | `partitions.bin` |
| 3 | `0x10000` | `firmware.bin` |

> ⚠️ **Perbedaan penting:** Master pakai `0x1000`, Slave pakai `0x0` untuk bootloader. Jangan tertukar!

Klik **Program** dan tunggu sampai selesai.

---

## Step 6: Selesai!

1. Klik **Disconnect** di web flasher
2. Cabut kabel USB
3. Colok ulang — board akan langsung jalan dengan firmware baru

---

## Troubleshooting

| Masalah | Solusi |
|---|---|
| Port tidak muncul di popup | Coba kabel USB lain. Install driver CH340/CP2102. |
| "Failed to connect" | Masuk Boot Mode (lihat Step 2), lalu coba Connect lagi. |
| Board hidup tapi tidak jalan | Salah alamat bootloader. Master = `0x1000`, Slave = `0x0`. Flash ulang dengan alamat yang benar. |
| Board mati total setelah flash | Tidak rusak. Erase dulu: connect board → centang **Erase** di web flasher → flash ulang. |
| Popup bilang "No compatible devices" | Pastikan pakai Chrome atau Edge. Browser lain tidak support. |
