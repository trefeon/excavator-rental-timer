# Terminal Guide: Flashing Firmware via Command Line

If you prefer using the terminal (or if the visual interface is giving you trouble), you can quickly scan for your board and flash it using terminal commands.

---

## 1. Finding Your COM Port (Terminal)

You can list all connected serial devices directly from the PowerShell terminal inside VS Code.

1. Open the Terminal in VS Code (`Ctrl` + `\`` or **Terminal > New Terminal**).
2. Ensure you are using PowerShell.
3. Copy and paste the following command and press **Enter**:
   ```powershell
   Get-PnpDevice -Class Ports -PresentOnly | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
   ```
4. Look at the output under **FriendlyName**. You will see something like `USB Serial Device (COM7)` or `USB-SERIAL CH340 (COM8)`. 
5. Note the `COM` port number (e.g., `COM7`).

---

## 2. Flashing the Firmware

Once you know your `COM` port, you can use the `pio` command to compile and upload the firmware in a single step.

The command format is:
`pio run -e <environment_name> -t upload --upload-port <COM_PORT>`

### The Environments:
- `master` -> The main remote controller
- `slave` -> Standard ESP32 module inside the excavator
- `slave_c3` -> ESP32-C3 Super Mini module inside the excavator
- `slave_8266` -> NodeMCU/ESP8266 module inside the excavator

### Examples:

**To flash the ESP32-C3 Super Mini on COM7:**
```powershell
pio run -e slave_c3 -t upload --upload-port COM7
```

**To flash the Master Remote on COM5:**
```powershell
pio run -e master -t upload --upload-port COM5
```

**To flash a Standard ESP32 Slave on COM8:**
```powershell
pio run -e slave -t upload --upload-port COM8
```

1. Copy the appropriate command.
2. Change the `COM` number at the end to match the port you found in Step 1.
3. Paste it into the VS Code Terminal and press **Enter**.
4. Wait for the green `[SUCCESS]` message at the bottom!

---

## 3. Viewing the Serial Logs (Terminal)

To see the heartbeat logs or check if the board is connecting to WiFi, you can open the serial monitor directly in the terminal:

```powershell
pio device monitor --port COM7
```
*(Replace `COM7` with your actual port).*

To exit the serial monitor, press `Ctrl` + `C`.
