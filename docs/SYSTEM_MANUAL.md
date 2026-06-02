# Excavator Rental Timer - Complete System Manual

This document serves as the complete operational, technical, and integration manual for the **Excavator Rental Timer Module**. It is designed to be shared with hardware technicians, operators, and external software developers.

---

## 1. System Overview
The system is a centralized Wi-Fi-based timer module designed for RC (Remote Control) Excavator rentals. It completely decouples the billing/timer logic from the proprietary RC controllers.

### Architecture (Master-Slave)
* **Master Unit (1x ESP32):** Acts as the central Wi-Fi Access Point (AP), Web Server, and API Gateway. Sits at the cashier/operator desk.
* **Slave Units (Multiple ESP32s):** Installed inside each excavator. They automatically connect to the Master, handle the physical countdown timer, and turn the excavator's power on/off via a relay.

### Key Features
* **Zero-Touch Provisioning:** Turn on a new excavator, and it automatically appears on the dashboard.
* **Powerloss Recovery:** The timer survives battery replacements.
* **Independent Operation:** Slaves manage their own timers. If the Master loses power, currently active toys keep running.

---

## 2. Hardware Assembly & Wiring

### Master Unit
Requires **no external wiring**. Simply connect an ESP32 DevKit V1 to a 5V USB power supply at the cashier desk.

### Slave Unit (Inside the Toy)
Each toy requires an ESP32 DevKit V1, a 3.3V Relay, a TM1637 4-digit display, an active buzzer, and an 18650 battery holder.

**Pin Mapping:**
* `GPIO 22` -> TM1637 CLK
* `GPIO 23` -> TM1637 DIO
* `GPIO 26` -> Relay IN
* `GPIO 27` -> Buzzer (+)
* `GPIO 32` -> Manual Resume Button (Internal Pull-up)

**Power Routing (Relay):**
1. Battery Positive (+) -> Fuse -> Relay COM.
2. Relay NO -> Toy PCB Positive (+).
3. Battery Negative (-) -> Toy PCB Negative (-).

*Note: The relay defaults to OFF (Normally Open) for safety.*

---

## 3. Wi-Fi & Network Configuration

The system does **not** require internet access. It creates its own secure, isolated local network.

* **SSID:** `ExcavatorMaster`
* **Password:** `12345678` (Configurable in `wifi_master.ino`)
* **Master IP Address:** `192.168.4.1`
* **Slave IP Addresses:** Assigned via DHCP automatically (`192.168.4.2` and above).

To access the system, connect your phone, tablet, or laptop to the `ExcavatorMaster` Wi-Fi network and navigate to `http://192.168.4.1` in your browser.

---

## 4. Operator Guide (Web Dashboard)

The Web Dashboard is the primary control center for the rental staff.

### Adding Time
1. Ensure the customer's toy is powered on and appears in the **"Select Device"** dropdown.
2. Click the **+5 Min** or **+10 Min** buttons. 
3. The relay will activate, the display on the toy will start blinking `MM:SS`, and the customer can begin playing.

### Pause / Resume
If a customer needs a break, select their toy and press **Pause**. The relay will cut power to the toy. Press **Resume** to restore power and continue the countdown.

### Stop
Pressing **Stop** will immediately end the session, reset the timer to zero, and cut power to the toy.

### Time Transfer
If an excavator breaks down while a customer is playing:
1. Turn on a backup excavator.
2. On the dashboard, click **Transfer Time**.
3. Select the broken toy as the source, and the backup toy as the destination. The time will instantly move over.

### Manage Devices
Click **Manage Devices** to rename a toy (e.g., change the ID from `3` to `99`) or to delete a missing toy from the registry.

---

## 5. Battery Hotswap & Safety

The system uses a shared 18650 battery for both the ESP32 and the toy. When the battery runs out during an active session, staff can replace it without losing the customer's remaining time.

**Hotswap Procedure:**
1. Pull out the dead 18650 battery. The toy and the timer display will die.
2. Insert a fresh 18650 battery.
3. The Slave ESP32 will boot, reconnect to Wi-Fi, and recover the remaining time from its NVS memory.
4. **Safety Warning:** The toy will beep 3 times (3 seconds) to warn the staff to remove their hands.
5. The toy will **Auto-Resume**, restoring power to the tracks/motors.

---

## 6. Mobile App & API Integration

If you wish to build a custom Android, iOS, or POS integration, the Master ESP32 provides a complete REST API. 

The Android app only needs to communicate with `http://192.168.4.1`. It never communicates directly with the toys.

**Key Endpoints:**
* `GET /api/slaves` - Returns a JSON array of all active toys and their current timers.
* `POST /api/command` - Sends commands (`ADD_TIME`, `STOP`, `PAUSE`) to a specific toy.
* `POST /api/transfer_time` - Moves time between toys.

**Developer Documentation:**
Please provide your software developer with the following files located in the `docs/` directory of the project:
1. `WIFI_API_SPEC.md` (Detailed endpoint documentation and JSON schemas).
2. `openapi.yaml` (Standardized Swagger/OpenAPI spec for auto-generating code).

---

## 7. Troubleshooting & LED Indicators

**Master ESP32 (At Cashier):**
* **Blue LED ON Solid:** Wi-Fi Access Point is active and ready.
* **Blue LED Blinking:** The Master is actively polling the slaves (Normal operation).

**Slave ESP32 (Inside Toy):**
* **Blue LED Blinking Slowly:** Attempting to connect to the `ExcavatorMaster` Wi-Fi.
* **Blue LED Blinking Fast:** Connected to Wi-Fi and actively sending heartbeat.
* **Display shows `----`:** Toy is Locked (Zero time remaining).
* **Display goes blank:** Battery is dead or fuse is blown.
