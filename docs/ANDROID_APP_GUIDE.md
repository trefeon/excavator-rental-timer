# Panduan Integrasi Android — Excavator Rental Timer API

Last Updated: June 4, 2026

---

## 1. Prinsip Arsitektur

```text
┌─────────────────────┐          ┌──────────────┐          ┌──────────────┐
│  Aplikasi Android   │──HTTP──► │ Master ESP32 │──HTTP──► │ Slave ESP32  │
│                     │◄──JSON── │ (Bridge API) │◄──JSON── │ (Timer HW)   │
│  ● User Auth        │          │              │          │              │
│  ● Pricing/Tarif    │          │ HANYA proxy  │          │ ● Relay      │
│  ● History/Revenue  │          │ Tidak simpan │          │ ● TM1637     │
│  ● Role-based UI    │          │ data bisnis  │          │ ● Buzzer     │
│  ● Semua kalkulasi  │          │              │          │ ● Timer      │
└─────────────────────┘          └──────────────┘          └──────────────┘
```

### Tanggung Jawab Masing-Masing Layer

| Layer | Tanggung Jawab |
|---|---|
| **Android App** | Auth lokal/cloud, harga & paket, histori sewa, revenue, konversi menit→detik, format display, role-based access |
| **Master ESP32** | Bridge/proxy saja: forward command ke slave, polling state dari slave, registry MAC↔ID |
| **Slave ESP32** | Timer countdown, relay ON/OFF, display TM1637, buzzer, powerloss recovery, simpan state ke NVS |

> **KUNCI:** Semua value dari API adalah **RAW** (mentah). Tidak ada kalkulasi apapun. Aplikasi Android bertanggung jawab 100% atas semua logika bisnis.

---

## 2. Koneksi ke Master

| Parameter | Nilai |
|---|---|
| Wi-Fi SSID | `ExcavatorMaster` |
| Password | `12345678` |
| Base URL | `http://192.168.4.1` |
| Protocol | HTTP (cleartext) |

**Wajib di `AndroidManifest.xml`:**
```xml
<application android:usesCleartextTraffic="true" ...>
```

---

## 3. API Endpoints Lengkap

Semua endpoint **open access** (tanpa token/auth). Auth dikelola 100% di internal Android.

### 3.1 `GET /api/slaves` — Daftar Semua Unit

Polling endpoint utama. Panggil setiap **3-5 detik**.

**Response (`200 OK`):**
```json
[
  {
    "id": 1,
    "ip": "192.168.4.2",
    "mac": "48:3F:DA:00:11:22",
    "online": true,
    "state": "RUNNING",
    "time_left": 287,
    "battery": "OK"
  },
  {
    "id": 2,
    "ip": "192.168.4.3",
    "mac": "48:3F:DA:00:33:44",
    "online": false,
    "state": "LOCKED",
    "time_left": 0,
    "battery": "OK"
  }
]
```

#### Field Dictionary

| Field | Type | Deskripsi | Catatan untuk Android |
|---|---|---|---|
| `id` | `int` | Nomor urut unit (1-50) | Format tampilan: `String.format("EXC-%02d", id)` |
| `ip` | `string` | IP address slave di jaringan | Untuk debugging saja, tidak perlu ditampilkan ke user |
| `mac` | `string` | MAC address hardware (format `XX:XX:XX:XX:XX:XX`) | Digunakan saat edit/delete slave |
| `online` | `boolean` | `true` jika slave merespon dalam 30 detik terakhir | Gunakan untuk indikator koneksi |
| `state` | `string` | Status unit saat ini | Lihat tabel State di bawah |
| `time_left` | `int` | Sisa waktu dalam **detik** (raw) | Android harus format sendiri: `time_left / 60` = menit, `time_left % 60` = detik |
| `battery` | `string` | Status baterai | Saat ini selalu `"OK"` (hardcoded, belum ada sensor) |

> **⚠️ Field yang TIDAK ada:** `name` (format sendiri dari `id`), `last_seen` (gunakan `online` boolean saja).

---

### 3.2 `POST /api/command` — Kirim Perintah ke Unit

**Request Body:**
```json
{
  "id": 1,
  "cmd": "ADD_TIME",
  "time": 300
}
```

#### Request Fields

| Field | Type | Wajib | Deskripsi |
|---|---|---|---|
| `id` | `int` | ✅ | Nomor unit target (dari `GET /api/slaves`) |
| `cmd` | `string` | ✅ | Command yang dikirim (lihat tabel di bawah) |
| `time` | `int` | Untuk `ADD_TIME` saja | Jumlah waktu dalam **detik** |

#### Daftar Commands

| Command | `time` | Kapan Digunakan |
|---|---|---|
| `ADD_TIME` | 1 - 28800 | Menambah waktu sewa (dalam detik). Contoh: 300 = 5 menit |
| `PAUSE` | 0 | Jeda timer (relay OFF, countdown berhenti) |
| `RESUME` | 0 | Lanjutkan timer yang di-pause |
| `STOP` | 0 | Reset waktu ke 0 & kunci relay |
| `IDENTIFY` | 0 | Buzzer 3x + display kedip (untuk mencari unit fisik) |
| `REBOOT` | 0 | Restart ESP32 slave |

