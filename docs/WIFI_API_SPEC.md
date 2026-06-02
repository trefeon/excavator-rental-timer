# Spesifikasi API Wi-Fi untuk Android

Dokumen ini menjelaskan bagaimana Aplikasi Android berkomunikasi dengan sistem Excavator Rental menggunakan arsitektur **Wi-Fi Master-Slave**.

---

## 1. Arsitektur Komunikasi

```
Aplikasi Android          Master ESP32              Slave ESP32
     │                   (192.168.4.1)            (192.168.4.x)
     │                        │                        │
     │── GET /api/slaves ──►  │  (data dari RAM)       │
     │◄── JSON array ─────────│                        │
     │                        │                        │
     │── POST /api/command ─► │── POST /api/command ──►│
     │◄── response ───────────│◄── response ───────────│
     │                        │                        │
     │                        │── GET /api/state ────► │ (polling tiap 1-2 detik)
     │                        │◄── status ─────────────│
```

**Prinsip:** Android **HANYA** berkomunikasi dengan Master (`192.168.4.1`). Tidak perlu tahu IP masing-masing Slave.

---

## 2. Konfigurasi Android

Karena ini berjalan di jaringan lokal (HTTP, bukan HTTPS):

```xml
<!-- AndroidManifest.xml -->
<application android:usesCleartextTraffic="true">
```

**HTTP Client tips:**
- Timeout: **2 detik**.
- Base URL: `http://192.168.4.1`.
- Polling interval: **2-3 detik** (jangan 1 detik, kurangi beban AP).
- Content-Type: `application/json`.

---

## 3. Endpoint API Master

Semua endpoint menggunakan base URL `http://192.168.4.1`.

### A. Mendapatkan Status Seluruh Armada

Mengembalikan status real-time semua mainan dari RAM Master.

- **URL:** `GET /api/slaves`
- **Response:** `200 OK` — JSON Array

```json
[
  {
    "id": 1,
    "ip": "192.168.4.2",
    "mac": "AA:BB:CC:DD:EE:FF",
    "online": true,
    "state": "RUNNING",
    "rem": 270,
    "disp": "04:30",
    "paid": 300,
    "bat": "OK"
  },
  {
    "id": 2,
    "ip": "192.168.4.3",
    "mac": "11:22:33:44:55:66",
    "online": false,
    "state": "LOCKED",
    "rem": 0,
    "disp": "--:--",
    "paid": 0,
    "bat": "OK"
  }
]
```

**Catatan field:**
- `online`: `true` jika lastSeen < 30 detik.
- `state`: `LOCKED`, `RUNNING`, `PAUSED`, `ENDED`, `FAULT`.
- `rem`: sisa waktu dalam detik.
- `disp`: format `MM:SS` untuk ditampilkan.
- `bat`: saat ini selalu `"OK"` (ADC belum diimplementasi).

---

### B. Mengirim Perintah ke Slave

Master meneruskan (proxy) perintah ke Slave yang dituju.

- **URL:** `POST /api/command`
- **Header:** `Content-Type: application/json`
- **Body:**

```json
{
  "id": 1,
  "cmd": "ADD_TIME",
  "val": 300
}
```

**Response sukses (200 OK):**
```json
{
  "ok": 1,
  "code": "OK",
  "rem": 300,
  "state": "RUNNING"
}
```

**Response gagal:**
```json
{
  "ok": 0,
  "error": "Slave offline/timeout"
}
```

#### Daftar Command

| Command | `val` | Deskripsi |
|---------|-------|-----------|
| `ADD_TIME` | detik (300 = 5 menit) | Tambah waktu. Jika LOCKED/ENDED → auto RUNNING |
| `PAUSE` | 0 | Jeda timer, relay OFF |
| `RESUME` | 0 | Lanjutkan timer, relay ON |
| `STOP` | 0 | Reset waktu ke 0, state LOCKED |
| `IDENTIFY` | 0 | Bunyikan buzzer 3x + kedip layar |
| `REBOOT` | 0 | Restart ESP32 slave |

---

### C. Transfer Sisa Waktu

