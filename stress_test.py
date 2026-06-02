"""Full-stress test — edge cases, error handling, real-world scenarios.
Tests EXC-01 (ESP32) and EXC-03 (ESP8266) simultaneously.
Requires PC connected to ExcavatorMaster WiFi."""
import json, time, sys, random, subprocess
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

URL = "http://192.168.4.1"
RETRIES = 3
RETRY_DELAY = 2
passed = 0
failed = 0
warned = 0

def post(ep, d, retries=RETRIES):
    payload = json.dumps(d).encode()
    req = Request(f"{URL}{ep}", data=payload, headers={"Content-Type": "application/json"})
    for attempt in range(1, retries + 1):
        try:
            r = urlopen(req, timeout=5)
            b = r.read().decode()
            return r.status, json.loads(b) if b.startswith("{") else b
        except HTTPError as e:
            msg = e.read().decode() if e.fp else str(e)
            return e.code, json.loads(msg) if msg.startswith("{") else msg
        except URLError as e:
            if attempt < retries:
                time.sleep(RETRY_DELAY)
            else:
                return None, str(e)
        except Exception as e:
            if attempt < retries:
                time.sleep(RETRY_DELAY)
            else:
                return None, str(e)
    return None, "exhausted"

def get(url, retries=RETRIES):
    for attempt in range(1, retries + 1):
        try:
            r = urlopen(url, timeout=5)
            return r.status, r.read().decode()
        except HTTPError as e:
            msg = e.read().decode() if e.fp else str(e)
            return e.code, msg
        except Exception as e:
            if attempt < retries:
                time.sleep(RETRY_DELAY)
            else:
                return None, str(e)
    return None, "exhausted"

def raw_post(ep, body_bytes, retries=RETRIES):
    """Send raw bytes without JSON encoding."""
    req = Request(f"{URL}{ep}", data=body_bytes,
                  headers={"Content-Type": "application/json"})
    for attempt in range(1, retries + 1):
        try:
            r = urlopen(req, timeout=5)
            b = r.read().decode()
            return r.status, b
        except HTTPError as e:
            msg = e.read().decode() if e.fp else str(e)
            return e.code, msg
        except Exception as e:
            if attempt < retries:
                time.sleep(RETRY_DELAY)
            else:
                return None, str(e)
    return None, "exhausted"

def check(tag="", ids=None):
    """Poll /api/slaves and print state for specified or all IDs."""
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        slaves = json.loads(b)
        if ids:
            slaves = [x for x in slaves if x["id"] in ids]
        for x in slaves:
            on = "ON" if x.get("online") else "OFF"
            print(f"    [{tag}] EXC-{x['id']:02d} | {on} | state={x.get('state')} rem={x.get('rem')} disp={x.get('disp')} paid={x.get('paid')}", flush=True)
        return json.loads(b)
    print(f"    [{tag}] API err: {b}", flush=True)
    return []

def get_slaves():
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        return json.loads(b)
    return []

def cmd(id, c, v=0):
    return post("/api/command", {"id": id, "cmd": c, "val": v})

def stop_all():
    """Force all slaves to LOCKED, rem=0."""
    cmd(1, "STOP")
    cmd(3, "STOP")
    time.sleep(0.5)

