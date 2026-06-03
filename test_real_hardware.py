import json, time
from urllib.request import urlopen, Request

MASTER_URL = "http://192.168.4.1"

def get(ep):
    return json.loads(urlopen(MASTER_URL + ep, timeout=3).read().decode())
def post(ep, d):
    req = Request(MASTER_URL + ep, data=json.dumps(d).encode(), headers={"Content-Type": "application/json"})
    return json.loads(urlopen(req, timeout=3).read().decode())

print("========================================")
print(" REAL HARDWARE SLAVE -> MASTER VALIDATION")
print("========================================")

slaves = get("/api/slaves")
online_slaves = [s for s in slaves if s.get("online", False)]

if not online_slaves:
    print("ERROR: No real slaves are online!")
    exit(1)

target = online_slaves[0]
slave_id = target["id"]
print(f"[1] Found Online Real Slave: ID={slave_id} (IP: {target['ip']})")

rev_before = next((r for r in get("/api/revenue") if r.get("id") == slave_id), {"sessions":0, "revenueIDR":0})
print(f"[2] Current Revenue for Slave {slave_id}: {rev_before['sessions']} sessions, {rev_before['revenueIDR']} IDR")

print("[3] Sending ADD_TIME (60 seconds) to Slave...")
post("/api/command", {"id": slave_id, "cmd": "ADD_TIME", "val": 60})
time.sleep(2) # Give Master time to proxy and poll

slaves_after = get("/api/slaves")
target_after = next(s for s in slaves_after if s["id"] == slave_id)
print(f"    -> State is now: {target_after['state']}, paid={target_after['paid']}")

print("[4] Sending STOP command to end session early...")
post("/api/command", {"id": slave_id, "cmd": "STOP", "val": 0})
time.sleep(2) # Give Master time to poll the LOCKED state

rev_after = next((r for r in get("/api/revenue") if r.get("id") == slave_id), {"sessions":0, "revenueIDR":0})
print(f"[5] Final Revenue for Slave {slave_id}: {rev_after['sessions']} sessions, {rev_after['revenueIDR']} IDR")

if rev_after["sessions"] == rev_before.get("sessions", 0) + 1:
    print("\nSUCCESS: Logic Validated on Real Hardware!")
else:
    print("\nFAILED: Sessions did not increase!")