Memindahkan sisa waktu dari mainan rusak ke mainan cadangan. Master akan:
1. Verifikasi target online.
2. Ambil sisa waktu dari source.
3. STOP source.
4. ADD_TIME ke target.
5. Jika target gagal → revert (kembalikan waktu ke source).

- **URL:** `POST /api/transfer_time`
- **Body:**
```json
{
  "from_id": 1,
  "to_id": 2
}
```
- **Response:** `200 OK` — `{"ok":1}` atau `{"ok":0,"error":"..."}`

---

### D. Manajemen Registry

#### Ubah ID Mainan
- **URL:** `POST /api/edit_slave`
- **Body:**
```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "id": 5
}
```
- **Response:** `200 OK` — `{"ok":1}` atau `400` jika ID sudah dipakai.

#### Hapus Mainan dari Registry
- **URL:** `POST /api/delete_slave`
- **Body:**
```json
{
  "mac": "AA:BB:CC:DD:EE:FF"
}
```
- **Response:** `200 OK` — `{"ok":1}`

---

### E. Riwayat Penggunaan (Usage History)

Master menyimpan total waktu bermain (paid) untuk setiap Slave di NVS Flash. Setiap kali session berakhir (RUNNING/PAUSED → LOCKED/ENDED), master mencatat durasi session.

#### Lihat Riwayat
- **URL:** `GET /api/history`
- **Query (optional):** `?id=1` untuk Slave tertentu
- **Response:** `200 OK` — JSON Array

```json
[
  {
    "id": 1,
    "mac": "80:F3:DA:63:25:DC",
    "totalSec": 7200,
    "sessions": 12,
    "lastSec": 600,
    "lastTime": 12345,
    "online": true
  }
]
```

**Field:**
- `totalSec`: Total detik yang sudah dibayar (lifetime)
- `sessions`: Jumlah session bermain
- `lastSec`: Durasi session terakhir (detik)
- `lastTime`: Timestamp terakhir bermain (millis/1000 sejak boot master)
- `online`: Status online slave

#### Reset Riwayat (Password Required)
- **URL:** `POST /api/history/reset`
- **Body:**
```json
{
  "password": "admin123",
  "id": 1
}
```
- `id` (optional): Jika diisi, reset hanya Slave tertentu. Jika kosong, reset semua.
- **Response:** `200 OK` — `{"ok":1}` atau `403` — `{"ok":0,"error":"Wrong password"}`

---

### F. Registrasi / Heartbeat (Internal — Slave ke Master)

- **URL:** `GET /api/register?mac=AA:BB:CC:DD:EE:FF`
- **Response:**
```json
{
  "id": 3
}
```

Digunakan oleh Slave saat boot dan setiap 15 detik sebagai heartbeat.

---

## 4. Autentikasi & User Management

### Login
- **URL:** `POST /api/login`
- **Body:**
```json
{
  "username": "superadmin",
  "password": "super123"
}
```
- **Response:** `200 OK`
```json
{
  "ok": 1,
  "token": "a1b2c3d4...",
  "username": "superadmin",
  "role": 0
}
```
- Role: `0` = SuperAdmin, `1` = Admin, `2` = Staff
- Token berlaku 24 jam. Kirim di header `Authorization: Bearer <token>`.

### List Users (Admin+)
- **URL:** `GET /api/users`
- **Header:** `Authorization: Bearer <token>`
- **Response:** JSON Array `[{ "username": "admin1", "role": 1 }, ...]`

### Create User (SuperAdmin only)
- **URL:** `POST /api/users`
- **Body:**
```json
{
  "username": "staff2",
  "password": "staff123",
  "role": 2
}
```

### Delete User (SuperAdmin only)
- **URL:** `POST /api/users/delete`
- **Body:** `{ "username": "staff2" }`

### Change Password (any logged-in user)
- **URL:** `POST /api/users/change-password`
- **Body:**
```json
{
  "old_password": "old123",
  "new_password": "new456"
}
```

---

## 5. Paket Waktu & Harga

Paket waktu sudah ditentukan (fixed). Admin bisa mengatur harganya.