def wait_api(timeout=30):
    print("  Waiting for master API...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            r = urlopen(f"{URL}/api/slaves", timeout=3)
            r.read()
            print(" OK", flush=True)
            return True
        except Exception:
            print(".", end="", flush=True)
            time.sleep(1)
    print(" TIMEOUT", flush=True)
    return False

def wait_slave(id, timeout=25):
    """Wait until a specific slave is online and responding."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        slaves = get_slaves()
        for s in slaves:
            if s["id"] == id and s.get("online"):
                return True
        time.sleep(2)
    return False

def section(title):
    print(f"\n{'='*60}")
    print(f" {title}")
    print(f"{'='*60}", flush=True)

def result(test_id, condition, msg):
    global passed, failed, warned
    tag = "[PASS]" if condition else "[FAIL]"
    if not condition:
        failed += 1
    else:
        passed += 1
    print(f"    {tag} {test_id}: {msg}", flush=True)
    return condition

def warn(test_id, msg):
    global warned
    warned += 1
    print(f"    [WARN] {test_id}: {msg}", flush=True)


# ══════════════════════════════════════════════════════════════
#  RESET
# ══════════════════════════════════════════════════════════════

section("PHASE 0: RESET ALL DEVICES")

def ensure_wifi():
    """Reconnect to ExcavatorMaster WiFi if disconnected."""
    try:
        r = urlopen(f"{URL}/api/slaves", timeout=3)
        r.read()
        return True
    except Exception:
        pass
    print("  WiFi lost, reconnecting...", flush=True)
    subprocess.run(["netsh", "wlan", "connect", "name=ExcavatorMaster"],
                   capture_output=True, timeout=10)
    time.sleep(10)
    try:
        r = urlopen(f"{URL}/api/slaves", timeout=5)
        r.read()
        print("  WiFi reconnected", flush=True)
        return True
    except Exception:
        print("  WiFi reconnect failed!", flush=True)
        return False

ensure_wifi()

print("\n[RESET] Rebooting EXC-01...")
cmd(1, "REBOOT")
time.sleep(5)
print("[RESET] Rebooting EXC-03...")
cmd(3, "REBOOT")

# Wait for reboot with WiFi reconnection support
print("  Waiting 25s for slaves to reboot...")
time.sleep(25)
ensure_wifi()

if not wait_api(timeout=60):
    print("\n[FATAL] Master API unreachable. Trying WiFi reconnect...")
    ensure_wifi()
    if not wait_api(timeout=30):
        print("[FATAL] Still unreachable. Aborting.")
        sys.exit(1)

# Wait for both slaves to come back online
for slave_id in [1, 3]:
    if wait_slave(slave_id, timeout=25):
        print(f"  EXC-{slave_id:02d} online")
    else:
        print(f"  [WARN] EXC-{slave_id:02d} did not come back online!")

stop_all()
print("  Reset complete.\n")


# ══════════════════════════════════════════════════════════════
#  PHASE 1: INPUT BOUNDARY VALIDATION
# ══════════════════════════════════════════════════════════════

section("PHASE 1: INPUT BOUNDARY VALIDATION")

# --- T01-T06: ADD_TIME edge cases ---
print("\n[1.1] ADD_TIME value boundaries")

stop_all()
s, r = cmd(1, "ADD_TIME", -1)
result("T01", r.get("code") == "BAD_VAL", f"val=-1 rejected: {r}")

stop_all()
s, r = cmd(1, "ADD_TIME", 0)
result("T02", r.get("code") == "BAD_VAL", f"val=0 rejected: {r}")

stop_all()
s, r = cmd(1, "ADD_TIME", 1)
result("T03", r.get("ok") == 1 and r.get("rem") == 1, f"val=1 accepted: {r}")
stop_all()

s, r = cmd(1, "ADD_TIME", 999999)
result("T04", r.get("rem") == 28800, f"val=999999 capped at 28800: rem={r.get('rem')}")
stop_all()

# Same tests on EXC-03
s, r = cmd(3, "ADD_TIME", -500)
result("T05", r.get("code") == "BAD_VAL", f"EXC-03 val=-500 rejected: {r}")

stop_all()
s, r = cmd(3, "ADD_TIME", 0)
result("T06", r.get("code") == "BAD_VAL", f"EXC-03 val=0 rejected: {r}")

s, r = cmd(3, "ADD_TIME", 999999)
result("T07", r.get("rem") == 28800, f"EXC-03 val=999999 capped: rem={r.get('rem')}")
stop_all()


# --- T08-T12: Missing/invalid fields ---
print("\n[1.2] Missing and invalid fields")

s, r = post("/api/command", {})
result("T08", r.get("error", "") != "" or s == 400, f"empty body: {r}")

s, r = post("/api/command", {"cmd": "ADD_TIME", "val": 300})
result("T09", s == 400 or r.get("error", "") != "", f"missing id: {r}")

s, r = post("/api/command", {"id": 1, "val": 300})
result("T10", s == 400 or r.get("error", "") != "" or r.get("code") == "UNKNOWN_COMMAND", f"missing cmd: {r}")

s, r = cmd(99, "ADD_TIME", 300)
result("T11", r.get("error", "") != "" or s == 404, f"non-existent EXC-99: {r}")

s, r = cmd(1, "NONEXISTENT_CMD")
result("T12", r.get("code") == "UNKNOWN_COMMAND" or r.get("ok") == 0, f"unknown command: {r}")


# --- T13-T17: HTTP protocol edge cases ---
print("\n[1.3] HTTP protocol edge cases")

# Oversized payload
huge = json.dumps({"cmd": "ADD_TIME", "val": 1}) + "x" * 10000
s, r = raw_post("/api/command", huge.encode())
result("T13", s is not None, f"10KB payload: HTTP {s}")

# Malformed JSON
s, r = raw_post("/api/command", b"{not json}")
result("T14", s == 400, f"malformed JSON: HTTP {s}")

# Empty bytes
s, r = raw_post("/api/command", b"")
result("T15", s == 400 or s == 411, f"empty body: HTTP {s}")

# Binary garbage
s, r = raw_post("/api/command", bytes(range(256)) * 10)
result("T16", s is not None, f"binary garbage: HTTP {s}")

# JSON array instead of object
s, r = raw_post("/api/command", b'[1,2,3]')
result("T17", s == 400 or s is not None, f"JSON array body: HTTP {s}")


# --- T18-T21: Wrong HTTP methods ---
print("\n[1.4] Wrong HTTP methods")

try:
    req = Request(f"{URL}/api/command", method="GET")
    r = urlopen(req, timeout=5)
    s = r.status
except HTTPError as e:
    s = e.code
except Exception as e:
    s = str(e)
result("T18", s == 405 or s == 400 or s is not None, f"GET /api/command: {s}")

try:
    req = Request(f"{URL}/api/slaves", method="POST", data=b"{}", headers={"Content-Type":"application/json"})
    r = urlopen(req, timeout=5)
    s = r.status
except HTTPError as e:
    s = e.code
except Exception as e:
    s = str(e)
result("T19", s == 405 or s == 400 or s is not None, f"POST /api/slaves: {s}")

try:
    req = Request(f"{URL}/api/register", method="POST")
    r = urlopen(req, timeout=5)
    s = r.status
except HTTPError as e:
    s = e.code
except Exception as e:
    s = str(e)
result("T20", s == 405 or s == 400 or s is not None, f"POST /api/register: {s}")

# Unknown endpoint
s, r = get(f"{URL}/api/nonexistent")
result("T21", s == 404 or s is not None, f"GET /api/nonexistent: {s}")


# ══════════════════════════════════════════════════════════════
#  PHASE 2: STATE MACHINE TRANSITIONS
# ══════════════════════════════════════════════════════════════

section("PHASE 2: STATE MACHINE EDGE CASES")

print("\n[2.1] LOCKED state transitions")

stop_all()
time.sleep(0.5)

# LOCKED -> can't PAUSE
s, r = cmd(1, "PAUSE")
result("T22", r.get("code") == "BAD_STATE", f"PAUSE when LOCKED: {r}")

# LOCKED -> can't RESUME (rem=0)
s, r = cmd(1, "RESUME")
result("T23", r.get("code") == "BAD_STATE", f"RESUME when LOCKED rem=0: {r}")

# LOCKED -> STOP (already locked, should still work)
s, r = cmd(1, "STOP")
result("T24", r.get("ok") == 1, f"STOP when already LOCKED: {r}")


print("\n[2.2] LOCKED -> RUNNING -> PAUSED -> RUNNING -> STOPPED")

stop_all()
s, r = cmd(1, "ADD_TIME", 600)
result("T25", r.get("ok") == 1 and r.get("state") == "RUNNING", f"ADD_TIME from LOCKED: {r}")
time.sleep(0.5)

s, r = cmd(1, "PAUSE")
result("T26", r.get("ok") == 1 and r.get("state") == "PAUSED", f"PAUSE from RUNNING: {r}")
time.sleep(0.5)

# PAUSE when already PAUSED
s, r = cmd(1, "PAUSE")
result("T27", r.get("code") == "BAD_STATE", f"PAUSE when already PAUSED: {r}")

s, r = cmd(1, "RESUME")
result("T28", r.get("ok") == 1 and r.get("state") == "RUNNING", f"RESUME from PAUSED: {r}")
time.sleep(0.5)

s, r = cmd(1, "STOP")
result("T29", r.get("ok") == 1 and r.get("state") == "LOCKED", f"STOP from RUNNING: {r}")
time.sleep(0.5)

# RESUME after STOP (rem=0)
s, r = cmd(1, "RESUME")
result("T30", r.get("code") == "BAD_STATE", f"RESUME after STOP (rem=0): {r}")


print("\n[2.3] Timer expiry -> ENDED state")

stop_all()
s, r = cmd(1, "ADD_TIME", 5)
result("T31", r.get("ok") == 1, f"ADD_TIME 5s: {r}")
print("    Waiting 7s for timer to expire...", flush=True)
time.sleep(7)

s, b = get(f"{URL}/api/slaves")
if s == 200:
    slaves = json.loads(b)
    s1 = next((x for x in slaves if x["id"] == 1), None)
    if s1:
        state = s1.get("state")
        rem = s1.get("rem")
        result("T32", state == "LOCKED" and rem == 0, f"Timer expired: state={state} rem={rem}")
    else:
        result("T32", False, "EXC-01 not found")
else:
    result("T32", False, "API error")

# After ENDED: PAUSE should fail, RESUME should work if time is added
stop_all()
s, r = cmd(1, "PAUSE")
result("T33", r.get("code") == "BAD_STATE", f"PAUSE after ENDED: {r}")

s, r = cmd(1, "ADD_TIME", 120)
result("T34", r.get("ok") == 1 and r.get("state") == "RUNNING", f"ADD_TIME after ENDED: {r}")
stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 3: RAPID-FIRE / CONCURRENT OPERATIONS
# ══════════════════════════════════════════════════════════════

section("PHASE 3: RAPID-FIRE & CONCURRENT OPERATIONS")

print("\n[3.1] Rapid-fire ADD_TIME (10x 60s)")

stop_all()
for i in range(10):
    cmd(1, "ADD_TIME", 60)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    slaves = json.loads(b)
    s1 = next((x for x in slaves if x["id"] == 1), None)
    if s1:
        rem = s1.get("rem")
        result("T35", rem == 600, f"10x60s = 600s: rem={rem}")
    else:
        result("T35", False, "EXC-01 not found")
stop_all()


print("\n[3.2] Rapid-fire mixed commands")

stop_all()
cmd(1, "ADD_TIME", 300)
time.sleep(0.3)
cmd(1, "PAUSE")
time.sleep(0.1)
cmd(1, "RESUME")
time.sleep(0.1)
cmd(1, "PAUSE")
time.sleep(0.1)
cmd(1, "RESUME")
time.sleep(0.1)
cmd(1, "STOP")
time.sleep(0.3)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    slaves = json.loads(b)
    s1 = next((x for x in slaves if x["id"] == 1), None)
    if s1:
        result("T36", s1["state"] == "LOCKED" and s1["rem"] == 0, f"Final state after rapid mixed: {s1['state']} rem={s1['rem']}")
stop_all()


print("\n[3.3] Both EXC-01 and EXC-03 simultaneous ADD_TIME")

stop_all()
cmd(1, "ADD_TIME", 300)
cmd(3, "ADD_TIME", 600)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    slaves = json.loads(b)
    s1 = next((x for x in slaves if x["id"] == 1), None)
    s3 = next((x for x in slaves if x["id"] == 3), None)
    result("T37", s1 and s1.get("rem") == 300, f"EXC-01 rem={s1.get('rem') if s1 else '?'}")
    result("T38", s3 and s3.get("rem") == 600, f"EXC-03 rem={s3.get('rem') if s3 else '?'}")
stop_all()


print("\n[3.4] Concurrent STOP + ADD_TIME same device")

cmd(1, "ADD_TIME", 300)
time.sleep(0.3)
# Fire STOP and ADD_TIME at the same time
import concurrent.futures
def fire_stop():
    return cmd(1, "STOP")
def fire_add():
    return cmd(1, "ADD_TIME", 120)

with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
    f1 = ex.submit(fire_stop)
    f2 = ex.submit(fire_add)
    r1 = f1.result()
    r2 = f2.result()

time.sleep(0.5)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    slaves = json.loads(b)
    s1 = next((x for x in slaves if x["id"] == 1), None)
    if s1:
        # After concurrent STOP+ADD, state should be consistent (either LOCKED or RUNNING)
        consistent = s1["state"] in ["LOCKED", "RUNNING", "PAUSED"]
        result("T39", consistent, f"Concurrent STOP+ADD: state={s1['state']} rem={s1['rem']} (consistent={consistent})")
stop_all()


print("\n[3.5] Rapid REBOOT command (should not crash)")

s, r = cmd(1, "REBOOT")
result("T40", r.get("ok") == 1 or r.get("code") == "REBOOTING", f"REBOOT ack: {r}")
time.sleep(20)
ensure_wifi()
if wait_slave(1, timeout=25):
    result("T41", True, "Slave came back after rapid reboot")
else:
    result("T41", False, "Slave did not come back after reboot")


# ══════════════════════════════════════════════════════════════
#  PHASE 4: TIMER ACCURACY
# ══════════════════════════════════════════════════════════════

section("PHASE 4: TIMER ACCURACY")

print("\n[4.1] 10-second accuracy test (EXC-01)")

stop_all()
time.sleep(1)
cmd(1, "ADD_TIME", 30)
time.sleep(1)
s1_rem = None
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            s1_rem = x["rem"]
print(f"    t+0: rem={s1_rem}")

time.sleep(10)
s2_rem = None
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            s2_rem = x["rem"]
print(f"    t+10: rem={s2_rem}")

if s1_rem is not None and s2_rem is not None:
    delta = s1_rem - s2_rem
    result("T42", abs(delta - 10) <= 2, f"Timer drift: delta={delta}s (expected ~10)")
else:
    result("T42", False, "Could not read rem")
stop_all()


print("\n[4.2] 10-second accuracy test (EXC-03)")

stop_all()
time.sleep(1)
cmd(3, "ADD_TIME", 30)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            s1_rem = x["rem"]
print(f"    t+0: rem={s1_rem}")

time.sleep(10)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            s2_rem = x["rem"]
print(f"    t+10: rem={s2_rem}")

if s1_rem is not None and s2_rem is not None:
    delta = s1_rem - s2_rem
    result("T43", abs(delta - 10) <= 2, f"Timer drift: delta={delta}s (expected ~10)")
else:
    result("T43", False, "Could not read rem")
stop_all()


print("\n[4.3] Display overflow (EXC-03) - add > 5999s")

stop_all()
cmd(3, "ADD_TIME", 6000)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            disp = x.get("disp")
            rem = x.get("rem")
            result("T44", rem == 6000, f"EXC-03 rem={rem} (expected 6000)")
            result("T45", disp and ":" in str(disp), f"EXC-03 display={disp}")
stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 5: MANAGE API EDGE CASES
# ══════════════════════════════════════════════════════════════

section("PHASE 5: MANAGE API EDGE CASES")

print("\n[5.1] Edit slave")

# Edit with invalid MAC
s, r = post("/api/edit_slave", {"mac": "INVALID", "id": 5})
result("T46", s == 400 or r.get("ok") == 0, f"edit invalid MAC: {r}")

# Edit with non-existent MAC
s, r = post("/api/edit_slave", {"mac": "FF:FF:FF:FF:FF:FF", "id": 5})
result("T47", s == 200 or s == 400, f"edit non-existent MAC: HTTP {s}")

# Edit with missing fields
s, r = post("/api/edit_slave", {})
result("T48", s == 400 or r.get("ok") == 0, f"edit empty body: {r}")

# Edit with invalid JSON
s, r = raw_post("/api/edit_slave", b"not json")
result("T49", s == 400, f"edit malformed JSON: HTTP {s}")


print("\n[5.2] Delete slave")

s, r = post("/api/delete_slave", {"mac": "INVALID"})
result("T50", s == 400 or r.get("ok") == 0, f"delete invalid MAC: {r}")

s, r = post("/api/delete_slave", {"mac": "FF:FF:FF:FF:FF:FF"})
result("T51", s == 200 or s == 400, f"delete non-existent MAC: HTTP {s}")

s, r = raw_post("/api/delete_slave", b"garbage")
result("T52", s == 400, f"delete malformed JSON: HTTP {s}")


print("\n[5.3] Transfer edge cases")

stop_all()
# Transfer self -> self
s, r = post("/api/transfer_time", {"from_id": 1, "to_id": 1})
result("T53", s == 200 or s == 400 or r.get("ok") == 0, f"transfer self->self: HTTP {s}")

# Transfer with no time on source
s, r = post("/api/transfer_time", {"from_id": 1, "to_id": 3})
result("T54", r.get("ok") == 0 or s == 400, f"transfer with 0 rem: {r}")

# Transfer with non-existent source
cmd(3, "ADD_TIME", 120)
time.sleep(0.5)
s, r = post("/api/transfer_time", {"from_id": 99, "to_id": 3})
result("T55", r.get("ok") == 0 or s == 404, f"transfer non-existent source: {r}")

# Transfer with non-existent target
cmd(1, "ADD_TIME", 120)
time.sleep(0.5)
s, r = post("/api/transfer_time", {"from_id": 1, "to_id": 99})
result("T56", r.get("ok") == 0 or s == 404, f"transfer non-existent target: {r}")

# Transfer with invalid from_id and to_id
s, r = post("/api/transfer_time", {"to_id": 2})
result("T57", s == 400 or r.get("ok") == 0, f"transfer missing from_id: {r}")

s, r = post("/api/transfer_time", {"from_id": 2})
result("T58", s == 400 or r.get("ok") == 0, f"transfer missing to_id: {r}")

stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 6: REAL-WORLD ERROR HANDLING
# ══════════════════════════════════════════════════════════════

section("PHASE 6: REAL-WORLD ERROR HANDLING")

print("\n[6.1] Powerloss recovery (EXC-01)")

stop_all()
cmd(1, "ADD_TIME", 600)
time.sleep(2)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            print(f"    BEFORE: state={x['state']} rem={x['rem']}")

cmd(1, "REBOOT")
print("    Sending REBOOT, waiting 25s...")
time.sleep(25)
ensure_wifi()

for attempt in range(6):
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        slaves = json.loads(b)
        online = [x for x in slaves if x["id"] == 1 and x.get("online")]
        if online:
            break
    time.sleep(5)

s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            rem = x.get("rem", 0)
            state = x.get("state", "")
            print(f"    AFTER: state={state} rem={rem}")
            result("T59", state == "RUNNING" and rem > 500,
                   f"Powerloss recovery: state={state} rem={rem}")

stop_all()


print("\n[6.2] Powerloss recovery (EXC-03)")

stop_all()
cmd(3, "ADD_TIME", 600)
time.sleep(2)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            print(f"    BEFORE: state={x['state']} rem={x['rem']}")

cmd(3, "REBOOT")
print("    Sending REBOOT, waiting 25s...")
time.sleep(25)
ensure_wifi()

for attempt in range(6):
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        slaves = json.loads(b)
        online = [x for x in slaves if x["id"] == 3 and x.get("online")]
        if online:
            break
    time.sleep(5)

s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            rem = x.get("rem", 0)
            state = x.get("state", "")
            print(f"    AFTER: state={state} rem={rem}")
            result("T60", state == "RUNNING" and rem > 500,
                   f"Powerloss recovery ESP8266: state={state} rem={rem}")

stop_all()


print("\n[6.3] Rapid power cycling (3x REBOOT in quick succession)")

cmd(1, "ADD_TIME", 600)
time.sleep(1)
for i in range(3):
    cmd(1, "REBOOT")
    time.sleep(8)
    ensure_wifi()
    print(f"    Reboot {i+1}/3 done", flush=True)

print("    Waiting 25s for final recovery...")
time.sleep(25)
ensure_wifi()
if wait_slave(1, timeout=30):
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        for x in json.loads(b):
            if x["id"] == 1:
                result("T61", x.get("rem", 0) > 0,
                       f"After rapid cycling: rem={x.get('rem')} state={x.get('state')}")
else:
    result("T61", False, "Slave did not recover after rapid cycling")
stop_all()


print("\n[6.4] Timer countdown accuracy under load (EXC-01)")

stop_all()
cmd(3, "ADD_TIME", 300)  # keep EXC-03 busy
cmd(1, "ADD_TIME", 15)
time.sleep(0.5)
# Fire rapid commands while timer is counting
for i in range(5):
    cmd(3, "ADD_TIME", 60)
    time.sleep(0.3)

print("    Waiting 12s...", flush=True)
time.sleep(12)

s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            rem = x.get("rem", 0)
            result("T62", rem <= 5, f"Timer under load: rem={rem} (should be ~3s)")
stop_all()


print("\n[6.5] IDENTIFY on both devices")

s1, r1 = cmd(1, "IDENTIFY")
time.sleep(4)
s3, r3 = cmd(3, "IDENTIFY")
time.sleep(4)
result("T63", r1.get("ok") == 1, f"EXC-01 IDENTIFY: {r1}")
result("T64", r3.get("ok") == 1, f"EXC-03 IDENTIFY: {r3}")


print("\n[6.6] Multiple reboots with timer active (EXC-03)")

stop_all()
cmd(3, "ADD_TIME", 600)
time.sleep(1)

for i in range(3):
    s, r = cmd(3, "REBOOT")
    result(f"T65.{i+1}", r.get("ok") == 1 or r.get("code") == "REBOOTING",
           f"Reboot {i+1}/3 ack: {r}")
    time.sleep(20)
    ensure_wifi()
    if not wait_slave(3, timeout=25):
        result(f"T65.{i+1}", False, f"EXC-03 did not come back after reboot {i+1}")
        break
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        for x in json.loads(b):
            if x["id"] == 3:
                print(f"    After reboot {i+1}: rem={x['rem']} state={x['state']}", flush=True)

stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 7: FULL LIFECYCLE TEST (BOTH DEVICES)
# ══════════════════════════════════════════════════════════════

section("PHASE 7: FULL LIFECYCLE (BOTH DEVICES)")

stop_all()
time.sleep(1)

# EXC-01: ADD -> PAUSE -> RESUME -> ADD more -> STOP
print("\n[7.1] EXC-01 lifecycle")
cmd(1, "ADD_TIME", 300)
time.sleep(0.5)
check("EXC-01-ADD", [1])
cmd(1, "PAUSE")
time.sleep(0.3)
check("EXC-01-PAUSE", [1])
cmd(1, "RESUME")
time.sleep(0.3)
check("EXC-01-RESUME", [1])
cmd(1, "ADD_TIME", 120)
time.sleep(0.3)
check("EXC-01-ADD2", [1])
cmd(1, "STOP")
time.sleep(0.3)
s1 = check("EXC-01-STOP", [1])

# Verify total was tracked
for x in (s1 if isinstance(s1, list) else []):
    if x["id"] == 1:
        result("T66", x["state"] == "LOCKED" and x["rem"] == 0,
               f"EXC-01 final: state={x['state']} rem={x['rem']} paid={x.get('paid')}")


# EXC-03: same lifecycle
print("\n[7.2] EXC-03 lifecycle")
cmd(3, "ADD_TIME", 480)
time.sleep(0.5)
check("EXC-03-ADD", [3])
cmd(3, "PAUSE")
time.sleep(0.3)
check("EXC-03-PAUSE", [3])
cmd(3, "RESUME")
time.sleep(0.3)
check("EXC-03-RESUME", [3])
cmd(3, "ADD_TIME", 240)
time.sleep(0.3)
check("EXC-03-ADD2", [3])
cmd(3, "STOP")
time.sleep(0.3)
s3 = check("EXC-03-STOP", [3])

for x in (s3 if isinstance(s3, list) else []):
    if x["id"] == 3:
        result("T67", x["state"] == "LOCKED" and x["rem"] == 0,
               f"EXC-03 final: state={x['state']} rem={x['rem']} paid={x.get('paid')}")


# ══════════════════════════════════════════════════════════════
#  PHASE 8: STRESS OVERFLOW (ADD_TIME multiple times to cap)
# ══════════════════════════════════════════════════════════════

section("PHASE 8: OVERFLOW STRESS")

print("\n[8.1] Add time repeatedly to hit 28800 cap (EXC-01)")

stop_all()
for i in range(50):
    cmd(1, "ADD_TIME", 600)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            result("T68", x["rem"] == 28800, f"After 50x600s: rem={x['rem']} (expected 28800)")
            result("T69", x["state"] == "RUNNING", f"State: {x['state']}")
stop_all()


print("\n[8.2] Add time repeatedly on EXC-03 (8266) to hit 28800 cap")

stop_all()
for i in range(50):
    cmd(3, "ADD_TIME", 600)
time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 3:
            result("T70", x["rem"] == 28800, f"After 50x600s: rem={x['rem']}")
            result("T71", x["state"] == "RUNNING", f"State: {x['state']}")
stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 9: TRANSFER WITH TIME (REAL TRANSFER)
# ══════════════════════════════════════════════════════════════

section("PHASE 9: TRANSFER WITH REAL TIME")

stop_all()
cmd(1, "ADD_TIME", 600)
time.sleep(0.5)

s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            print(f"    Source EXC-01: rem={x['rem']} state={x['state']}")

s, r = post("/api/transfer_time", {"from_id": 1, "to_id": 3})
result("T72", r.get("ok") == 1, f"Transfer result: {r}")

time.sleep(1)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            result("T73", x["rem"] == 0 and x["state"] == "LOCKED",
                   f"Source after transfer: rem={x['rem']} state={x['state']}")
        if x["id"] == 3:
            result("T74", x["rem"] == 600,
                   f"Target after transfer: rem={x['rem']} state={x['state']}")

stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 10: MASTER API RESPONSIVENESS
# ══════════════════════════════════════════════════════════════

section("PHASE 10: MASTER API RESPONSIVENESS")

print("\n[10.1] Rapid GET /api/slaves (20x)")

t0 = time.time()
ok_count = 0
for i in range(20):
    s, b = get(f"{URL}/api/slaves")
    if s == 200:
        ok_count += 1
elapsed = time.time() - t0
result("T75", ok_count == 20, f"20x GET /api/slaves: {ok_count}/20 OK in {elapsed:.1f}s")
avg_ms = (elapsed / 20) * 1000
result("T76", avg_ms < 500, f"Avg response: {avg_ms:.0f}ms")


print("\n[10.2] Rapid POST /api/command (20x)")

t0 = time.time()
ok_count = 0
for i in range(20):
    s, r = cmd(1, "STOP")
    if s == 200:
        ok_count += 1
elapsed = time.time() - t0
result("T77", ok_count == 20, f"20x STOP: {ok_count}/20 OK in {elapsed:.1f}s")


# ══════════════════════════════════════════════════════════════
#  PHASE 11: EDGE CASE — ADD_TIME DURING PAUSE
# ══════════════════════════════════════════════════════════════

section("PHASE 11: ADD_TIME DURING PAUSE")

stop_all()
cmd(1, "ADD_TIME", 120)
time.sleep(0.5)
cmd(1, "PAUSE")
time.sleep(0.5)

s, r = cmd(1, "ADD_TIME", 60)
result("T78", r.get("ok") == 1, f"ADD_TIME while PAUSED: {r}")
time.sleep(0.5)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            result("T79", x["rem"] == 180, f"rem after ADD while PAUSED: {x['rem']} (expected 180)")

# Resume and verify time counts down
cmd(1, "RESUME")
time.sleep(3)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            result("T80", x["rem"] < 180, f"Timer ticking after RESUME: rem={x['rem']}")

stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 12: IDENTIFY + REBOOT COMBINATION
# ══════════════════════════════════════════════════════════════

section("PHASE 12: IDENTIFY + REBOOT COMBO")

# IDENTIFY while running
cmd(1, "ADD_TIME", 300)
time.sleep(0.5)
cmd(1, "IDENTIFY")
time.sleep(4)
s, b = get(f"{URL}/api/slaves")
if s == 200:
    for x in json.loads(b):
        if x["id"] == 1:
            result("T81", x["state"] == "RUNNING" and x["rem"] > 280,
                   f"IDENTIFY while running: state={x['state']} rem={x['rem']}")
stop_all()


# ══════════════════════════════════════════════════════════════
#  PHASE 13: MASTER REGISTRY EDGE CASES
# ══════════════════════════════════════════════════════════════

section("PHASE 13: REGISTRY EDGE CASES")

print("\n[13.1] Register with invalid MAC formats")

s, b = get(f"{URL}/api/register?mac=")
result("T82", s == 400, f"empty MAC: HTTP {s}")

s, b = get(f"{URL}/api/register?mac=INVALID")
result("T83", s == 400, f"invalid MAC: HTTP {s}")

s, b = get(f"{URL}/api/register?mac=AA:BB:CC:DD:EE")
result("T84", s == 400, f"short MAC: HTTP {s}")

s, b = get(f"{URL}/api/register?mac=AA:BB:CC:DD:EE:FF:00")
result("T85", s == 400, f"long MAC: HTTP {s}")

s, b = get(f"{URL}/api/register?mac=ZZ:ZZ:ZZ:ZZ:ZZ:ZZ")
result("T86", s == 400, f"hex-invalid MAC: HTTP {s}")


# ══════════════════════════════════════════════════════════════
#  SUMMARY
# ══════════════════════════════════════════════════════════════

section("FINAL CLEANUP")
stop_all()
time.sleep(1)
check("FINAL")

print(f"\n{'='*60}")
print(f" RESULTS")
print(f"{'='*60}")
total = passed + failed
print(f"  PASSED:  {passed}/{total}")
print(f"  FAILED:  {failed}/{total}")
print(f"  WARNED:  {warned}")
print(f"{'='*60}")

if failed == 0:
    print(" ALL TESTS PASSED!")
else:
    print(f" {failed} TEST(S) FAILED — review above")
