# Panduan Flashing ESP32 via Browser (Web Flasher)

Panduan ini dibuat untuk memudahkan proses instalasi atau update firmware pada **ESP32 DOIT Dev Kit** dan **ESP32-C3 Super Mini** secara langsung melalui browser, tanpa perlu menginstal aplikasi tambahan di komputer Anda.

---

## 1. Persiapan Awal
Sebelum memulai, pastikan Anda telah menyiapkan hal-hal berikut:
- **Browser**: Gunakan **Google Chrome** atau **Microsoft Edge**. Browser seperti Firefox dan Safari tidak mendukung fitur Web Serial API sehingga tidak bisa digunakan.
- **Kabel Data USB**: Siapkan kabel USB Type-C (untuk ESP32-C3) atau Micro USB (untuk ESP32 DOIT). 
  > **⚠️ PERHATIAN PENTING**
  > Pastikan Anda menggunakan kabel data! Banyak kabel USB murah yang hanya bisa digunakan untuk charging (mengisi daya) dan tidak bisa mengirim data. Jika board tidak terdeteksi sama sekali, kemungkinan besar masalahnya ada pada kabel.
- **File Firmware**: Siapkan file `.bin` (contoh: `firmware.bin`) yang akan Anda flash.

---

## 2. Instalasi Driver USB
Jika saat menghubungkan ESP32 ke komputer, Port COM tidak terdeteksi di browser, Anda harus menginstal driver yang sesuai dengan board Anda.

### A. ESP32-C3 Super Mini
Board jenis ini biasanya menggunakan koneksi **Native USB**.
- **Windows 10 / Windows 11**: Biasanya **Plug-and-Play** dan langsung terdeteksi sebagai `USB Serial Device (COMx)`. Anda umumnya tidak perlu menginstal driver tambahan.

### B. ESP32 DOIT Dev Kit V1
Board jenis ini menggunakan chip eksternal untuk komunikasi serial. Periksa tulisan pada chip kecil berbentuk persegi di dekat port USB:
- **Jika chip CH340**: [Download Driver CH341SER.ZIP (Official WCH)](http://www.wch-ic.com/downloads/CH341SER_EXE.html). Ekstrak lalu jalankan file `setup.exe`.
- **Jika chip CP2102**: [Download CP210x Windows Driver (Official Silicon Labs)](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

---

## 3. Mode Bootloader (Khusus jika gagal Connect)
> **ℹ️ INFO PENTING**
> Langkah ini seringkali **Wajib** untuk ESP32-C3 Super Mini baru yang sering gagal dikoneksikan ke Web Flasher secara otomatis. 

Jika browser gagal terhubung saat Anda mengklik tombol "Connect", paksa masuk ke **Bootloader Mode** dengan cara:
1. Pastikan board ESP32 terhubung ke USB komputer.
2. Tekan dan **tahan** tombol **BOOT** (atau tombol angka 9 di board C3 Super Mini).
3. Sambil masih menahan tombol BOOT, tekan tombol **RESET** (atau EN) sekali, lalu lepaskan tombol RESET.
4. Terakhir, lepaskan tombol **BOOT**.
5. Board sekarang berada di mode Bootloader dan siap diflash. Coba klik Connect kembali di browser.

*(Catatan: ESP32 DOIT Dev Kit biasanya bisa masuk ke bootloader secara otomatis tanpa dipencet).*

---

## 4. Langkah Flashing via Web Flasher
Ikuti langkah-langkah di bawah ini untuk memulai proses flashing:

1. Buka halaman resmi Espressif Web Flasher di browser: **[https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/)**
2. Pada pengaturan **Baudrate**, pilih `460800` (atau biarkan di `115200`).
3. Klik tombol biru bertuliskan **Connect**.
4. Akan muncul jendela popup dari browser di sudut kiri atas. Pilih Port yang sesuai (misalnya `USB Serial Device (COM5)` atau `CP210x...`) lalu klik **Connect**.
   > *Jika Anda berhasil terhubung, tulisan di layar akan menunjukkan spesifikasi chip Anda (misal: "ESP32-C3 (revision 3)").*
5. Pada bagian **Flash Address**, ketik `0x10000` (jika Anda hanya melakukan update file `firmware.bin`).
6. Klik tombol **Choose File**, kemudian cari dan pilih file `firmware.bin` Anda.
7. Klik tombol **Program**.
8. Tunggu hingga bar progress berwarna hijau mencapai 100% dan muncul pesan log `Flash Done`.
9. Selesai! **Tekan tombol RESET** pada board ESP32 Anda untuk menjalankan firmware yang baru.

---

### FAQ & Troubleshooting

- **Pilihan Port tidak muncul di browser?**
  Pastikan kabel USB adalah kabel data, dan Anda sudah menginstal driver yang sesuai dengan chip serial.
  
- **Error "Failed to connect to device" atau layar berhenti di tulisan "Connecting..."?**
  Ini berarti komputer gagal memerintahkan board masuk ke mode flash. Lakukan langkah **Masuk ke Mode Bootloader** secara manual (Lihat Bagian 3).
  
- **Apakah saya perlu file selain firmware.bin?**
  Jika Anda mengupdate board yang sudah pernah diflash sebelumnya, cukup 1 file `firmware.bin` saja di alamat `0x10000`. Jika ini adalah board yang benar-benar baru dari pabrik (kosong), Anda perlu mem-flash file bootloader dan partition table terlebih dahulu, ATAU meminta file gabungan (`merged.bin`) dari developer Anda yang bisa langsung diflash di alamat `0x0`.
