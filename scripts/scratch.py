"""
Advanced Crowd RC Stress Test for Excavator Rental Master
Simulates 9 slaves and 1 master environment with heavy traffic.
"""
import json
import time
import sys
import threading
import random
from urllib.request import urlopen, Request
from urllib.error import URLError
from http.server import BaseHTTPRequestHandler, HTTPServer
import socket

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

MASTER_URL = "http://192.168.4.1"
SLAVE_COUNT = 9
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--verbose', action='store_true', help='Print verbose requests/responses')
parser.add_argument('--duration', type=int, default=0, help='Run duration in seconds')
args = parser.parse_args()

VERBOSE = args.verbose
RUN_DURATION = args.duration

# ── Metrics ──────────────────────────────────────────────────
stats = {
    "req_count": 0,
    "err_count": 0,
    "slave_hits": 0
}

def record_req(success):
    if success:
        stats["req_count"] += 1
    else:
        stats["err_count"] += 1

# ── HTTP Client Helpers ───────────────────────────────────────
def get(url, timeout=3):
    try:
        r = urlopen(url, timeout=timeout)
        b = r.read().decode()
        record_req(True)
        if VERBOSE:
            print(f"[GET] {url} -> {r.status} | len: {len(b)}", flush=True)
        return r.status, b
    except Exception as e:
        record_req(False)
        return None, str(e)

def post(ep, d, timeout=3):
    try:
        payload = json.dumps(d).encode()
        req = Request(f"{MASTER_URL}{ep}", data=payload, headers={"Content-Type": "application/json"})
        r = urlopen(req, timeout=timeout)
        b = r.read().decode()
        record_req(True)
        if VERBOSE:
            print(f"[POST] {ep} {d} -> {r.status} | len: {len(b)}", flush=True)
        return r.status, json.loads(b) if b.startswith("{") else b
    except Exception as e:
        record_req(False)
        return None, str(e)

# ── Mock Slave Server (Port 80) ──────────────────────────────
class MockSlaveHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass # suppress standard logging

    def do_GET(self):
        if self.path == "/api/state":
            stats["slave_hits"] += 1
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":1,"state":"RUNNING","rem":300,"disp":"05:00","paid":300,"bat":"99%"}')
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == "/api/command":
            stats["slave_hits"] += 1
            content_length = int(self.headers.get('Content-Length', 0))
            self.rfile.read(content_length) # consume body
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":1,"code":"OK","rem":300,"state":"RUNNING"}')
        else:
            self.send_response(404)
            self.end_headers()

def run_mock_slave_server():
    try:
        server = HTTPServer(('0.0.0.0', 80), MockSlaveHandler)
        print("[MOCK SLAVE] Listening on port 80 to catch master's polling...")
        server.serve_forever()
    except Exception as e:
        print(f"[MOCK SLAVE] Failed to start on port 80: {e}. Master will experience timeouts (good for stress testing).")

# ── Actors ───────────────────────────────────────────────────

def setup_slaves():
    print(f"\n[SETUP] Registering {SLAVE_COUNT} slaves to Master...")
    for i in range(1, SLAVE_COUNT + 1):
        mac = f"AA:BB:CC:DD:EE:{i:02X}"
        s, b = get(f"{MASTER_URL}/api/register?mac={mac}")
        if s == 200:
            print(f"  -> Registered {mac} : {b.strip()}")
        else:
            print(f"  -> Failed to register {mac} : {b}")
        time.sleep(0.2)

def app_dashboard_thread():
    """Simulates Android app continuously checking the dashboard."""
    while True:
        get(f"{MASTER_URL}/api/slaves")
        time.sleep(1.0)

def app_admin_thread():
    """Simulates Admin checking history and revenue periodically."""
    while True:
        get(f"{MASTER_URL}/api/history")
        time.sleep(0.5)
        get(f"{MASTER_URL}/api/revenue")
        time.sleep(2.5)

