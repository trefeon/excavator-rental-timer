# Android Integration Guide — Excavator Rental Timer API

Last Updated: June 4, 2026

---

## 1. Architectural Principles

```text
┌─────────────────────┐          ┌──────────────┐          ┌──────────────┐
│  Android App        │──HTTP──► │ Master ESP32 │──HTTP──► │ Slave ESP32  │
│                     │◄──JSON── │ (Bridge API) │◄──JSON── │ (Timer HW)   │
│  ● User Auth        │          │              │          │              │
│  ● Pricing/Tariff   │          │ ONLY proxy   │          │ ● Relay      │
│  ● History/Revenue  │          │ No business  │          │ ● TM1637     │
│  ● Role-based UI    │          │ data stored  │          │ ● Buzzer     │
│  ● All calculations │          │              │          │ ● Timer      │
└─────────────────────┘          └──────────────┘          └──────────────┘
```

### Layer Responsibilities

| Layer | Responsibility |
|---|---|
| **Android App** | Local/cloud auth, prices & packages, rental history, revenue, minutes→seconds conversion, display formatting, role-based access |
| **Master ESP32** | Bridge/proxy only: forwards commands to slaves, polls state from slaves, MAC↔ID registry |
| **Slave ESP32** | Timer countdown, relay ON/OFF, TM1637 display, buzzer, powerloss recovery, save state to NVS |

> **KEY:** All values from the API are **RAW**. There are no calculations whatsoever. The Android App is 100% responsible for all business logic.

---

## 2. Connection to Master

| Parameter | Value |
|---|---|
| Wi-Fi SSID | `ExcavatorMaster` |
| Password | `12345678` |
| Base URL | `http://192.168.4.1` |
| Protocol | HTTP (cleartext) |

**Required in `AndroidManifest.xml`:**
```xml
<application android:usesCleartextTraffic="true" ...>
```

---

## 3. Full API Endpoints

All endpoints are **open access** (no token/auth). Auth is 100% managed internally by Android.

### 3.1 `GET /api/slaves` — List All Units

Primary polling endpoint. Call every **3-5 seconds**.

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

| Field | Type | Description | Notes for Android |
|---|---|---|---|
| `id` | `int` | Unit sequence number (1-50) | Display format: `String.format("EXC-%02d", id)` |
| `ip` | `string` | Slave IP address on network | For debugging only, no need to show to user |
| `mac` | `string` | Hardware MAC address (`XX:XX:XX:XX:XX:XX`) | Used when editing/deleting slaves |
| `online` | `boolean` | `true` if slave responded in last 30s | Use for connection indicator |
| `state` | `string` | Current unit status | See State table below |
| `time_left` | `int` | Remaining time in **seconds** (raw) | Android must format itself: `time_left / 60` = mins, `time_left % 60` = secs |
| `battery` | `string` | Battery status | Currently always `"OK"` (hardcoded, no sensor yet) |

> **⚠️ Fields NOT present:** `name` (format yourself from `id`), `last_seen` (just use `online` boolean).

---

### 3.2 `POST /api/command` — Send Command to Unit

**Request Body:**
```json
{
  "id": 1,
  "cmd": "ADD_TIME",
  "time": 300
}
```

#### Request Fields

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | `int` | ✅ | Target unit number (from `GET /api/slaves`) |
| `cmd` | `string` | ✅ | Command sent (see table below) |
| `time` | `int` | `ADD_TIME` only | Time amount in **seconds** |

#### Command List

| Command | `time` | When Used |
|---|---|---|
| `ADD_TIME` | 1 - 28800 | Add rental time (in seconds). Example: 300 = 5 minutes |
| `PAUSE` | 0 | Pause timer (relay OFF, countdown stops) |
| `RESUME` | 0 | Resume paused timer |
| `STOP` | 0 | Reset time to 0 & lock relay |
| `IDENTIFY` | 0 | Buzzer 3x + display blink (to find physical unit) |
| `REBOOT` | 0 | Restart ESP32 slave |

