# Mermaid Diagrams - Excavator Timer MVP

Diagram ini memakai Mermaid.js supaya mudah dirender di GitHub, Markdown preview, dan dokumentasi web. Referensi yang dipakai: Mermaid flowchart, sequence diagram, state diagram, subgraph, dan `classDef` dari docs resmi Mermaid.

## 1. MVP System Architecture

```mermaid
flowchart LR
  android["Android Dashboard<br/>BLE central"]

  subgraph toy["Each Excavator Toy"]
    esp["ESP32 Module<br/>BLE peripheral<br/>Timer + NVS"]
    display["TM1637 4-digit display<br/>MM:SS visible on toy"]
    relay["Cheap 3V/3.3V relay<br/>NO contact"]
    pcb["Original RC toy PCB<br/>receiver + motors"]
    remote["Original RC remote"]
  end

  android -- "BLE scan advertising" --> esp
  android -- "connect only for command<br/>ADD_TIME / PAUSE / RESUME" --> esp
  esp -- "CLK/DIO" --> display
  esp -- "GPIO relay control" --> relay
  relay -- "switched battery +" --> pcb
  remote -- "2.4GHz RC control" --> pcb

  classDef phone fill:#eff6ff,stroke:#2563eb,stroke-width:2px,color:#1e3a8a;
  classDef module fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#064e3b;
  classDef power fill:#fff7ed,stroke:#d97706,stroke-width:2px,color:#7c2d12;
  classDef toybox fill:#f8fafc,stroke:#475569,stroke-width:2px,color:#0f172a;

  class android phone;
  class esp,display module;
  class relay power;
  class pcb,remote toybox;
```

## 2. Cheap Relay Wiring

```mermaid
flowchart LR
  batteryPlus["18650 +"]
  batteryMinus["18650 - / GND"]
  regulator["3.3V buck / buck-boost"]
  esp["ESP32"]
  adc["ADC divider<br/>220k / 100k + 100nF"]
  driver["Relay driver<br/>GPIO -> 1k -> transistor<br/>flyback diode on coil"]
  relayCoil["3V/3.3V relay coil"]
  fuse["Fuse / PTC<br/>2A-5A"]
  relayContact["Relay contact<br/>COM -> NO"]
  toyPlus["Toy PCB +"]
  toyMinus["Toy PCB -"]
  display["TM1637 display<br/>VCC/GND/CLK/DIO"]

  batteryPlus --> regulator --> esp
  batteryMinus --> regulator
  esp --> display
  batteryPlus --> adc --> esp
  batteryMinus --> adc

  esp -- "relay GPIO" --> driver --> relayCoil
  batteryPlus --> fuse --> relayContact --> toyPlus
  batteryMinus --> toyMinus

  classDef batt fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#064e3b;
  classDef logic fill:#eff6ff,stroke:#2563eb,stroke-width:2px,color:#1e3a8a;
  classDef switch fill:#fef2f2,stroke:#dc2626,stroke-width:2px,color:#7f1d1d;
  classDef load fill:#f8fafc,stroke:#475569,stroke-width:2px,color:#0f172a;

  class batteryPlus,batteryMinus batt;
  class regulator,esp,adc,display logic;
  class driver,relayCoil,fuse,relayContact switch;
  class toyPlus,toyMinus load;
```

## 3. Runtime State Machine

```mermaid
stateDiagram-v2
  [*] --> BOOT
  BOOT --> LOCKED: no saved time
  BOOT --> PAUSED: saved remaining_seconds > 0
  BOOT --> FAULT: storage CRC invalid

  LOCKED --> RUNNING: ADD_TIME 300 and battery OK
  LOCKED --> LOW_BATT: ADD_TIME 300 and battery critical

  RUNNING --> RUNNING: ADD_TIME 300
  RUNNING --> PAUSED: PAUSE
  RUNNING --> LOW_BATT: low battery cutoff
  RUNNING --> ENDED: remaining_seconds == 0

  PAUSED --> RUNNING: RESUME and battery OK
  PAUSED --> PAUSED: ADD_TIME 300
  PAUSED --> LOCKED: STOP

  LOW_BATT --> PAUSED: battery replaced<br/>CLEAR_FAULT
  LOW_BATT --> LOW_BATT: ADD_TIME 300

  ENDED --> LOCKED: STOP / clear session
  ENDED --> RUNNING: ADD_TIME 300

  FAULT --> PAUSED: CLEAR_FAULT with remaining_seconds > 0
  FAULT --> LOCKED: CLEAR_FAULT with no remaining time
```

