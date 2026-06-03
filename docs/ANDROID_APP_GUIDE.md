# Panduan Pengembangan Aplikasi Android

Last Updated: June 3, 2026

## 1. Peran Aplikasi Android

Aplikasi Android bertindak sebagai antarmuka (dashboard) dan pengelola pengguna (user management) secara mandiri. **Master ESP32 TIDAK lagi menangani autentikasi, login, atau manajemen user.**

Tanggung jawab aplikasi Android:

1. **Autentikasi Lokal/Cloud**: Menyimpan data user (SuperAdmin, Admin, Staff) di database lokal Android (misal SQLite/Room) atau cloud (Firebase).
2. **Dashboard Monitoring**: Memanggil `GET /api/slaves` ke Master ESP32 secara berkala (polling) untuk melihat status setiap excavator.
3. **Pengiriman Perintah**: Mengirim `POST /api/command` untuk menambah waktu, pause, resume, dll.
4. **Pembatasan Akses (Role Based)**: Menampilkan/menyembunyikan tombol fitur (seperti Reset Pendapatan) berdasarkan Role user yang sedang login di Android.

---

## 2. Koneksi ke Master ESP32

- **Wi-Fi SSID**: `ExcavatorMaster`
- **Password**: `12345678`
- **Base URL Master API**: `http://192.168.4.1`

_Catatan: Pastikan aplikasi Android mengizinkan cleartext traffic (HTTP biasa) di `AndroidManifest.xml` (`android:usesCleartextTraffic="true"`)._

---

## 3. Integrasi Endpoint (Tanpa Token)

Semua endpoint di Master ESP32 bersifat **terbuka (Open Access)**. Tidak ada validasi bearer token atau password di sisi ESP32. Aplikasi Android bertanggung jawab penuh untuk mencegah user biasa (Staff) menekan tombol Admin.

### Polling Status (Setiap 3 Detik)

Gunakan Retrofit/OkHttp untuk memanggil endpoint berikut:

- `GET /api/slaves`: Menampilkan daftar device, sisa waktu (`rem`), dan status (`RUNNING`, `LOCKED`, dll).
- `GET /api/history`: (Opsional) Mengambil data histori total bermain.
- `GET /api/revenue`: (Khusus Admin) Mengambil data pendapatan Rupiah.

### Pengiriman Command

Ketika Staff menekan tombol Set 5 Menit di EXC-01:

1. Aplikasi mengirim JSON: `{"id": 1, "cmd": "ADD_TIME", "val": 5}` ke `POST /api/command`.
2. Master mem-forward ke Slave EXC-01.
3. Master merespon dengan status berhasil.

### Transfer Waktu

Ketika Admin memindahkan sisa waktu dari EXC-01 ke EXC-02:

1. Aplikasi mengirim JSON: `{"from_id": 1, "to_id": 2}` ke `POST /api/transfer_time`.

### Reset Data (Khusus Admin)

Admin dapat menekan tombol Reset:

- `POST /api/history/reset` (Body kosong)
- `POST /api/revenue/reset` (Body kosong)
- `POST /api/reset-all` (Body kosong)

---

## 4. State Color Mapping

Desain UI di Android direkomendasikan menggunakan mapping warna berikut berdasarkan field `state` dari `/api/slaves`:

| State     | Rekomendasi Warna | Icon |
| --------- | ----------------- | ---- |
| `RUNNING` | Hijau             | ▶    |
| `PAUSED`  | Oranye            | ⏸    |
| `LOCKED`  | Abu-abu           | 🔒   |
| `ENDED`   | Merah             | ⏹    |
| `OFFLINE` | Abu-abu pudar     | ⚫   |

---

## 5. Contoh Konfigurasi Retrofit (Java)

```java
import retrofit2.Call;
import retrofit2.http.Body;
import retrofit2.http.GET;
import retrofit2.http.POST;
import java.util.List;

public interface ExcavatorApi {
    @GET("api/slaves")
    Call<List<SlaveDto>> getSlaves();

    @POST("api/command")
    Call<CommandResponse> sendCommand(@Body CommandDto cmd);

    @POST("api/transfer_time")
    Call<StatusDto> transferTime(@Body TransferDto transfer);

    @GET("api/history")
    Call<List<HistoryDto>> getHistory();

    @GET("api/revenue")
    Call<List<RevenueDto>> getRevenue();

    @GET("api/packages")
    Call<List<PackageDto>> getPackages();

    @POST("api/packages/update")
    Call<StatusDto> updatePackage(@Body PackageDto pkg);

    // Fitur Reset
    @POST("api/history/reset")
    Call<StatusDto> resetHistory();

    @POST("api/revenue/reset")
    Call<StatusDto> resetRevenue();

    @POST("api/reset-all")
    Call<StatusDto> resetAll();
}
```

_Tidak perlu lagi interceptor untuk Bearer Token ke ESP32, karena semua autentikasi ditangani dan divalidasi 100% di dalam internal aplikasi Android._