> **IMPORTANT:** `time` unit is always **SECONDS**. If Android shows a "5 minutes" option, send `time: 300`.

**Success Response (`200 OK`):**
```json
{
  "ok": 1,
  "code": "OK",
  "time_left": 587,
  "state": "RUNNING"
}
```

**Failed Response (example: invalid state):**
```json
{
  "ok": 0,
  "code": "BAD_STATE",
  "time_left": 0,
  "state": "LOCKED"
}
```

#### Response Fields

| Field | Type | Description |
|---|---|---|
| `ok` | `int` | `1` = success, `0` = failed |
| `code` | `string` | Status code (see error codes table) |
| `time_left` | `int` | Latest remaining time in **seconds** |
| `state` | `string` | Latest state after command |

#### Slave Error Codes

| Code | Meaning | Android Action |
|---|---|---|
| `OK` | Success | Update UI |
| `BAD_STATE` | Invalid command for current state (e.g. PAUSE when LOCKED) | Show toast "Unit not in a suitable state" |
| `EXCEEDS_LIMIT` | ADD_TIME exceeds max limit (480 mins) | Show toast "Time exceeds maximum limit" |
| `UNKNOWN_COMMAND` | Unrecognized command | Bug — check Android code |
| `BAD_FORMAT` | Request body is not JSON | Bug — check request format |
| `BAD_JSON` | JSON parse error | Bug — check JSON syntax |

---

### 3.3 `POST /api/edit_slave` — Change Unit ID

**Request Body:**
```json
{
  "mac": "48:3F:DA:00:11:22",
  "id": 5
}
```

**Response:** `{"ok": 1}` or `{"ok": 0, "error": "ID Taken"}`

---

### 3.4 `POST /api/delete_slave` — Delete Unit from Registry

**Request Body:**
```json
{
  "mac": "48:3F:DA:00:11:22"
}
```

**Response:** `{"ok": 1}`

---

## 4. HTTP Status Codes

| HTTP Status | Meaning | Android Handling |
|---|---|---|
| `200 OK` | Success | Parse JSON response |
| `400 Bad Request` | Invalid input / malformed JSON / ID taken | Show `error` field to user |
| `404 Not Found` | Slave not found in registry | "Unit is not registered yet" |
| `502 Bad Gateway` | Slave offline / radio connection dropped | "Unit is not responding (offline)" |
| `503 Service Unavailable` | Master busy (race condition) | Auto-retry after 1-2 seconds |

---

## 5. State Machine & Color Mapping

| State | UI Color | Icon | Relay | Display | Description |
|---|---|---|---|---|---|
| `LOCKED` | Gray | 🔒 | OFF | `----` | Standby, no session |
| `RUNNING` | Green | ▶ | ON | `MM:SS` | Countdown active |
| `PAUSED` | Orange | ⏸ | OFF | `MM:SS` | Timer paused |
| `ENDED` | Red | ⏹ | OFF | `----` | Time is up |
| `OFFLINE` | Faded gray | ⚫ | - | - | Unit not responding (`online: false` field) |

> **Note:** `OFFLINE` is not a firmware state. It is deduced from the `online == false` field in the `GET /api/slaves` response.

---

## 6. MUST Be Handled by Android (Not Firmware)

| Feature | Responsibility | Explanation |
|---|---|---|
| **Unit Name** | Android | Format from `id`: `String.format("EXC-%02d", id)` |
| **Time Display** | Android | Format from `time_left`: `String.format("%02d:%02d", time_left/60, time_left%60)` |
| **Price/Tariff** | Android | Save in Room/SQLite. Master knows nothing about prices |
| **Rental History**| Android | Log every ADD_TIME transaction in local database |
| **Revenue** | Android | Calculate from rental history × tariff |
| **User Auth** | Android | SuperAdmin / Admin / Staff — save in Room or Firebase |
| **Role-Based UI** | Android | Show/hide buttons based on role |
| **Time Conversion**| Android | User selects "5 mins" → Android sends `time: 300` |

