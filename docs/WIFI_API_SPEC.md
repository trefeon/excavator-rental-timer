# Wi-Fi API Specification for Android

Dokumen ini menjelaskan bagaimana Aplikasi Android berkomunikasi dengan sistem Excavator Rental menggunakan arsitektur **Wi-Fi Master-Slave**.

| 1 | EXC-01 | `192.168.4.11` |
| 2 | EXC-02 | `192.168.4.12` |
| 3 | EXC-03 | `192.168.4.13` |
| N | EXC-N | `192.168.4.(10 + N)` |

---

## 3. API Endpoints (Master)

Semua permintaan dari Android kini **HANYA ditembak ke Master IP (`192.168.4.1`)**. Anda tidak perlu melacak atau menembak IP tiap Slave secara manual. Master akan mengaturnya di belakang layar!

### A. Mendapatkan Status Seluruh Armada
Endpoint ini dipanggil oleh Android (misalnya setiap 1-2 detik) untuk mendapatkan *live status* dari SEMUA mainan yang sedang menyala secara instan, karena datanya disajikan langsung dari RAM Master.

- **URL:** `GET http://192.168.4.1/api/slaves`
- **Response Format:** JSON Array (HTTP 200 OK)

**Contoh Response:**
```json
[
  {
    "id": 1,
    "ip": "192.168.4.11",
    "mac": "AA:BB:CC:DD:EE:FF",
    "online": true,
    "state": "RUNNING",
    "rem": 299,
    "disp": "04:59",
    "paid": 300,
    "bat": "OK"
  }
]
```

---

### B. Mengirim Perintah (Command)
Endpoint ini dipanggil saat Anda menekan tombol di Aplikasi Android (Tambah Waktu, Pause, Stop) untuk mainan tertentu. Master akan mem-*forward* perintah ini ke Slave yang tepat secara transparan.

- **URL:** `POST http://192.168.4.1/api/command`
- **Header:** `Content-Type: application/json` (atau text/plain)
- **Request Body (JSON):**
```json
{
  "id": 1,
  "cmd": "ADD_TIME",
  "val": 300
}
```

**Daftar Command (`cmd`):**
| Command | Deskripsi `val` | Keterangan |
|---|---|---|
| `ADD_TIME` | Waktu dalam **detik** | Menambah sisa waktu dan langsung mengubah status menjadi `RUNNING`. (Misal: 5 menit = `300`). |
| `PAUSE` | `0` | Menjeda hitungan mundur, mematikan Relay, status menjadi `PAUSED`. |
| `RESUME` | `0` | Melanjutkan sisa waktu, menyalakan Relay, status kembali `RUNNING`. |
| `STOP` | `0` | Mereset waktu jadi 0, mematikan Relay, status menjadi `LOCKED`. |

---

### C. Manajemen Registry Slave
Digunakan jika Aplikasi Android ingin memiliki fitur "Halaman Admin" untuk menghapus/merename mainan.

#### Mengubah ID Mainan
- **URL:** `POST http://192.168.4.1/api/edit_slave`
- **Body:** `{"mac": "AA:BB:CC...", "id": 5}`

#### Menghapus Mainan dari Memori Master
- **URL:** `POST http://192.168.4.1/api/delete_slave`
- **Body:** `{"mac": "AA:BB:CC..."}`

---

## 4. Tips untuk Developer Android (Retrofit / OkHttp)
1. Karena ini berjalan di jaringan lokal IP Address (bukan HTTPS), pastikan Anda mengizinkan **Cleartext Traffic** di Android. (`android:usesCleartextTraffic="true"`)
2. Set Timeout HTTP Client Anda menjadi sekitar `2 detik`.
3. Gunakan `GET /api/slaves` untuk me-render `RecyclerView` (daftar ekskavator). Karena datanya berupa *array* yang sudah lengkap dengan status dan *countdown* waktu, Anda tinggal *bind* ke *ViewHolder* tanpa perlu membuat request terpisah per-mainan!
4. Cukup simpan Base URL `http://192.168.4.1` di aplikasi, Master yang akan memikirkan sisanya!
