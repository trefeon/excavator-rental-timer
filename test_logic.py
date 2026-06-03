import json, time, sys, threading
from urllib.request import urlopen, Request
from http.server import BaseHTTPRequestHandler, HTTPServer

MASTER_URL = "http://192.168.4.1"

# Mock Slave State
class MockState:
    state = "LOCKED"
    paid = 0
    rem = 0

class MockSlaveHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args): pass
    def do_GET(self):
        if self.path == "/api/state":
            resp_bytes = json.dumps({"ok":1, "state": MockState.state, "rem": MockState.rem, "disp": "00:00", "paid": MockState.paid, "bat": "100%"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
        else:
            self.send_response(404); self.end_headers()
    def do_POST(self):
        if self.path == "/api/command":
            content_length = int(self.headers.get('Content-Length', 0))
            self.rfile.read(content_length) # consume body
            # App sent ADD_TIME, mock slave changes state to RUNNING
            MockState.state = "RUNNING"
            MockState.paid = 15000
            MockState.rem = 900
            resp_bytes = b'{"ok":1,"code":"OK","rem":900,"state":"RUNNING"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
        else:
            self.send_response(404); self.end_headers()

def run_server():
    HTTPServer(('0.0.0.0', 80), MockSlaveHandler).serve_forever()

threading.Thread(target=run_server, daemon=True).start()

def get(ep):
    return json.loads(urlopen(MASTER_URL + ep, timeout=3).read().decode())
def post(ep, d):
    req = Request(MASTER_URL + ep, data=json.dumps(d).encode(), headers={"Content-Type": "application/json"})
    return json.loads(urlopen(req, timeout=3).read().decode())

print("========================================")
print(" SLAVE -> MASTER LOGIC VALIDATION SCRIPT")
print("========================================")

print("\n[1] Resetting Master...")
post("/api/reset-all", {"id":0})
time.sleep(1)

print("[2] Registering single slave for validation...")
reg = get("/api/register?mac=AA:BB:CC:DD:EE:99")
slave_id = reg.get("id")
print(f"  -> Assigned ID: {slave_id}")
time.sleep(1)

print("[3] Simulating Command from App to start Session...")
# This will make Master hit our POST /api/command, which sets our state to RUNNING and paid=15000
post("/api/command", {"id": slave_id, "cmd": "ADD_TIME", "val": 15})
time.sleep(2)

print("[4] Checking Master's view of Slave State...")
slaves = get("/api/slaves")
target_slave = next((s for s in slaves if s["id"] == slave_id), None)
assert target_slave is not None, "Slave not found in master list!"
assert target_slave["state"] == "RUNNING", f"Expected RUNNING, got {target_slave['state']}"
assert target_slave["paid"] == 15000, f"Expected paid=15000, got {target_slave['paid']}"
print("  -> SUCCESS: Master successfully registered slave state as RUNNING with paid=15000!")

print("[5] Simulating End of Session (Slave locks)...")
MockState.state = "ENDED"
time.sleep(3) # Wait for Master to poll the ENDED state and trigger recordSessionEnd

print("[6] Validating History & Revenue Aggregation on Master...")
rev = get("/api/revenue")
target_rev = next((r for r in rev if r.get("id") == slave_id), None)
assert target_rev is not None, "Revenue record not found for slave!"
assert target_rev["sessions"] == 1, f"Expected 1 session, got {target_rev['sessions']}"
assert target_rev["revenueIDR"] == 15000, f"Expected 15000 revenue, got {target_rev['revenueIDR']}"
print("  -> SUCCESS: Master detected session end and aggregated revenue correctly!")

print("\n========================================")
print(" ALL SLAVE-TO-MASTER LOGIC VALIDATED OK!")
print("========================================")
