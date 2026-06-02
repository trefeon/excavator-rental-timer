# Alur Aplikasi Android / Mockup Spec

## 1. Peran Aplikasi

Aplikasi Android adalah dashboard dan pengirim command. ESP32 Slave tetap source of truth untuk timer dan relay.

Tanggung jawab app:

- Panggil `GET /api/slaves` ke Master untuk daftar mainan.
- Tampilkan status hingga 9 mainan online bersamaan dalam satu layar.
- Kirim command via `POST /api/command` ke Master.
- Simpan log transaksi lokal.
- Pantau status real-time dengan polling berkala.

## 2. Layar Utama

```
Dashboard
  → Detail Mainan
  → Tambah Waktu
  → Log Transaksi
```

## 3. Layar Dashboard

Tujuan: tampilan cepat untuk operator.

Baris daftar mainan:

```
┌────────────────────────────────────────────┐
│ EXC-01  RUNNING   04:30  OK   [+5] [Pause] │
│ EXC-02  LOCKED    --     OK   [+5]         │
│ EXC-03  PAUSED    02:40  OK   [Resume]     │
│ EXC-04  OFFLINE   --     --   [Edit] [Del] │
└────────────────────────────────────────────┘
```

Aksi per baris:
- Tap baris → detail mainan.
- Long press → quick `+5 menit`.
- Tombol inline: `+5 menit`, `Pause/Resume`, `Stop`.

Warna status:

| State | Warna |
|-------|-------|
| `RUNNING` | Hijau |
| `PAUSED` | Oranye |
| `LOCKED` | Abu-abu |
| `ENDED` | Merah |
| `FAULT` | Merah tua |
| `OFFLINE` | Abu-abu pudar |

## 4. Layar Detail Mainan

Field yang ditampilkan:

```
Toy ID:       EXC-01
State:        RUNNING
Remaining:    04:30
Battery:      OK
IP:           192.168.4.2
MAC:          AA:BB:CC:DD:EE:FF
Last Seen:    2 detik lalu
```

Tombol aksi:

```
[+5 menit]   [+10 menit]
[Pause]      [Resume]
[STOP / Kunci]           ← dengan konfirmasi
[Transfer Waktu]          ← pindah ke mainan lain
[Identify / Ping]         ← cari fisik mainan
```

## 5. Alur Kirim Command

```
1. Staff tap tombol (misal: +5 menit di EXC-01)
2. App build JSON: {"id": 1, "cmd": "ADD_TIME", "val": 300}
3. App POST ke http://192.168.4.1/api/command
4. Master forward ke Slave EXC-01
5. Master kembalikan response ke App
6. App update UI dari response / polling berikutnya
7. App simpan log transaksi lokal
```

**Retry logic:**
```
Jika response 502 (Slave offline/timeout):
  → Tampilkan pesan "Mainan tidak terjangkau"
  → Jangan buat transaksi sampai sukses
  → Coba lagi setelah 3 detik (maks 3x)
```

## 6. Alur Polling Dashboard

```
1. App buka Dashboard
2. App GET /api/slaves setiap 2 detik
3. Response JSON array → update RecyclerView/List
4. Setiap item di-bind dengan: id, state, disp, bat, online
5. Warna baris berubah sesuai state
6. Jika mainan offline > 30 detik → tampilkan abu-abu
```

## 7. Data Lokal

Profil mainan (cache):

```
toy_id
mac
ip
last_seen_state
last_seen_remaining
last_seen_at
```

Log transaksi:

```
timestamp
toy_id
command
value
response_code
remaining_after
operator_id (opsional)
```

## 8. Penanganan Offline

```
lastSeen < 30 detik  → ONLINE
lastSeen >= 30 detik → OFFLINE
```

Jika command ke mainan offline:
- Tampilkan: "EXC-01 sedang offline. Periksa battery atau jangkauan Wi-Fi."
- Jangan buat transaksi sampai ACK sukses.

## 9. Keamanan

- App hanya berkomunikasi dengan Master di `192.168.4.1`.
- Gunakan `android:usesCleartextTraffic="true"` karena jaringan lokal HTTP.
- Tidak ada autentikasi user untuk MVP (Wi-Fi AP terisolasi).
- Untuk versi produksi: tambahkan PIN / password di Master.

## 10. Mockup Referensi

Mockup interaktif tersedia di:

```
docs/android-dashboard-mockup.html
```
