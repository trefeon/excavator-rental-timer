# Panduan Pengembangan Aplikasi Android

Last Updated: June 3, 2026

## 1. Peran Aplikasi Android

Aplikasi Android bertindak sebagai antarmuka (dashboard) dan pengelola pengguna (user management) secara mandiri. **Master ESP32 TIDAK lagi menangani autentikasi, login, atau manajemen user.**

Tanggung jawab aplikasi Android:

1. **Autentikasi Lokal/Cloud**: Menyimpan data user (SuperAdmin, Admin, Staff) di database lokal Android (misal SQLite/Room) atau cloud (Firebase).
2. **Dashboard Monitoring**: Memanggil `GET /api/slaves` ke Master ESP32 secara berkala (polling) untuk melihat status setiap excavator.
3. **Pengiriman Perintah**: Mengirim `POST /api/command` untuk menambah waktu, pause, resume, dll.
4. **Logika Bisnis (Harga & Histori)**: Master tidak menyimpan data harga, paket, atau riwayat pendapatan. Aplikasi Android **WAJIB** menyimpan tarif harga, mencatat total pemasukan, dan mencatat histori penyewaan alat menggunakan database lokal (seperti Room/SQLite).
5. **Pembatasan Akses (Role Based)**: Menampilkan/menyembunyikan tombol fitur berdasarkan Role user yang sedang login di Android.

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

- `GET /api/slaves`: Menampilkan daftar device, sisa waktu (`time_left`), dan status (`RUNNING`, `LOCKED`, dll).

### Pengiriman Command

Ketika Staff menekan tombol Set 5 Menit di EXC-01:

43: 1. Aplikasi mengirim JSON: `{"id": 1, "cmd": "ADD_TIME", "time": 300}` ke `POST /api/command`.
44: 2. Master mem-forward ke Slave EXC-01.
45: 3. Master merespon dengan status berhasil.

---

## 4. Siklus Pengelolaan (Logika Bisnis di Android)

Karena Master hanya bertindak sebagai jembatan (bridge), aplikasi Android wajib melakukan hal berikut:

1. **Hitung Biaya Sewa:** Ketika pengguna memilih paket 5 menit seharga Rp5.000, aplikasi mencatat pendapatan tersebut ke database lokal.
2. **Kirim Detik ke Master:** Aplikasi mengirimkan 300 detik ke Master via `ADD_TIME`.
3. **Sinkronisasi UI:** Aplikasi membaca `time_left` (sisa waktu) dari Master (via polling) untuk memperbarui tampilan sisa detik di layar HP pengguna.

---

## 5. State Color Mapping

Desain UI di Android direkomendasikan menggunakan mapping warna berikut berdasarkan field `state` dari `/api/slaves`:

| State     | Rekomendasi Warna | Icon |
| --------- | ----------------- | ---- |
| `RUNNING` | Hijau             | ▶    |
| `PAUSED`  | Oranye            | ⏸    |
| `LOCKED`  | Abu-abu           | 🔒   |
| `ENDED`   | Merah             | ⏹    |
| `OFFLINE` | Abu-abu pudar     | ⚫   |

---

## 6. Contoh Konfigurasi Retrofit (Java)

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

    // Manajemen Slave
    @POST("api/edit_slave")
    Call<StatusDto> editSlave(@Body EditSlaveDto edit);

    @POST("api/delete_slave")
    Call<StatusDto> deleteSlave(@Body DeleteSlaveDto delete);
}
```

_Tidak perlu lagi interceptor untuk Bearer Token ke ESP32, karena semua autentikasi ditangani dan divalidasi 100% di dalam internal aplikasi Android._

---

## 7. HTTP Status Codes & Error Handling

Untuk memastikan aplikasi Android tidak _crash_ dan dapat memberikan _feedback_ UI yang jelas (seperti _Toast_ atau _Dialog_), Master ESP32 menerapkan standarisasi HTTP Status Code berikut:

| HTTP Status | Arti | Penanganan di Android |
| :--- | :--- | :--- |
| **`200 OK`** | Sukses. | Lanjutkan proses parsing JSON. |
| **`400 Bad Request`** | Input tidak valid / JSON rusak / ID sudah dipakai. | Tampilkan `response.body().error` ke layar. |
| **`404 Not Found`** | Slave tidak ditemukan di _registry_. | Beritahu admin bahwa Slave dengan ID tersebut belum terdaftar. |
| **`502 Bad Gateway`** | Slave sedang _Offline_ atau koneksi radio terputus. | Master ESP32 online, tetapi gagal mem-_forward_ instruksi ke Slave. Tampilkan indikator "Slave Offline". |
| **`503 Service Unavailable`** | Master ESP32 sedang sibuk. | Terjadi _race condition_ di sisi Master. Aplikasi Android dapat melakukan _Auto-Retry_ 1-2 detik kemudian. |

Bentuk baku untuk balasan error (Status 4xx dan 5xx) adalah:

```json
{
  "ok": 0,
  "error": "Pesan error spesifik dari sistem"
}
```

---

## 8. API Reference & DTO Schemas

Berikut adalah detail struktur JSON (_Request_ / _Response_) untuk setiap _endpoint_ beserta rekomendasi kelas model DTO di Java/Kotlin. Sebagai tambahan, untuk memudahkan _testing_, kami telah menyediakan:

1. **OpenAPI/Swagger Spec**: Tersedia di file `docs/openapi.yaml`.
2. **Postman Collection**: Tersedia di file `docs/Excavator_API_Postman_Collection.json`. _Import_ file ini ke Postman untuk langsung mencoba semua API tanpa harus _coding_ terlebih dahulu.

### 1. Daftar Slaves (`GET /api/slaves`)

**Response:**

```json
[
  {
    "id": 1,
    "ip": "192.168.4.2",
    "mac": "48:3F:DA:00:11:22",
    "name": "EXC-01",
    "state": "RUNNING",
    "time_left": 3562,
    "last_seen": 2
  }
]
```

**Java DTO:**

```java
public class SlaveDto {
    public int id;
    public String ip;
    public String mac;
    public String name;
    public String state; // "LOCKED", "RUNNING", "PAUSED", "ENDED", "OFFLINE"
    public int time_left;      // Sisa waktu dalam detik
    public int last_seen;// Detik berlalu sejak slave terakhir merespon
}
```

### 2. Kirim Perintah (`POST /api/command`)

**Request Body:**

```json
{
  "id": 1,
  "cmd": "ADD_TIME",
  "time": 300
}
```

_Note: `time` untuk `ADD_TIME` adalah dalam format **detik** (misal 300 untuk 5 menit)._

**Response:**

```json
{
  "ok": 1,
  "code": "OK",
  "time_left": 3600,
  "state": "RUNNING"
}
```

**Java DTO:**

```java
public class CommandDto {
    public int id;
    public String cmd; // "ADD_TIME", "PAUSE", "RESUME", "STOP", "IDENTIFY", "REBOOT"
    public int time;    // Parameter nilai (contoh: detik untuk ADD_TIME, 0 untuk lainnya)
}

