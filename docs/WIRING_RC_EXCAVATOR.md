# Wiring & Installation Guide for Slave Module on RC Excavator

This document contains brainstorming guidelines and wiring schematics for integrating the ESP32-C3 Super Mini module (as a Timer Slave) into the RC Excavator toy.

## 1. Power Cut-Off Strategy
The main purpose of this module is to turn off the RC Excavator when the rental time is up. The most effective and safe (Fail-Safe) way for rental toys is to **cut the main power line (Battery) to the RC Mainboard** using a Relay.

**Advantage of Normally Open (NO) mode:** 
If the ESP module dies, gets damaged, or encounters an error, the Relay will not activate. The RC will automatically die (cannot be played secretly or lose control).

---

## 2. Required Components
1. ESP32-C3 Super Mini Module (already flashed with `slave_c3` firmware).
2. 1-Channel Relay Module (5V, capable of holding a 10A load).
3. Step-down / Buck Converter (e.g. LM2596 or Mini360) to lower the RC battery voltage to 5V.
4. TM1637 4-Digit Display.
5. Active Buzzer 3.3V/5V.
6. Push Button (Tactile Switch).
7. Jumper wires / AWG 22 copper wires.

---

## 3. Main Wiring Schematics

### A. Y-Splitter Power Supply Schematic
Because we want to make the RC Battery the **single power source** for the ESP32 and the RC Mainboard, we must split the cables (parallel) from the battery connector.

The cable from the RC battery connector is split into **two branches**:
**Branch 1 (To power the ESP32):**
- RC Battery (+) ➔ **IN (+)** Buck Converter.
- RC Battery (-) ➔ **IN (-)** Buck Converter.
- **OUT (+)** Buck Converter *(set to 5V)* ➔ **5V Pin** on ESP32-C3.
- **OUT (-)** Buck Converter ➔ **GND Pin** on ESP32-C3.

**Branch 2 (To power the RC controlled by Relay):**
- RC Battery (-) ➔ Goes straight to **(-) Cable of RC Mainboard** without being cut.
- RC Battery (+) ➔ First goes to the **COM** screw on the Relay.
- The **NO (Normally Open)** screw on the Relay ➔ Goes to the **(+) Cable of RC Mainboard**.

*Electrical Flow Schematic:*
`Battery (+)` ──┬──> `Buck Converter` ──> `ESP32` (Always On)
              │
              └──> `Relay (COM)` ──[Open/Close]──> `Relay (NO)` ──> `RC Mainboard (+)`

### B. Relay Control Wiring from ESP32
So the ESP32 can control the relay switch above:
- **VCC Relay Pin** ➔ 5V Pin on ESP32-C3
- **GND Relay Pin** ➔ GND Pin on ESP32-C3
- **IN / Signal Relay Pin** ➔ **GPIO 4 Pin** on ESP32-C3

> [!NOTE]
> The `slave_c3` firmware is configured using **Active LOW (Low Trigger Relay)** logic. A LOW signal (0V) from GPIO 4 will activate the relay, and a HIGH signal (3.3V) will deactivate the relay. If you use a MOSFET (for example, P-channel with pull-up, or MOSFET inverter driver circuit), make sure this logic is adjusted accordingly.

### C. Peripheral Wiring (Display, Buzzer, Button)
According to the code in the firmware:

**1. TM1637 Display (Timer Screen):**
* VCC ➔ 5V ESP32
* GND ➔ GND ESP32
* CLK ➔ **GPIO 6**
* DIO ➔ **GPIO 7**

**2. Active Buzzer:**
* Positive Pole (+) ➔ **GPIO 5**
* Negative Pole (-) ➔ GND ESP32

**3. Push Button (Physical Resume Button):**
* Leg 1 Button ➔ **GPIO 9**
* Leg 2 Button ➔ GND ESP32

---

## 4. Physical Installation Tips on Excavator

1. **Display Placement:** 
   Find a flat surface on the upper *body* of the excavator (e.g., on the side window of the cabin or the rear engine cover). Make a hole in the plastic body the size of the TM1637 number box so it looks neat from the outside (embossed numbers). Seal it with Hot Glue to make it splash-proof.
   
2. **Button & Buzzer Position:**
   * **Buzzer** should face the RC plastic vents so its sound comes out loud when time is up.
   * **Resume Button** can be hidden at the bottom of the RC (near the factory on/off switch) or behind the cabin, making it easy for operators to reach but not easy for children to press randomly.

3. **ESP32 & Relay Module Protection:**
   Wrap the ESP32 and Buck Converter using a large transparent Heat Shrink Tube or put them in a small plastic box (e.g., a candy box) then hot-glue it before hiding it inside the RC hull. This is to avoid short circuits caused by dust, sand, or water in the play area.

4. **Relay Cable Strength:**
   Because the excavator motor current is quite large when the RC climbs or digs sand (can reach 2-4 Amperes), **make sure the cables entering the COM and NO ports on the Relay are thick enough**, don't use regular dupont jumper cables.

---

## 5. Hardware Flow Execution
- RC is turned on (Factory ON Switch is pressed).
- Battery supplies the Buck Converter ➔ ESP32 turns on.
- ESP32 searches for Master WiFi signal.
- Relay is in **OFF (Dead)** position, RC cannot be moved (Mainboard hasn't received power).
- Operator adds time from the Android application ➔ Master sends instructions to ESP32.
- ESP32 triggers Relay (Relay clicks) ➔ COM connects to NO ➔ RC Excavator turns on and can be controlled.
- Time is up ➔ Relay turns back **OFF** ➔ Mainboard dies instantly.
