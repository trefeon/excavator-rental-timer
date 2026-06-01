# Spesifikasi API Wi-Fi untuk Android

Dokumen ini menjelaskan bagaimana Aplikasi Android berkomunikasi dengan sistem Excavator Rental menggunakan arsitektur **Wi-Fi Master-Slave**.

---

## 1. Arsitektur Komunikasi

```
Aplikasi Android          Master ESP32              Slave ESP32
     │                   (192.168.4.1)            (192.168.4.x)
     │                        │                        │
     │── GET /api/slaves ──► │  (data dari RAM)        │
     │◄── JSON array ────── │                        │
     │                        │                        │
     │── POST /api/command ─►│── POST /api/command ──►│
     │◄── response ──────── │◄── response ────────── │
     │                        │                        │
     │                        │── GET /api/state ────►│ (polling tiap 1-2 detik)
     │                        │◄── status ─────────── │
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

### E. Registrasi / Heartbeat (Internal — Slave ke Master)

- **URL:** `GET /api/register?mac=AA:BB:CC:DD:EE:FF`
- **Response:**
```json
{
  "id": 3
}
```

Digunakan oleh Slave saat boot dan setiap 15 detik sebagai heartbeat.

---

## 4. Endpoint Slave (Internal — Dipanggil Master)

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

## 5. Kode Error

| HTTP Status | Arti |
|-------------|------|
| 200 | Sukses |
| 400 | Bad request (parameter kurang/salah) |
| 404 | Slave tidak ditemukan di registry |
| 502 | Slave offline / timeout |
| 503 | Server busy (mutex timeout) |

---

## 6. Tips Developer Android (Retrofit / OkHttp / Ktor)

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

## 7. Catatan

- Master menyimpan registry MAC→ID di NVS Flash (persistent).
- Slave melakukan heartbeat setiap 15 detik untuk update `lastSeen`.
- Status `online` ditentukan oleh: `lastSeen < 30 detik`.
- Polling interval dashboard: **2 detik** (jangan lebih cepat, kurangi beban jaringan AP).
