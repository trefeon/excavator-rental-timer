# Panduan Flashing ESP32 via Browser (Web Flasher)

Panduan ini dibuat untuk memudahkan klien melakukan flashing firmware **Excavator Rental Timer** menggunakan file gabungan (`merged.bin`) langsung dari browser Google Chrome.

---

## 1. Persiapan Awal
Sebelum memulai, pastikan Anda telah menyiapkan hal-hal berikut:
* **Browser**: Gunakan **Google Chrome** atau **Microsoft Edge** pada Laptop/PC (Firefox, Safari, dan handphone/tablet tidak didukung).
* **Kabel USB**: Siapkan kabel USB data (kabel Type-C untuk ESP32-C3, atau Micro USB untuk ESP32 regular).
  > **⚠️ PERINGATAN PENTING**
  > Gunakan kabel USB yang mendukung transfer data (kabel bawaan HP yang bagus). Kabel charger murahan seringkali hanya bisa menyalurkan daya (charge-only) dan tidak bisa mengirim data, sehingga board tidak akan terdeteksi.
* **File Firmware**: Gunakan file gabungan (`merged.bin`) yang telah diberikan:
  * `master_webui_merged.bin` (untuk Master ESP32 besar)
  * `slave_c3_merged.bin` (untuk Slave ESP32-C3 kecil)

---

## 2. Instalasi Driver USB (Jika Board Tidak Terdeteksi)
Jika Port COM board tidak muncul di browser saat dicolokkan, instal driver berikut sesuai jenis board Anda:

* **ESP32 DOIT Dev Kit V1** (Master — board besar):
  * Cek chip hitam kecil di dekat port USB. Jika tertulis **CH340**, [Download & Install Driver CH340](http://www.wch-ic.com/downloads/CH341SER_EXE.html).
  * Jika tertulis **CP2102**, [Download & Install Driver CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).
* **ESP32-C3 Super Mini** (Slave — board kecil):
  * Biasanya langsung terdeteksi di Windows 10/11 sebagai `USB Serial Device (COMx)` secara otomatis tanpa driver tambahan.

---

## 3. Apakah Harus Menekan Tombol BOOT? (Auto Flashing)

* **Master ESP32 (Board Besar)**:
  * **TIDAK PERLU** menekan tombol apapun! Board ini memiliki sirkuit auto-reset. Saat Anda klik Connect di browser, board akan otomatis masuk ke mode flash sendiri.
* **Slave ESP32-C3 Super Mini (Board Kecil)**:
  * Karena ukurannya yang sangat kecil, board ini tidak memiliki sirkuit auto-reset otomatis.
  * **Jika gagal terhubung otomatis**, Anda harus memasukannya ke mode flash secara manual:
    1. Cabut kabel USB dari board C3 Super Mini.
    2. Tekan dan **tahan tombol BOOT** (tombol kecil di dekat port USB).
    3. Sambil tetap menahan tombol BOOT, **colokkan kabel USB** ke laptop/PC.
    4. Lepaskan tombol BOOT setelah kabel tercolok (sekitar 1 detik).
    5. Board sekarang dalam mode Bootloader dan siap dihubungkan.

---

## 4. Langkah Flashing via Web Flasher

Ikuti langkah-langkah di bawah ini untuk memulai proses instalasi firmware:

1. Buka halaman resmi Web Flasher di browser: **[https://esptool.spacehuhn.com/](https://esptool.spacehuhn.com/)**
2. Atur **Baudrate** ke **`115200`** (ini adalah kecepatan paling aman dan stabil untuk mencegah error/koneksi putus di tengah jalan).
3. Klik tombol biru **Connect**.
4. Akan muncul popup browser di pojok kiri atas. Pilih nama Port serial board Anda (misal: `USB Serial Device` atau `CP210x...`), lalu klik **Connect**.
   * *Jika berhasil terhubung, info jenis chip Anda akan tertulis di layar.*
5. Pada bagian input file:
   * **Address/Offset**: Isi dengan **`0x0`** (wajib `0x0` karena menggunakan file merged).
   * **Choose File**: Pilih file `.bin` yang sesuai (`master_webui_merged.bin` atau `slave_c3_merged.bin`).
6. Klik tombol **Program** (atau **Start Flashing**).
7. Tunggu hingga proses mencapai 100% dan muncul keterangan sukses.
8. Setelah selesai:
   * Untuk **Master ESP32**, board akan otomatis restart.
   * Untuk **Slave ESP32-C3 Super Mini**, cabut kabel USB lalu colokkan kembali (atau tekan tombol **RESET** sekali) agar board mulai berjalan.