def app_operator_thread():
    """Simulates an active operator pressing command buttons across 9 excavators."""
    commands = [
        {"cmd": "ADD_TIME", "val": 5},
        {"cmd": "ADD_TIME", "val": 10},
        {"cmd": "PAUSE", "val": 0},
        {"cmd": "RESUME", "val": 0},
        {"cmd": "STOP", "val": 0}
    ]
    while True:
        target_id = random.randint(1, SLAVE_COUNT)
        cmd = random.choice(commands)
        post("/api/command", {"id": target_id, "cmd": cmd["cmd"], "val": cmd["val"]})
        time.sleep(0.8) # Operator pushes a button every 0.8 seconds

def attacker_thread():
    """Simulates bad actors sending broken requests to test resilience."""
    while True:
        try:
            # 1. Broken JSON
            req = Request(f"{MASTER_URL}/api/command", data=b"\"{BAD_JSON!!!}", headers={"Content-Type": "application/json"})
            urlopen(req, timeout=3)
        except Exception:
            pass

        try:
            # 2. Huge Payload
            huge_payload = json.dumps({"id": 1, "cmd": "ADD_TIME", "data": "X" * 5000}).encode()
            req2 = Request(f"{MASTER_URL}/api/command", data=huge_payload, headers={"Content-Type": "application/json"})
            urlopen(req2, timeout=3)
        except Exception:
            pass

        try:
            # 3. Invalid Endpoint
            urlopen(f"{MASTER_URL}/api/hidden_admin_hack_attempt", timeout=3)
        except Exception:
            pass

        record_req(False) # Track these as errors/attacks
        time.sleep(5.0)

# ── Main ─────────────────────────────────────────────────────
if __name__ == "__main__":
    print("=" * 60)
    print(" CROWD RC SIMULATION (9 Slaves, 1 Master)")
    print("=" * 60)

    # 1. Start Mock Server (Non-blocking)
    threading.Thread(target=run_mock_slave_server, daemon=True).start()
    time.sleep(1)

    # 2. Factory Reset Master
    print("\n[RESET] Factory resetting Master before test...")
    post("/api/reset-all", {"id": 0})
    time.sleep(0.5)

    # 3. Register Slaves
    setup_slaves()

    # 4. Start Threads
    print("\n[STRESS] Launching Android App & Attacker Simulators...")
    threading.Thread(target=app_dashboard_thread, daemon=True).start()
    threading.Thread(target=app_admin_thread, daemon=True).start()
    threading.Thread(target=app_operator_thread, daemon=True).start()
    threading.Thread(target=attacker_thread, daemon=True).start()

    print("\n[TEST RUNNING] Press Ctrl+C to stop.")
    print(f"{'Time':<10} | {'HTTP Req/s':<12} | {'Errors':<8} | {'Mock Slave Hits/s':<18}")
    print("-" * 60)

    try:
        t_start = time.time()
        while True:
            t0 = time.time()
            req_start = stats["req_count"]
            err_start = stats["err_count"]
            hits_start = stats["slave_hits"]
            
            time.sleep(3)
            
            t1 = time.time()
            elapsed = t1 - t0
            
            rps = (stats["req_count"] - req_start) / elapsed
            errs = stats["err_count"] - err_start
            hps = (stats["slave_hits"] - hits_start) / elapsed
            
            total_elapsed = int(t1 - t_start)
            print(f"{total_elapsed:<10d} | {rps:<12.1f} | {errs:<8d} | {hps:<18.1f}")
            
            if 0 < RUN_DURATION < total_elapsed:
                break
                
    except KeyboardInterrupt:
        print("\n[STOPPED] Stress test interrupted by user.")

    print("\n" + "=" * 60)
    print(" FINAL STATISTICS")
    print("=" * 60)
    print(f"Total Requests Sent : {stats['req_count']}")
    print(f"Total Errors/Timeouts: {stats['err_count']}")
    print(f"Total Master->Slave : {stats['slave_hits']}")
    print("=" * 60)
    print("If errors are extremely high, the ESP32 Master is likely choking under the load.")
    print("If errors are 0 and RPS is stable, the ESP32 Master handles 9 slaves + 3 apps perfectly.")