## 4. Add Time BLE Command

```mermaid
sequenceDiagram
  participant Staff
  participant App as Android Dashboard
  participant ESP as ESP32 Toy Module
  participant Store as NVS Storage
  participant Relay
  participant Display as TM1637 Display

  Staff->>App: Tap +5 menit on EXC-01
  App->>ESP: BLE connect
  App->>ESP: Read State characteristic
  ESP-->>App: state=LOCKED/RUNNING/PAUSED, remaining
  App->>App: Build command_id, nonce, HMAC
  App->>ESP: Write Command ADD_TIME 300
  ESP->>ESP: Validate signature and command_id
  ESP->>Store: Save remaining_seconds + session
  ESP->>Relay: ON if battery OK and state RUNNING
  ESP->>Display: Show MM:SS
  ESP-->>App: ACK OK + new remaining
  App->>App: Save local transaction log
  App->>ESP: BLE disconnect
```

## 5. Battery Hotswap Flow

```mermaid
sequenceDiagram
  participant ESP as ESP32
  participant Store as NVS Storage
  participant Relay
  participant Staff
  participant App as Android Dashboard
  participant Display as TM1637

  ESP->>Store: Save remaining every 5-10s
  Staff->>ESP: Remove 18650 battery
  ESP--xESP: Power off
  Relay-->>Relay: Default OFF
  Staff->>ESP: Insert new 18650 battery
  ESP->>Relay: Keep OFF on boot
  ESP->>Store: Load saved remaining_seconds
  ESP->>Display: Show paused remaining
  ESP-->>App: Advertise PAUSED + remaining
  Staff->>App: Tap Resume
  App->>ESP: BLE command RESUME
  ESP->>Relay: ON if battery OK
  ESP->>Display: Show running MM:SS
```

## 6. Android BLE Direct Dashboard Flow

```mermaid
flowchart TD
  open["Open dashboard"]
  scan["Scan BLE advertisements"]
  list["Render toy list<br/>EXC-01 RUNNING 03m OK"]
  choose{"Staff action?"}
  detail["Open toy detail"]
  command["Build signed command"]
  connect["Connect BLE to selected toy only"]
  write["Write Command characteristic"]
  ack{"ACK OK?"}
  log["Save transaction log"]
  retry["Reconnect and read State/Ack<br/>resend same command_id if needed"]
  disconnect["Disconnect BLE"]
  continue["Continue scanning"]

  open --> scan --> list --> choose
  choose -->|tap row| detail --> choose
  choose -->|+5 / pause / resume| command --> connect --> write --> ack
  ack -->|yes| log --> disconnect --> continue --> scan
  ack -->|no / disconnect| retry --> ack

  classDef good fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#064e3b;
  classDef action fill:#eff6ff,stroke:#2563eb,stroke-width:2px,color:#1e3a8a;
  classDef warn fill:#fff7ed,stroke:#d97706,stroke-width:2px,color:#7c2d12;

  class open,scan,list,continue good;
  class detail,command,connect,write,log,disconnect action;
  class choose,ack,retry warn;
```

## 7. BLE Data Ownership

```mermaid
flowchart LR
  app["Android App<br/>dashboard + command + log"]
  adv["BLE Advertising<br/>toy_id, state, remaining_min, battery"]
  gatt["BLE GATT<br/>State / Command / Ack / Info"]
  esp["ESP32<br/>source of truth"]
  nvs["NVS<br/>remaining_seconds, state, session_id, command_id"]
  relay["Relay<br/>power gate"]
  display["TM1637<br/>visible countdown"]

  esp --> adv --> app
  app --> gatt --> esp
  esp <--> nvs
  esp --> relay
  esp --> display

  classDef appClass fill:#eff6ff,stroke:#2563eb,stroke-width:2px,color:#1e3a8a;
  classDef espClass fill:#ecfdf5,stroke:#059669,stroke-width:2px,color:#064e3b;
  classDef ioClass fill:#f8fafc,stroke:#475569,stroke-width:2px,color:#0f172a;

  class app appClass;
  class esp,nvs espClass;
  class adv,gatt,relay,display ioClass;
```
