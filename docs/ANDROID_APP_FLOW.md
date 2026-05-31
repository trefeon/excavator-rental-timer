# Android App Flow / Mockup Spec

## 1. App Role

Android app is dashboard and command sender. ESP32 remains source of truth for timer and relay.

App responsibilities:

- Scan BLE advertising from all toys.
- Show state list for 10 toys.
- Connect to one toy only when sending command.
- Send signed commands.
- Store local transaction log.
- Provide QR provisioning.
- Mirror the timer shown on the TM1637 display attached to each toy.

## 2. Main Screens

```text
Dashboard
  -> Toy Detail
  -> Add Time Sheet
  -> Provision QR
  -> Logs
```

## 3. Dashboard Screen

Purpose: fast operator view.

Rows:

```text
EXC-01  RUNNING   03:20  OK
EXC-02  LOCKED    --     OK
EXC-03  PAUSED    02:40  LOW
EXC-04  OFFLINE   --     --
```

Row actions:

- Tap row -> detail.
- Long press -> quick `+5 menit`.

Status color:

| State | Color |
| --- | --- |
| `RUNNING` | green |
| `PAUSED` | amber |
| `LOW_BATT` | red |
| `FAULT` | red |
| `LOCKED` | gray |
| `OFFLINE` | muted |

## 4. Toy Detail Screen

Fields:

```text
Toy ID
State
Remaining time
Battery status
Last seen
Last command result
Session ID
Toy display value
```

Actions:

```text
[+5 menit]
[+10 menit]
[Pause]
[Resume]
[Stop]
[Clear Fault]
```

Primary workflow:

```text
staff taps EXC-01
staff taps +5 menit
app connects BLE
app writes ADD_TIME 300
app waits ACK
app disconnects
row updates from advertising/ack
```

## 5. Provisioning Flow

```text
staff taps Add Toy
scan QR sticker
QR contains toy_id, ble_name, secret
app stores toy profile locally
app watches BLE advertising for ble_name
if seen -> mark READY
```

QR payload example:

```json
{
  "v": 1,
  "toy_id": "EXC-01",
  "ble_name": "EXC-01",
  "secret": "base64-device-secret"
}
```

## 6. Command Sending Flow

```text
1. Build command_id.
2. Build nonce.
3. Build canonical payload.
4. Sign HMAC_SHA256 using device secret.
5. Connect BLE.
6. Read State characteristic.
7. Write Command characteristic.
8. Wait Ack notify/read.
9. Save local log.
10. Disconnect BLE.
```

Retry:

```text
If disconnect before ACK:
  reconnect
  read state/ack
  if command_id applied:
      mark success
  else:
      resend same command_id
```

## 7. Local Data

Toy profile:

```text
toy_id
ble_name
secret
last_seen_state
last_seen_remaining
last_seen_battery
last_seen_at
```

Session log:

```text
timestamp
toy_id
session_id
command_id
command
value
ack_code
remaining_after
operator_id optional
```

## 8. Offline Handling

```text
last_seen < 5s   -> ONLINE
5s..15s          -> WEAK
> 15s            -> OFFLINE
```

If command to offline toy:

```text
show "Belum terlihat BLE. Dekatkan HP/tablet ke mainan."
do not create paid transaction until ACK success
```

## 9. Mockup Artifact

Interactive/static mockup is in:

```text
docs/android-dashboard-mockup.html
```
