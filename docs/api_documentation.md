# Dokumentasi API - Excavator Rental Timer

Sistem Excavator Rental Timer terdiri dari dua komponen utama yang berkomunikasi melalui jaringan Wi-Fi lokal: **Master** (Access Point) dan **Slave** (Client). Berikut adalah dokumentasi lengkap untuk semua endpoint API yang tersedia pada Master dan Slave.

---

## 1. API Master (Access Point)
Master berjalan sebagai Access Point dengan IP default `192.168.4.1`. Master bertugas mengelola *registry* (daftar slave) dan menjadi proxy pengirim perintah ke Slave.

### 1.1. `GET /api/slaves`
Mengambil daftar semua slave (excavator) yang terdaftar di sistem beserta status real-time mereka (terakhir diambil dari polling).

- **Method:** `GET`
- **URL:** `http://192.168.4.1/api/slaves`
- **Response (200 OK):**
  ```json
  [
    {
      "id": 1,
      "ip": "192.168.4.2",
      "mac": "80:F3:DA:63:25:DC",
      "online": true,
      "state": "RUNNING",
      "rem": 300,
      "disp": "05:00",
      "paid": 300,
      "bat": "OK"
    }
  ]
  ```

### 1.2. `GET /api/register`
Digunakan oleh Slave untuk melakukan registrasi otomatis ke Master saat baru terhubung ke jaringan Wi-Fi. Master akan merespons dengan memberikan ID untuk slave tersebut.

- **Method:** `GET`
- **URL:** `http://192.168.4.1/api/register?mac={MAC_ADDRESS}`
- **Query Params:**
  - `mac` (String, Required): MAC Address dari Slave, format `XX:XX:XX:XX:XX:XX`
- **Response (200 OK):**
  ```json
  {
    "id": 1
  }
  ```
- **Error Responses:**
  - `400 Bad Request`: Jika MAC tidak disertakan atau formatnya tidak valid.
  - `503 Service Unavailable`: Jika server sedang sibuk (mutex timeout).

### 1.3. `POST /api/command`
Mengirim perintah ke slave tertentu. Master bertindak sebagai *proxy* dan akan meneruskan perintah ini ke IP slave yang bersangkutan.

- **Method:** `POST`
- **URL:** `http://192.168.4.1/api/command`
- **Headers:** `Content-Type: application/json`
- **Body Request:**
  ```json
  {
    "id": 1,
    "cmd": "ADD_TIME",
    "val": 300
  }
  ```
  - `id` (Number): ID dari slave yang dituju.
  - `cmd` (String): Perintah yang dikirim. Pilihan: `ADD_TIME`, `PAUSE`, `RESUME`, `STOP`, `IDENTIFY`, `REBOOT`.
  - `val` (Number): Nilai yang diperlukan untuk perintah tertentu (contoh: jumlah detik untuk `ADD_TIME`). Default `0`.
- **Response:** 
  Mengembalikan response langsung dari slave.
- **Error Responses:**
  - `404 Not Found`: Jika slave dengan ID tersebut tidak ditemukan atau tidak memiliki IP.
  - `502 Bad Gateway`: Jika slave offline atau timeout saat dihubungi.

### 1.4. `POST /api/transfer_time`
Mentransfer seluruh sisa waktu (*remaining time*) dari satu slave ke slave lainnya. Slave sumber akan dihentikan (*STOP*), dan sisa waktunya dipindahkan ke slave target.

- **Method:** `POST`
- **URL:** `http://192.168.4.1/api/transfer_time`
- **Headers:** `Content-Type: application/json`
- **Body Request:**
  ```json
  {
    "from_id": 1,
    "to_id": 2
  }
  ```
- **Response (200 OK):**
  ```json
  {
    "ok": 1
  }
  ```

### 1.5. `POST /api/edit_slave`
Mengubah ID yang dialokasikan untuk sebuah slave (berdasarkan MAC address).

- **Method:** `POST`
- **URL:** `http://192.168.4.1/api/edit_slave`
- **Headers:** `Content-Type: application/json`
- **Body Request:**
  ```json
  {
    "mac": "80:F3:DA:63:25:DC",
    "id": 5
  }
  ```
- **Response (200 OK):**
  ```json
  {
    "ok": 1
  }
  ```

### 1.6. `POST /api/delete_slave`
Menghapus data slave dari *registry* Master.

- **Method:** `POST`
- **URL:** `http://192.168.4.1/api/delete_slave`
- **Headers:** `Content-Type: application/json`
- **Body Request:**
  ```json
  {
    "mac": "80:F3:DA:63:25:DC"
  }
  ```
- **Response (200 OK):**
  ```json
  {
    "ok": 1
  }
  ```

---

## 2. API Slave (Client)
Setiap Slave (excavator) memiliki web server internal yang dapat dipanggil langsung, baik oleh Master saat *polling* atau sebagai penerima *proxy command*.

### 2.1. `GET /api/state`
Mengambil status real-time dari excavator. Ini dipanggil oleh Master secara periodik (polling) untuk memperbarui status di dashboard.

- **Method:** `GET`
- **URL:** `http://{SLAVE_IP}/api/state`
- **Response (200 OK):**
  ```json
  {
    "toy": "EXC-01",
    "state": "RUNNING",
    "rem": 300,
    "disp": "05:00",
    "paid": 300,
    "bat": "OK",
    "fault": 0,
    "seq": 1
  }
  ```
  - `toy`: Nama/ID alat.
  - `state`: Status alat (`LOCKED`, `RUNNING`, `PAUSED`, `ENDED`, `FAULT`).
  - `rem`: Sisa waktu dalam detik.
  - `disp`: Teks yang muncul di layar 7-segment.
  - `paid`: Total waktu yang telah ditambahkan.
  - `seq`: Sequence number (berubah saat status berubah).

### 2.2. `POST /api/command`
Mengeksekusi perintah langsung pada Slave. Endpoint ini sama dengan endpoint proxy di Master, namun parameter `id` bersifat opsional di sini.

- **Method:** `POST`
- **URL:** `http://{SLAVE_IP}/api/command`
- **Headers:** `Content-Type: application/json`
- **Body Request:**
  ```json
  {
    "cmd": "ADD_TIME",
    "val": 300
  }
  ```
- **Response (200 OK):**
  ```json
  {
    "ok": 1,
    "code": "OK",
    "rem": 300,
    "state": "RUNNING"
  }
  ```
- **Daftar Perintah (`cmd`):**
  - `ADD_TIME`: Menambah waktu bermain (butuh `val` dalam satuan detik).
  - `PAUSE`: Menjeda timer (mematikan relay).
  - `RESUME`: Melanjutkan timer dari posisi jeda.
  - `STOP`: Menghentikan dan mereset timer menjadi 0 (LOCKED).
  - `REBOOT`: Melakukan restart (*reboot*) pada perangkat ESP32 slave.
  - `IDENTIFY`: Membunyikan buzzer 3 kali untuk mengidentifikasi fisik excavator.