### Lihat Paket (Public)
- **URL:** `GET /api/packages`
- **Response:**
```json
[
  { "id": 0, "durationMin": 1, "priceIDR": 5000 },
  { "id": 1, "durationMin": 2, "priceIDR": 8000 },
  { "id": 2, "durationMin": 3, "priceIDR": 10000 },
  { "id": 3, "durationMin": 5, "priceIDR": 15000 },
  { "id": 4, "durationMin": 10, "priceIDR": 25000 },
  { "id": 5, "durationMin": 30, "priceIDR": 60000 },
  { "id": 6, "durationMin": 60, "priceIDR": 100000 }
]
```

### Update Harga Paket (Admin+)
- **URL:** `POST /api/packages/update`
- **Header:** `Authorization: Bearer <token>`
- **Body:** `{ "id": 0, "priceIDR": 5000 }`

---

## 6. Riwayat & Pendapatan

### Riwayat Penggunaan
- **URL:** `GET /api/history`
- **Response:** `200 OK` — JSON Array

### Pendapatan per Slave (Admin+)
- **URL:** `GET /api/revenue`
- **Header:** `Authorization: Bearer <token>`
- **Response:**
```json
[
  { "id": 1, "totalSec": 7200, "sessions": 12, "revenueIDR": 120000 },
  { "id": 3, "totalSec": 3600, "sessions": 6, "revenueIDR": 60000 },
  { "totalRevenueIDR": 180000 }
]
```

### Reset Riwayat & Pendapatan (Admin + Password)
- **URL:** `POST /api/history/reset` atau `POST /api/revenue/reset`
- **Body:**
```json
{
  "password": "admin123",
  "id": 1
}
```
- `id` (optional): Jika diisi, reset hanya Slave tertentu. Jika kosong, reset semua.

---

## 7. Endpoint Slave (Internal — Dipanggil Master)

Endpoint ini dipanggil langsung oleh Master ke IP Slave. Android **tidak perlu** memanggilnya langsung.

### GET /api/state
Mengembalikan status real-time Slave.

```json
{
  "toy": "EXC-01",
  "state": "RUNNING",
  "rem": 270,
  "disp": "04:30",
  "paid": 300,
  "bat": "OK",
  "fault": 0,
  "seq": 5
}
```

### POST /api/command
Menerima perintah dari Master. Format body sama seperti Master proxy, **tanpa field `id`**:

```json
{
  "cmd": "ADD_TIME",
  "val": 300
}
```

---

## 8. Kode Error

| HTTP Status | Arti |
|-------------|------|
| 200 | Sukses |
| 400 | Bad request (parameter kurang/salah) |
| 404 | Slave tidak ditemukan di registry |
| 502 | Slave offline / timeout |
| 503 | Server busy (mutex timeout) |

---

## 9. Tips Developer Android (Retrofit / OkHttp / Ktor)

```kotlin
// Contoh Retrofit interface
interface ExcavatorApi {
    @GET("api/slaves")
    suspend fun getSlaves(): List<SlaveDto>

    @POST("api/command")
    suspend fun sendCommand(@Body cmd: CommandDto): CommandResponseDto

    @POST("api/transfer_time")
    suspend fun transferTime(@Body transfer: TransferDto): StatusDto
}

// Base URL
val retrofit = Retrofit.Builder()
    .baseUrl("http://192.168.4.1/")
    .client(OkHttpClient.Builder()
        .connectTimeout(2, TimeUnit.SECONDS)
        .readTimeout(2, TimeUnit.SECONDS)
        .build())
    .addConverterFactory(GsonConverterFactory.create())
    .build()
```

**State color mapping:**
| State | Warna |
|-------|-------|
| `RUNNING` | Hijau |
| `PAUSED` | Oranye/kuning |
| `LOCKED` | Abu-abu |
| `ENDED` | Merah |
| `FAULT` | Merah tua |
| `OFFLINE` | Abu-abu pudar |

---

## 10. Catatan

- Master menyimpan registry MAC→ID di NVS Flash (persistent).
- Slave melakukan heartbeat setiap 15 detik untuk update `lastSeen`.
- Status `online` ditentukan oleh: `lastSeen < 30 detik`.
- Polling interval dashboard: **2 detik** (jangan lebih cepat, kurangi beban jaringan AP).