public class CommandResponse {
    public int ok;
    public String code;
    public int time_left;
    public String state;
}
```

### 3. Manajemen Slave (`edit_slave` & `delete_slave`)

Digunakan untuk mengubah pengaturan Unit/Slave (mac address) di Master.

**`POST /api/edit_slave` Request Body:**

```json
{
  "mac": "24:6F:28:XX:XX:XX",
  "id": 2
}
```

_(Response berupa `{ "ok": 1 }`)_

**`POST /api/delete_slave` Request Body:**

```json
{
  "mac": "24:6F:28:XX:XX:XX"
}
```

_(Response berupa `{ "ok": 1 }`)_

**Java DTOs:**

```java
public class EditSlaveDto {
    public String mac;
    public int id; // ID / Nomor urut baru
}

public class DeleteSlaveDto {
    public String mac;
}
```

### 4. Standard Success Response

Untuk _endpoint_ seperti `/api/edit_slave` atau `/api/delete_slave`.

**Response:**

```json
{
  "ok": 1
}
```

**Java DTO:**

```java
public class StatusDto {
    public int ok;
    public String message; // (Opsional)
}
```

---

## 8. Contoh Implementasi Kode (Java/Android)

Bagian ini berisi contoh kode konkrit untuk memanggil API Master ESP32 dari Android.

### A. Konfigurasi Retrofit Client

Gunakan _timeout_ yang cukup (misalnya 10-15 detik) karena Master harus mem-_forward_ instruksi ke Slave melalui radio WiFi (bisa sedikit tertunda).

```java
import java.util.concurrent.TimeUnit;
import okhttp3.OkHttpClient;
import retrofit2.Retrofit;
import retrofit2.converter.gson.GsonConverterFactory;

public class ApiClient {
    private static final String BASE_URL = "http://192.168.4.1/"; // Pastikan diakhiri slash (/)
    private static Retrofit retrofit = null;

    public static ExcavatorApi getApi() {
        if (retrofit == null) {
            OkHttpClient client = new OkHttpClient.Builder()
                    .connectTimeout(15, TimeUnit.SECONDS)
                    .readTimeout(15, TimeUnit.SECONDS)
                    .build();

            retrofit = new Retrofit.Builder()
                    .baseUrl(BASE_URL)
                    .client(client)
                    .addConverterFactory(GsonConverterFactory.create())
                    .build();
        }
        return retrofit.create(ExcavatorApi.class);
    }
}
```

### B. Memanggil Data Slaves (Dashboard Polling)

Dashboard harus me-`refresh` data `getSlaves` setiap beberapa detik secara asinkron.

```java
public void fetchDashboardData() {
    ApiClient.getApi().getSlaves().enqueue(new retrofit2.Callback<List<SlaveDto>>() {
        @Override
        public void onResponse(Call<List<SlaveDto>> call, retrofit2.Response<List<SlaveDto>> response) {
            if (response.isSuccessful() && response.body() != null) {
                List<SlaveDto> slaves = response.body();
                // Update UI RecyclerView/Adapter Anda di sini
                // slaves.get(0).state -> "RUNNING", dll
            }
        }

        @Override
        public void onFailure(Call<List<SlaveDto>> call, Throwable t) {
            // Tangani error, tampilkan indikator "Master Offline"
        }
    });
}
```

### C. Menambah Waktu (Command ADD_TIME)

Ingat, Master hanya menerima nilai dalam **detik**.

```java
public void addTimeForExcavator(int excavatorId, int durationSeconds) {
    CommandDto cmd = new CommandDto();
    cmd.id = excavatorId;
    cmd.cmd = "ADD_TIME";
    cmd.time = durationSeconds; // Contoh: 300 untuk 5 menit

    ApiClient.getApi().sendCommand(cmd).enqueue(new retrofit2.Callback<CommandResponse>() {
        @Override
        public void onResponse(Call<CommandResponse> call, retrofit2.Response<CommandResponse> response) {
            if (response.isSuccessful() && response.body() != null) {
                if (response.body().ok == 1) {
                    // Sukses menambah waktu
                    // Response body.time_left akan berisi sisa waktu terbaru dalam detik
                } else {
                    // Tampilkan pesan error dari response.body().code
                }
            } else {
                // Tampilkan pesan kegagalan (misal Slave Offline)
            }
        }

        @Override
        public void onFailure(Call<CommandResponse> call, Throwable t) {
            // Tampilkan error (koneksi terputus/timeout)
        }
    });
}
```