> **PENTING:** Satuan `time` selalu **DETIK**. Jika Android menampilkan pilihan "5 menit", kirim `time: 300`.

**Response Sukses (`200 OK`):**
```json
{
  "ok": 1,
  "code": "OK",
  "time_left": 587,
  "state": "RUNNING"
}
```

**Response Gagal (contoh: state tidak sesuai):**
```json
{
  "ok": 0,
  "code": "BAD_STATE",
  "time_left": 0,
  "state": "LOCKED"
}
```

#### Response Fields

| Field | Type | Deskripsi |
|---|---|---|
| `ok` | `int` | `1` = berhasil, `0` = gagal |
| `code` | `string` | Kode status (lihat tabel error codes) |
| `time_left` | `int` | Sisa waktu terbaru dalam **detik** |
| `state` | `string` | State terbaru setelah command |

#### Error Codes dari Slave

| Code | Arti | Tindakan Android |
|---|---|---|
| `OK` | Berhasil | Update UI |
| `BAD_STATE` | Command tidak valid untuk state saat ini (misal PAUSE saat LOCKED) | Tampilkan toast "Unit tidak dalam kondisi yang sesuai" |
| `EXCEEDS_LIMIT` | ADD_TIME melebihi batas max (480 menit) | Tampilkan toast "Waktu melebihi batas maksimum" |
| `UNKNOWN_COMMAND` | Command tidak dikenali | Bug — cek kode Android |
| `BAD_FORMAT` | Body request bukan JSON | Bug — cek request format |
| `BAD_JSON` | JSON parse error | Bug — cek JSON syntax |

---

### 3.3 `POST /api/edit_slave` — Ubah ID Unit

**Request Body:**
```json
{
  "mac": "48:3F:DA:00:11:22",
  "id": 5
}
```

**Response:** `{"ok": 1}` atau `{"ok": 0, "error": "ID Taken"}`

---

### 3.4 `POST /api/delete_slave` — Hapus Unit dari Registry

**Request Body:**
```json
{
  "mac": "48:3F:DA:00:11:22"
}
```

**Response:** `{"ok": 1}`

---

## 4. HTTP Status Codes

| HTTP Status | Arti | Penanganan Android |
|---|---|---|
| `200 OK` | Sukses | Parse JSON response |
| `400 Bad Request` | Input tidak valid / JSON rusak / ID sudah dipakai | Tampilkan `error` field ke user |
| `404 Not Found` | Slave tidak ditemukan di registry | "Unit belum terdaftar" |
| `502 Bad Gateway` | Slave offline / koneksi radio terputus | "Unit tidak merespon (offline)" |
| `503 Service Unavailable` | Master sibuk (race condition) | Auto-retry setelah 1-2 detik |

---

## 5. State Machine & Color Mapping

| State | Warna UI | Icon | Relay | Display | Deskripsi |
|---|---|---|---|---|---|
| `LOCKED` | Abu-abu | 🔒 | OFF | `----` | Standby, tidak ada sesi |
| `RUNNING` | Hijau | ▶ | ON | `MM:SS` | Countdown aktif |
| `PAUSED` | Oranye | ⏸ | OFF | `MM:SS` | Timer dijeda |
| `ENDED` | Merah | ⏹ | OFF | `----` | Waktu habis |
| `OFFLINE` | Abu-abu pudar | ⚫ | - | - | Unit tidak merespon (field `online: false`) |

> **Catatan:** `OFFLINE` bukan state dari firmware. Ini disimpulkan dari field `online == false` di response `GET /api/slaves`.

---

## 6. Yang WAJIB Di-handle Android (Bukan Firmware)

| Fitur | Tanggung Jawab | Penjelasan |
|---|---|---|
| **Nama Unit** | Android | Format dari `id`: `String.format("EXC-%02d", id)` |
| **Display Waktu** | Android | Format dari `time_left`: `String.format("%02d:%02d", time_left/60, time_left%60)` |
| **Harga/Tarif** | Android | Simpan di Room/SQLite. Master tidak tahu soal harga |
| **Histori Sewa** | Android | Catat setiap transaksi ADD_TIME di database lokal |
| **Revenue/Pendapatan** | Android | Hitung dari histori sewa × tarif |
| **User Auth** | Android | SuperAdmin / Admin / Staff — simpan di Room atau Firebase |
| **Role-Based UI** | Android | Tampilkan/sembunyikan tombol berdasarkan role |
| **Konversi Waktu** | Android | User pilih "5 menit" → Android kirim `time: 300` |

---

## 7. DTO Classes (Java)

