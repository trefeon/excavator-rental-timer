# Diagram Mermaid — Excavator Timer Wi-Fi Master-Slave

Diagram menggunakan Mermaid.js. Render di GitHub, Markdown preview, atau editor Mermaid.

---

## 1. Arsitektur Sistem

```mermaid
flowchart LR
  android["Android / Browser<br/>Dashboard"]
  master["ESP32 Master<br/>AP + API Gateway<br/>192.168.4.1"]

  subgraph slave1["Excavator 1"]
    esp1["ESP32 Slave 1<br/>WiFi Client"]
    disp1["TM1637 Display"]
    relay1["Relay Power Gate"]
    toy1["PCB Mainan RC"]
  end

  subgraph slave2["Excavator 2"]
    esp2["ESP32 Slave 2<br/>WiFi Client"]
    disp2["TM1637 Display"]
    relay2["Relay Power Gate"]
    toy2["PCB Mainan RC"]
  end

  android -- "HTTP API<br/>GET /api/slaves<br/>POST /api/command" --> master
  master -- "WiFi<br/>GET /api/state (polling)<br/>POST /api/command (proxy)" --> esp1
  master -- "WiFi" --> esp2
  esp1 --> disp1
  esp1 --> relay1 --> toy1
  esp2 --> disp2
  esp2 --> relay2 --> toy2

  classDef phone fill:#eff6ff,stroke:#2563eb,stroke-width:2px,color:#1e3a8a
  classDef masterNode fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#78350f
  classDef slaveNode fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#064e3b
  classDef ioNode fill:#f8fafc,stroke:#475569,stroke-width:2px,color:#0f172a

  class android phone
  class master masterNode
  class esp1,esp2 slaveNode
  class disp1,disp2,relay1,relay2,toy1,toy2 ioNode
```

---

## 2. Wiring Relay Slave

```mermaid
flowchart LR
  batteryPlus["18650 +"]
  batteryMinus["18650 - / GND"]
  regulator["3.3V Buck Converter"]
  esp["ESP32 Slave"]
  buzzer["Buzzer (GPIO 27)"]
  button["Button Resume (GPIO 32)"]
  fuse["Fuse/PTC 2A-5A"]
  relay["Relay 3.3V<br/>COM → NO"]
  toyPlus["PCB Mainan +"]
  toyMinus["PCB Mainan -"]
  display["TM1637 Display<br/>CLK=22, DIO=23"]

  batteryPlus --> regulator --> esp
  batteryMinus --> regulator
  esp --> buzzer
  button --> esp
  esp --> display
  batteryPlus --> fuse --> relay --> toyPlus
  batteryMinus --> toyMinus
  esp -- "GPIO 26" --> relay

  classDef batt fill:#ecfdf5,stroke:#059669,stroke-width:2px
  classDef logic fill:#eff6ff,stroke:#2563eb,stroke-width:2px
  classDef switch fill:#fef2f2,stroke:#dc2626,stroke-width:2px
  classDef load fill:#f8fafc,stroke:#475569,stroke-width:2px

  class batteryPlus,batteryMinus batt
  class regulator,esp,buzzer,button,display logic
  class fuse,relay switch
  class toyPlus,toyMinus load
```

---

## 3. State Machine Slave

```mermaid
stateDiagram-v2
  [*] --> BOOT
  BOOT --> LOCKED: sisa waktu = 0
  BOOT --> RUNNING: sisa waktu > 0 (powerloss recovery)

  LOCKED --> RUNNING: ADD_TIME
  LOCKED --> LOCKED: PAUSE/STOP (abaikan)

  RUNNING --> RUNNING: ADD_TIME
  RUNNING --> PAUSED: PAUSE
  RUNNING --> ENDED: sisa waktu = 0
  RUNNING --> LOCKED: STOP

  PAUSED --> RUNNING: RESUME
  PAUSED --> PAUSED: ADD_TIME
  PAUSED --> LOCKED: STOP

  ENDED --> RUNNING: ADD_TIME
  ENDED --> LOCKED: STOP
```

---

## 4. Alur Tambah Waktu

