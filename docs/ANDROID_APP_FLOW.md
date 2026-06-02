# Alur Aplikasi Android

## 1. Peran Aplikasi

Aplikasi Android adalah dashboard dan pengirim command. ESP32 Slave tetap source of truth untuk timer dan relay.

Tanggung jawab app:
- Login dan autentikasi (token-based)
- Panggil `GET /api/slaves` ke Master untuk daftar mainan
- Tampilkan status excavator
- Kirim command via `POST /api/command` ke Master
- Tampilkan riwayat dan pendapatan
- Pantau status real-time dengan polling berkala

---

## 2. Login Flow

```
┌─────────────┐
│ Login Screen│
│  username   │
│  password   │
│  [Masuk]    │
└──────┬──────┘
       │
       ▼
POST /api/login
{username, password}
       │
       ├── 200 → Simpan token di localStorage
       │         → Buka Dashboard
       │
       └── 401 → Tampilkan error
```

**Token:**
- Disimpan di `localStorage`
- dikirim di header `Authorization: Bearer <token>`
- Berlaku 24 jam
- Jika 401 → otomatis logout, kembali ke login

---

## 3. Role & Permission

| Role | Fitur |
|------|-------|
| **SuperAdmin** | Semua + kelola user + reboot device |
| **Admin** | Semua + reset data + edit package |
| **Staff** | Add time, pause, resume, stop |

---

## 4. Layar Dashboard

### Device Card
```
┌─────────────────────────┐
│ EXC-01          RUNNING │
│         04:59           │
│ Sisa: 4j 59m • Bayar: 5j 0m │
│ ┌─────────────────────┐ │
│ │ Riwayat      1 sesi │ │
│ │ Total: 5 menit      │ │
│ │ Terakhir: 5m 0d     │ │
│ │          Rp15.000   │ │
│ └─────────────────────┘ │
│ [1] [2] [3] [5] ...    │
│ [Jeda]     [Stop]      │
│ [Identify] [Transfer]  │
│ [Edit ID]  [Hapus]     │
│ [Reboot]               │
└─────────────────────────┘
```

### Package Buttons
| Set | Durasi | Harga |
|-----|--------|-------|
| 1 | 1 menit | Rp 5.000 |
| 2 | 2 menit | Rp 8.000 |
| 3 | 3 menit | Rp 10.000 |
| 5 | 5 menit | Rp 15.000 |
| 10 | 10 menit | Rp 25.000 |
| 30 | 30 menit | Rp 60.000 |
| 60 | 60 menit | Rp 100.000 |

---

## 5. Alur Kirim Command

```
1. Staff tap tombol (misal: Set 5 di EXC-01)
2. App build JSON: {"id": 1, "cmd": "ADD_TIME", "val": 5}
3. App POST ke http://192.168.4.1/api/command
   Header: Authorization: Bearer <token>
4. Master convert: val=5 → 300 detik (lookup package)
5. Master forward ke Slave EXC-01
6. Master kembalikan response ke App
7. App update UI dari response / polling berikutnya
```

**ADD_TIME val mapping:**
| val | Detik |
|-----|-------|
| 1 | 60 |
| 2 | 120 |
| 3 | 180 |
| 5 | 300 |
| 10 | 600 |
| 30 | 1800 |
| 60 | 3600 |

---

## 6. Alur Transfer

```
1. Staff klik [Transfer] di EXC-01
2. App tampilkan daftar slave lain yang online
3. Staff pilih target (misal: EXC-02)
4. App POST /api/transfer_time
   {from_id: 1, to_id: 2}
5. Master:
   a. Validasi: from != to
   b. Verifikasi kedua slave online
   c. Ambil rem dari source
   d. STOP source
   e. STOP target (replace)
   f. ADD_TIME ke target
6. App update UI
```

---

## 7. Alur Reset Data

### Reset Total Waktu
```
POST /api/history/reset
{password: "admin123"}
→ Reset: totalSec, sessions
```

### Reset Pendapatan
```
POST /api/revenue/reset
{password: "admin123"}
→ Reset: revenueIDR
```

### Reset Semua
```
POST /api/reset-all
{password: "admin123"}
→ Reset: history + revenue
```

---

## 8. Polling Dashboard

```
1. App buka Dashboard
2. App GET /api/slaves setiap 3 detik
3. App GET /api/history setiap 3 detik (untuk riwayat per slave)
4. App GET /api/revenue setiap 3 detik (untuk pendapatan, admin only)
5. Response JSON → update UI
6. Warna status berubah sesuai state
```

---

## 9. Data Lokal

### Token (localStorage)
```
token: "a1b2c3d4..."
role: 0
username: "superadmin"
```

### Cache
```
devices: [...]
packages: [...]
history: [...]
revenue: [...]
```

---

## 10. State Color Mapping

| State | Warna | Icon |
|-------|-------|------|
| `RUNNING` | Hijau | ▶ |
| `PAUSED` | Oranye | ⏸ |
| `LOCKED` | Abu-abu | 🔒 |
| `ENDED` | Merah | ⏹ |
| `OFFLINE` | Abu-abu pudar | ⚫ |

---

## 11. Error Handling

| HTTP Status | Arti | Aksi |
|-------------|------|------|
| 200 | Sukses | Update UI |
| 400 | Bad request | Tampilkan pesan error |
| 401 | Token expired | Logout, minta login ulang |
| 403 | Role tidak cukup | Sembunyikan fitur |
| 404 | Slave tidak ditemukan | Sembunyikan dari list |
| 502 | Slave offline | Tampilkan "Offline" |

---

## 12. Retrofit Example (Kotlin)

```kotlin
interface ExcavatorApi {
    @POST("api/login")
    suspend fun login(@Body body: LoginDto): LoginResponse

    @GET("api/slaves")
    suspend fun getSlaves(): List<SlaveDto>

    @POST("api/command")
    suspend fun sendCommand(@Body cmd: CommandDto): CommandResponse

    @POST("api/transfer_time")
    suspend fun transferTime(@Body transfer: TransferDto): StatusDto

    @GET("api/history")
    suspend fun getHistory(): List<HistoryDto>

    @GET("api/revenue")
    suspend fun getRevenue(): List<RevenueDto>

    @GET("api/packages")
    suspend fun getPackages(): List<PackageDto>

    @POST("api/packages/update")
    suspend fun updatePackage(@Body pkg: PackageDto): StatusDto

    @GET("api/users")
    suspend fun getUsers(): List<UserDto>

    @POST("api/users")
    suspend fun createUser(@Body user: UserDto): StatusDto

    @POST("api/users/delete")
    suspend fun deleteUser(@Body body: Map<String, String>): StatusDto

    @POST("api/users/change-password")
    suspend fun changePassword(@Body body: ChangePassDto): StatusDto

    @POST("api/history/reset")
    suspend fun resetHistory(@Body body: ResetDto): StatusDto

    @POST("api/revenue/reset")
    suspend fun resetRevenue(@Body body: ResetDto): StatusDto

    @POST("api/reset-all")
    suspend fun resetAll(@Body body: ResetDto): StatusDto
}

// Auth interceptor
class AuthInterceptor(private val tokenProvider: () -> String?) : Interceptor {
    override fun intercept(chain: Interceptor.Chain): Response {
        val request = chain.request().newBuilder()
        tokenProvider()?.let {
            addHeader("Authorization", "Bearer $it")
        }
        return chain.proceed(request.build())
    }
}
```
