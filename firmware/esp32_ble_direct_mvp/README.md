# ESP32 BLE Direct MVP Firmware Skeleton

Firmware skeleton untuk MVP pedagang murah:

- ESP32 sebagai BLE peripheral.
- Android scan semua mainan dan connect hanya saat command.
- Relay murah memutus/menyambung power mainan.
- TM1637 4-digit display ditempel di mainan untuk sisa waktu.
- Timer disimpan di NVS agar tidak reset saat 18650 diganti.

## Pins

| Function | ESP32 DevKit |
| --- | --- |
| Relay control | GPIO26 |
| Battery ADC | GPIO34 |
| TM1637 CLK | GPIO18 |
| TM1637 DIO | GPIO19 |
| Status LED | GPIO2 |
| Service button | GPIO14 |

## Arduino Libraries

- ESP32 Arduino core
- TM1637Display
- Preferences
- BLEDevice

## Important Config

Before flashing each unit:

```cpp
static const char *TOY_ID = "EXC-01";
static const uint8_t TOY_NUMERIC_ID = 1;
static const char *DEVICE_SECRET = "replace-with-unique-secret";
```

For production:

```cpp
static const bool ALLOW_UNSIGNED_DEBUG_COMMANDS = false;
```

## Command Format

```text
v1|command_id|command|value|session_id|nonce|signature
```

Debug command example when unsigned debug is enabled:

```text
v1|1001|ADD_TIME|300|S-001|debug|debug
```

Supported commands:

```text
ADD_TIME
PAUSE
RESUME
STOP
CLEAR_FAULT
GET_STATE
```

