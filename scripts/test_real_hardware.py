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

print("[2] Sending ADD_TIME (60 seconds) to Slave...")
post("/api/command", {"id": slave_id, "cmd": "ADD_TIME", "time": 60})
time.sleep(2) # Give Master time to proxy and poll

slaves_after = get("/api/slaves")
target_after = next(s for s in slaves_after if s["id"] == slave_id)
print(f"    -> State is now: {target_after['state']}, time_left={target_after['time_left']}")

print("[3] Sending STOP command to end session early...")
post("/api/command", {"id": slave_id, "cmd": "STOP", "time": 0})
time.sleep(2) # Give Master time to poll the LOCKED state

slaves_final = get("/api/slaves")
target_final = next(s for s in slaves_final if s["id"] == slave_id)
print(f"    -> State is now: {target_final['state']}")

if target_final["state"] == "LOCKED":
    print("\nSUCCESS: Logic Validated on Real Hardware!")
else:
    print("\nFAILED: Slave did not lock!")