```java
// === Response dari GET /api/slaves ===
public class SlaveDto {
    public int id;           // Nomor unit (1-50)
    public String ip;        // IP address slave
    public String mac;       // MAC address hardware
    public boolean online;   // true = unit merespon
    public String state;     // "LOCKED", "RUNNING", "PAUSED", "ENDED"
    public int time_left;    // Sisa waktu dalam DETIK
    public String battery;   // "OK" (hardcoded)

    /** Helper: format nama unit */
    public String getDisplayName() {
        return String.format("EXC-%02d", id);
    }

    /** Helper: format sisa waktu MM:SS */
    public String getDisplayTime() {
        return String.format("%02d:%02d", time_left / 60, time_left % 60);
    }
}

// === Request body untuk POST /api/command ===
public class CommandDto {
    public int id;       // Nomor unit target
    public String cmd;   // "ADD_TIME", "PAUSE", "RESUME", "STOP", "IDENTIFY", "REBOOT"
    public int time;     // Detik (hanya untuk ADD_TIME, 0 untuk lainnya)
}

// === Response dari POST /api/command ===
public class CommandResponse {
    public int ok;           // 1 = sukses, 0 = gagal
    public String code;      // "OK", "BAD_STATE", "EXCEEDS_LIMIT", dll
    public int time_left;    // Sisa waktu terbaru (detik)
    public String state;     // State terbaru
}

// === Request body untuk POST /api/edit_slave ===
public class EditSlaveDto {
    public String mac;   // MAC address unit yang mau diubah
    public int id;       // ID baru
}

// === Request body untuk POST /api/delete_slave ===
public class DeleteSlaveDto {
    public String mac;   // MAC address unit yang mau dihapus
}

// === Generic response untuk edit/delete ===
public class StatusDto {
    public int ok;          // 1 = sukses, 0 = gagal
    public String error;    // Pesan error (opsional)
}
```

---

## 8. Retrofit Interface

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

    @POST("api/edit_slave")
    Call<StatusDto> editSlave(@Body EditSlaveDto edit);

    @POST("api/delete_slave")
    Call<StatusDto> deleteSlave(@Body DeleteSlaveDto dto);
}
```

---

## 9. Retrofit Client Configuration

```java
import java.util.concurrent.TimeUnit;
import okhttp3.OkHttpClient;
import retrofit2.Retrofit;
import retrofit2.converter.gson.GsonConverterFactory;

public class ApiClient {
    private static final String BASE_URL = "http://192.168.4.1/";
    private static Retrofit retrofit = null;

    public static ExcavatorApi getApi() {
        if (retrofit == null) {
            OkHttpClient client = new OkHttpClient.Builder()
                    .connectTimeout(10, TimeUnit.SECONDS)
                    .readTimeout(10, TimeUnit.SECONDS)
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

---

## 10. Contoh Implementasi

### A. Dashboard Polling (setiap 3 detik)

```java
private Handler handler = new Handler();
private Runnable pollRunnable = new Runnable() {
    @Override
    public void run() {
        ApiClient.getApi().getSlaves().enqueue(new Callback<List<SlaveDto>>() {
            @Override
            public void onResponse(Call<List<SlaveDto>> call, Response<List<SlaveDto>> response) {
                if (response.isSuccessful() && response.body() != null) {
                    List<SlaveDto> slaves = response.body();
                    // Update RecyclerView adapter
                    adapter.updateData(slaves);
                }
                handler.postDelayed(pollRunnable, 3000);
            }

            @Override
            public void onFailure(Call<List<SlaveDto>> call, Throwable t) {
                // Tampilkan indikator "Master Offline"
                showMasterOffline();
                handler.postDelayed(pollRunnable, 5000); // retry lebih lambat
            }
        });
    }
};

// Start polling
handler.post(pollRunnable);
```

### B. Menambah Waktu (5 menit = 300 detik)

```java
public void addTime(int excavatorId, int durationMinutes, int priceRupiah) {
    // 1. Konversi menit ke detik
    int seconds = durationMinutes * 60;

    // 2. Kirim ke firmware
    CommandDto cmd = new CommandDto();
    cmd.id = excavatorId;
    cmd.cmd = "ADD_TIME";
    cmd.time = seconds;

    ApiClient.getApi().sendCommand(cmd).enqueue(new Callback<CommandResponse>() {
        @Override
        public void onResponse(Call<CommandResponse> call, Response<CommandResponse> res) {
            if (res.isSuccessful() && res.body() != null && res.body().ok == 1) {
                // 3. Catat revenue di database lokal Android
                db.insertRentalRecord(excavatorId, durationMinutes, priceRupiah);
                Toast.makeText(ctx, "Waktu ditambahkan!", Toast.LENGTH_SHORT).show();
            } else {
                String errorCode = res.body() != null ? res.body().code : "UNKNOWN";
                Toast.makeText(ctx, "Gagal: " + errorCode, Toast.LENGTH_SHORT).show();
            }
        }

        @Override
        public void onFailure(Call<CommandResponse> call, Throwable t) {
            Toast.makeText(ctx, "Koneksi terputus", Toast.LENGTH_SHORT).show();
        }
    });
}
```

---

## 11. Testing Tools

- **OpenAPI Spec**: `docs/openapi.yaml` — import ke Swagger UI untuk interactive docs
- **Postman Collection**: `docs/Excavator_API_Postman_Collection.json` — import ke Postman untuk test manual