```mermaid
sequenceDiagram
  participant Staff
  participant App as Android / Browser
  participant Master as ESP32 Master
  participant Slave as ESP32 Slave
  participant Relay
  participant Display as TM1637 Display

  Staff->>App: Tap +5 menit di EXC-01
  App->>Master: POST /api/command {"id":1,"cmd":"ADD_TIME","val":300}
  Master->>Master: Cari IP Slave EXC-01 di registry (mutex)
  Master->>Slave: POST /api/command {"cmd":"ADD_TIME","val":300}
  Slave->>Slave: Validasi command
  Slave->>Slave: remaining_seconds += 300
  Slave->>Slave: Save ke NVS
  Slave->>Relay: ON (jika state = RUNNING)
  Slave->>Display: Tampilkan MM:SS
  Slave-->>Master: {"ok":1,"rem":300,"state":"RUNNING"}
  Master-->>App: {"ok":1,"rem":300,"state":"RUNNING"}
  App->>App: Update UI + simpan log
```

---

## 5. Alur Hotswap Battery (Powerloss Recovery)

```mermaid
sequenceDiagram
  participant ESP as ESP32 Slave
  participant Store as NVS Storage
  participant Relay
  participant Staff
  participant Display as TM1637

  ESP->>Store: Simpan sisa waktu tiap 30 detik
  Staff->>ESP: Cabut battery 18650
  ESP--xESP: Power mati
  Relay-->>Relay: Default OFF
  Staff->>ESP: Pasang battery baru
  ESP->>Relay: Tetap OFF (boot safety)
  ESP->>Store: Load sisa waktu tersimpan
  ESP->>ESP: Bunyi peringatan 3x (3 detik jeda)
  ESP->>Relay: ON (auto-resume ke RUNNING)
  ESP->>Display: Tampilkan MM:SS
```

---

## 6. Alur Dashboard Polling

```mermaid
flowchart TD
  start["Dashboard terbuka"]
  poll["GET /api/slaves<br/>(setiap 2 detik)"]
  render["Render daftar mainan<br/>dengan warna status"]
  action{"Staff action?"}
  detail["Buka detail / modal"]
  cmd["Build command JSON"]
  send["POST /api/command<br/>ke Master"]
  resp{"Response?"}
  ok["Update UI<br/>Toast sukses"]
  fail["Toast error<br/>Retry jika perlu"]
  loop["Lanjut polling"]

  start --> poll --> render --> action
  action -->|tap baris| detail --> action
  action -->|tombol aksi| cmd --> send --> resp
  resp -->|200 OK| ok --> loop --> poll
  resp -->|502/error| fail --> loop

  classDef good fill:#ecfdf5,stroke:#059669,stroke-width:2px
  classDef action fill:#eff6ff,stroke:#2563eb,stroke-width:2px
  classDef warn fill:#fff7ed,stroke:#d97706,stroke-width:2px

  class start,poll,render,loop good
  class detail,cmd,send,ok action
  class action,resp,fail warn
```

---

## 7. Arsitektur Data (Dual-Core + Mutex)

```mermaid
flowchart LR
  subgraph core0["Core 0 — Polling Task"]
    pollTask["pollSlavesTask()"]
  end

  subgraph core1["Core 1 — Web Server"]
    webServer["server.handleClient()"]
  end

  subgraph shared["Shared Memory (Mutex Protected)"]
    mutex["SemaphoreHandle_t<br/>slavesMutex"]
    data["slaves[] array<br/>+ slaveCount"]
  end

  subgraph persistent["Persistent Storage"]
    nvs["NVS Preferences<br/>registry + id_map"]
  end

  pollTask -- "xSemaphoreTake/Give" --> mutex --> data
  webServer -- "xSemaphoreTake/Give" --> mutex --> data
  webServer --> nvs
  data --> pollTask
  data --> webServer

  classDef core fill:#eff6ff,stroke:#2563eb,stroke-width:2px
  classDef shared fill:#fef3c7,stroke:#d97706,stroke-width:2px
  classDef store fill:#ecfdf5,stroke:#059669,stroke-width:2px

  class core0,core1 core
  class mutex,data shared
  class nvs store
```
