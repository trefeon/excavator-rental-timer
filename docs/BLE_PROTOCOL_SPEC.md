# BLE Protocol Spec - Excavator Timer MVP

## 1. Roles

```text
ESP32 module = BLE peripheral / GATT server
Android app  = BLE central / GATT client
```

Android scans all toys. Android connects only to one toy when sending a command.

## 2. IDs

Device name format:

```text
EXC-01
EXC-02
...
EXC-99
```

Toy numeric id:

```text
EXC-01 -> 1
EXC-02 -> 2
```

## 3. UUIDs

Service UUID:

```text
7b7d0001-8f2a-4f6b-9b2e-2f3ad5a10001
```

Characteristics:

| Name | UUID | Properties | Purpose |
| --- | --- | --- | --- |
| State | `7b7d0002-8f2a-4f6b-9b2e-2f3ad5a10001` | Read, Notify | Current state snapshot |
| Command | `7b7d0003-8f2a-4f6b-9b2e-2f3ad5a10001` | Write | Send command |
| Ack | `7b7d0004-8f2a-4f6b-9b2e-2f3ad5a10001` | Read, Notify | Last command result |
| Info | `7b7d0005-8f2a-4f6b-9b2e-2f3ad5a10001` | Read | Static device info |

## 4. State Codes

| Code | Name | Relay |
| --- | --- | --- |
| 0 | `LOCKED` | OFF |
| 1 | `RUNNING` | ON |
| 2 | `PAUSED` | OFF |
| 3 | `LOW_BATT` | OFF |
| 4 | `ENDED` | OFF |
| 5 | `FAULT` | OFF |

Battery codes:

| Code | Name |
| --- | --- |
| 0 | `UNKNOWN` |
| 1 | `OK` |
| 2 | `LOW` |
| 3 | `CRITICAL` |

Fault codes:

| Code | Name |
| --- | --- |
| 0 | `NONE` |
| 1 | `LOW_BATT_CUTOFF` |
| 2 | `STORAGE_CRC` |
| 3 | `RELAY_STUCK` |
| 4 | `COMMAND_AUTH` |

## 5. Advertising Payload

Advertising should fit in manufacturer data.

Manufacturer data bytes:

```text
0      version          uint8  = 1
1      toy_id           uint8
2      state            uint8
3      remaining_min    uint8  saturated 0..255
4      battery_status   uint8
5      fault_code       uint8
6      seq              uint8  increments on state change/report
7      flags            uint8
8..9   remaining_sec    uint16 little-endian, optional if room
```

Flags:

```text
bit 0 = connected
bit 1 = command_locked
bit 2 = provisioned
bit 3 = debug_unsigned_enabled
```

Advertising interval:

| State | Interval |
| --- | --- |
| `LOCKED` | 1000-2000 ms |
| `RUNNING` | 500-1000 ms |
| `PAUSED` | 1000 ms |
| `LOW_BATT` | 300-500 ms |
| `FAULT` | 300-500 ms |

## 6. State Characteristic

Read/notify string format:

```text
v1;toy=EXC-01;state=RUNNING;rem=299;disp=03:20;paid=300;bat=OK;fault=0;seq=44;sid=S20260531-001
```

Fields:

| Field | Meaning |
| --- | --- |
| `toy` | Device id |
| `state` | State name |
| `rem` | Remaining seconds |
| `disp` | Current TM1637 display text |
| `paid` | Total paid seconds |
| `bat` | Battery status name |
| `fault` | Fault code |
| `seq` | State sequence |
| `sid` | Session id |

## 7. Command Characteristic

Command write format:

```text
v1|command_id|command|value|session_id|nonce|signature
```

Example signed command:

```text
v1|1021|ADD_TIME|300|S20260531-001|9F2A01BC|9d6e...
```

Unsigned debug command:

```text
v1|1021|ADD_TIME|300|S20260531-001|debug|debug
```

Supported commands:

| Command | Value | Behavior |
| --- | --- | --- |
| `ADD_TIME` | seconds | Add paid time, 300-second increments |
| `PAUSE` | 0 | Relay OFF, keep remaining |
| `RESUME` | 0 | Relay ON if remaining > 0 and battery OK |
| `STOP` | 0 | Relay OFF, clear remaining/session |
| `CLEAR_FAULT` | 0 | Clear recoverable fault |
| `GET_STATE` | 0 | Notify current state |

## 8. Ack Characteristic

Ack read/notify format:

```text
v1;cmd=1021;ok=1;code=OK;state=RUNNING;rem=599
```

Error example:

```text
v1;cmd=1022;ok=0;code=LOW_BATT;state=LOW_BATT;rem=599
```

Ack codes:

| Code | Meaning |
| --- | --- |
| `OK` | Command executed |
| `DUPLICATE` | Same command id already processed |
| `BAD_FORMAT` | Command parse failed |
| `BAD_SIGNATURE` | HMAC failed |
| `BAD_COUNTER` | command_id not newer |
| `BAD_STATE` | Command not allowed for current state |
| `LOW_BATT` | Battery too low to run |
| `LIMIT` | Value exceeds allowed limit |
| `FAULT` | Device in fault state |

## 9. Signature

Canonical string:

```text
toy_id|command_id|command|value|session_id|nonce
```

Signature:

```text
hex(HMAC_SHA256(device_secret, canonical_string))
```

Example canonical string:

```text
EXC-01|1021|ADD_TIME|300|S20260531-001|9F2A01BC
```

Anti replay:

- ESP stores `last_command_id`.
- New command must have `command_id > last_command_id`.
- If same `command_id` repeats, ESP returns previous ACK and does not execute again.

## 10. Android Command Flow

```text
scan advertising
select EXC-01
connect GATT
read State
write Command
wait Ack notify/read
read State if needed
disconnect
```

If BLE disconnects before ACK:

```text
reconnect
read State/Ack
if last_command_id already applied -> do not send new ADD_TIME
if not applied -> resend same command_id
```