---

## 7. DTO Classes (Java)

```java
// === Response from GET /api/slaves ===
public class SlaveDto {
    public int id;           // Unit number (1-50)
    public String ip;        // Slave IP address
    public String mac;       // Hardware MAC address
    public boolean online;   // true = unit responding
    public String state;     // "LOCKED", "RUNNING", "PAUSED", "ENDED"
    public int time_left;    // Remaining time in SECONDS
    public String battery;   // "OK" (hardcoded)

    /** Helper: format unit name */
    public String getDisplayName() {
        return String.format("EXC-%02d", id);
    }

    /** Helper: format remaining time MM:SS */
    public String getDisplayTime() {
        return String.format("%02d:%02d", time_left / 60, time_left % 60);
    }
}

// === Request body for POST /api/command ===
public class CommandDto {
    public int id;       // Target unit number
    public String cmd;   // "ADD_TIME", "PAUSE", "RESUME", "STOP", "IDENTIFY", "REBOOT"
    public int time;     // Seconds (only for ADD_TIME, 0 for others)
}

// === Response from POST /api/command ===
public class CommandResponse {
    public int ok;           // 1 = success, 0 = failed
    public String code;      // "OK", "BAD_STATE", "EXCEEDS_LIMIT", etc
    public int time_left;    // Latest remaining time (seconds)
    public String state;     // Latest state
}

// === Request body for POST /api/edit_slave ===
public class EditSlaveDto {
    public String mac;   // MAC address of unit to modify
    public int id;       // New ID
}

// === Request body for POST /api/delete_slave ===
public class DeleteSlaveDto {
    public String mac;   // MAC address of unit to delete
}

// === Generic response for edit/delete ===
public class StatusDto {
    public int ok;          // 1 = success, 0 = failed
    public String error;    // Error message (optional)
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

## 10. Implementation Example

### A. Dashboard Polling (every 3 seconds)

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
                // Show "Master Offline" indicator
                showMasterOffline();
                handler.postDelayed(pollRunnable, 5000); // retry slower
            }
        });
    }
};

// Start polling
handler.post(pollRunnable);
```

### B. Add Time (5 minutes = 300 seconds)

```java
public void addTime(int excavatorId, int durationMinutes, int priceRupiah) {
    // 1. Convert minutes to seconds
    int seconds = durationMinutes * 60;

    // 2. Send to firmware
    CommandDto cmd = new CommandDto();
    cmd.id = excavatorId;
    cmd.cmd = "ADD_TIME";
    cmd.time = seconds;

    ApiClient.getApi().sendCommand(cmd).enqueue(new Callback<CommandResponse>() {
        @Override
        public void onResponse(Call<CommandResponse> call, Response<CommandResponse> res) {
            if (res.isSuccessful() && res.body() != null && res.body().ok == 1) {
                // 3. Log revenue in local Android database
                db.insertRentalRecord(excavatorId, durationMinutes, priceRupiah);
                Toast.makeText(ctx, "Time added!", Toast.LENGTH_SHORT).show();
            } else {
                String errorCode = res.body() != null ? res.body().code : "UNKNOWN";
                Toast.makeText(ctx, "Failed: " + errorCode, Toast.LENGTH_SHORT).show();
            }
        }

        @Override
        public void onFailure(Call<CommandResponse> call, Throwable t) {
            Toast.makeText(ctx, "Connection lost", Toast.LENGTH_SHORT).show();
        }
    });
}
```

---

## 11. Testing Tools

- **OpenAPI Spec**: `docs/openapi.yaml` — import to Swagger UI for interactive docs
- **Postman Collection**: `docs/Excavator_API_Postman_Collection.json` — import to Postman for manual testing
