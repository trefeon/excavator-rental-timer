import sys
import json
import time
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8080
HTML_FILE = "frontend/index.html"

# Simulated DB
state_lock = threading.Lock()
slaves = [
    {"id": 1, "mac": "AA:BB:CC:DD:EE:01", "online": True, "state": "LOCKED", "time_left": 0, "battery": "OK", "sessionElapsed": 0, "stoppedManually": False},
    {"id": 2, "mac": "AA:BB:CC:DD:EE:02", "online": True, "state": "LOCKED", "time_left": 0, "battery": "OK", "sessionElapsed": 0, "stoppedManually": False},
    {"id": 3, "mac": "AA:BB:CC:DD:EE:03", "online": False, "state": "OFFLINE", "time_left": 0, "battery": "LOW", "sessionElapsed": 0, "stoppedManually": False}
]

stats = {
    "1": {"totalDetik": 120, "totalSesi": 3},
    "2": {"totalDetik": 0, "totalSesi": 0},
    "3": {"totalDetik": 450, "totalSesi": 5}
}

transaksi = []
karyawan = ["karyawan1", "karyawan2"]
current_simulated_date = ""

# Background simulator loop to tick timers
def simulator_loop():
    while True:
        time.sleep(1.0)
        with state_lock:
            for s in slaves:
                if not s["online"]:
                    continue
                if s["state"] == "RUNNING" and s["time_left"] > 0:
                    s["time_left"] -= 1
                    s["sessionElapsed"] += 1
                    
                    # Simulate periodic stats save every 10 seconds
                    if s["sessionElapsed"] >= 10:
                        sid_str = str(s["id"])
                        if sid_str not in stats:
                            stats[sid_str] = {"totalDetik": 0, "totalSesi": 0}
                        stats[sid_str]["totalDetik"] += s["sessionElapsed"]
                        s["sessionElapsed"] = 0
                        print(f"[SIMULATOR] Periodic save stats: EXC-{s['id']} totalDetik={stats[sid_str]['totalDetik']}")

                    if s["time_left"] == 0:
                        s["state"] = "ENDED"
                        sid_str = str(s["id"])
                        if sid_str not in stats:
                            stats[sid_str] = {"totalDetik": 0, "totalSesi": 0}
                        # Natural session end -> Add elapsed and increment sessions
                        if not s["stoppedManually"] and s["sessionElapsed"] > 0:
                            stats[sid_str]["totalDetik"] += s["sessionElapsed"]
                            stats[sid_str]["totalSesi"] += 1
                        s["sessionElapsed"] = 0
                        s["stoppedManually"] = False
                        print(f"[SIMULATOR] Natural session end: EXC-{s['id']} state=ENDED. stats={stats[sid_str]}")

class MockMasterHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Print logs to console
        sys.stdout.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), format%args))

    def send_json(self, data, code=200):
        resp = json.dumps(data).encode('utf-8')
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Admin-User, X-Admin-Pass")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Admin-User, X-Admin-Pass")
        self.end_headers()

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            # Serve the dashboard HTML file
            try:
                with open(HTML_FILE, "rb") as f:
                    html_content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(html_content)))
                self.end_headers()
                self.wfile.write(html_content)
            except Exception as e:
                self.send_error(500, f"Error reading {HTML_FILE}: {e}")
        
        elif self.path == "/api/auth":
            self.send_json({"exists": True})
            
        elif self.path == "/api/slaves":
            with state_lock:
                # Format to match what firmware returns
                res = []
                for s in slaves:
                    res.append({
                        "id": s["id"],
                        "ip": "",
                        "mac": s["mac"],
                        "online": s["online"],
                        "state": s["state"],
                        "time_left": s["time_left"],
                        "battery": s["battery"]
                    })
            self.send_json(res)
            
        elif self.path == "/api/stats":
            with state_lock:
                self.send_json(stats)
                
        elif self.path == "/api/transaksi":
            with state_lock:
                self.send_json(transaksi)
                
        elif self.path == "/api/karyawan":
            self.send_json({"karyawan": karyawan})
            
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length).decode('utf-8')
        
        if self.path == "/api/auth/login":
            # Simple dummy validation
            d = json.loads(body)
            u = d.get("username", "")
            p = d.get("password", "")
            if u == "admin":
                if p == "admin":
                    self.send_json({"ok": 1, "role": "sa"})
                else:
                    self.send_json({"ok": 0, "code": "WRONG_PASSWORD"}, 400)
            elif u in karyawan:
                if p == "admin" or p == "1234" or p == u:
                    self.send_json({"ok": 1, "role": "kr"})
                else:
                    self.send_json({"ok": 0, "code": "WRONG_PASSWORD"}, 400)
            else:
                self.send_json({"ok": 0, "code": "USER_NOT_FOUND"}, 400)
                
        elif self.path == "/api/command":
            d = json.loads(body)
            target_id = d.get("id")
            cmd = d.get("cmd")
            val_time = d.get("time", 0)
            
            with state_lock:
                target_slave = next((s for s in slaves if s["id"] == target_id), None)
                if not target_slave:
                    self.send_json({"ok": 0, "error": "Slave not found"}, 404)
                    return
                
                if cmd == "ADD_TIME":
                    target_slave["state"] = "RUNNING"
                    target_slave["time_left"] += val_time
                    target_slave["sessionElapsed"] = 0
                    target_slave["stoppedManually"] = False
                    
                elif cmd == "PAUSE":
                    target_slave["state"] = "PAUSED"
                    
                elif cmd == "RESUME":
                    target_slave["state"] = "RUNNING"
                    
                elif cmd == "STOP":
                    # Simulate residual flush
                    residual = target_slave["sessionElapsed"]
                    target_slave["state"] = "LOCKED"
                    target_slave["time_left"] = 0
                    target_slave["sessionElapsed"] = 0
                    target_slave["stoppedManually"] = True
                    
                    sid_str = str(target_id)
                    if sid_str not in stats:
                        stats[sid_str] = {"totalDetik": 0, "totalSesi": 0}
                    stats[sid_str]["totalDetik"] += residual
                    print(f"[SIMULATOR] Manual STOP residual stats save: EXC-{target_id} +{residual}s totalDetik={stats[sid_str]['totalDetik']}")

                self.send_json({
                    "ok": 1,
                    "code": "SUCCESS",
                    "time_left": target_slave["time_left"],
                    "state": target_slave["state"]
                })
                
        elif self.path == "/api/transaksi/add":
            trx_item = json.loads(body)
            with state_lock:
                transaksi.append(trx_item)
                # Sort newest first
                transaksi.sort(key=lambda x: x["ts"], reverse=True)
            self.send_json({"ok": 1})
            
        elif self.path == "/api/stats/add":
            d = json.loads(body)
            sid = str(d.get("id"))
            detik = d.get("detik", 0)
            sesi = d.get("sesi", 1)
            
            with state_lock:
                if sid not in stats:
                    stats[sid] = {"totalDetik": 0, "totalSesi": 0}
                stats[sid]["totalDetik"] += detik
                if sesi != 0:
                    stats[sid]["totalSesi"] += 1
            self.send_json({"ok": 1})
            
        elif self.path == "/api/stats/reset":
            d = json.loads(body)
            sid = str(d.get("id"))
            with state_lock:
                if sid in stats:
                    stats[sid]["totalDetik"] = 0
                    stats[sid]["totalSesi"] = 0
            self.send_json({"ok": 1})
            
        elif self.path == "/api/karyawan/add":
            d = json.loads(body)
            u = d.get("username")
            if u and u not in karyawan:
                karyawan.append(u)
                self.send_json({"ok": 1})
            else:
                self.send_json({"ok": 0, "code": "ALREADY_EXISTS"}, 400)
                
        elif self.path == "/api/karyawan/delete":
            d = json.loads(body)
            u = d.get("username")
            if u in karyawan:
                karyawan.remove(u)
                self.send_json({"ok": 1})
            else:
                self.send_json({"ok": 0}, 400)

        elif self.path == "/api/auth/verify-sa":
            d = json.loads(body)
            if d.get("password") == "admin":
                self.send_json({"ok": 1, "saUser": "admin"})
            else:
                self.send_json({"ok": 0}, 400)
        elif self.path == "/api/sync-time":
            d = json.loads(body)
            client_date = d.get("date", "")
            global current_simulated_date
            date_changed = False
            if current_simulated_date != "" and current_simulated_date != client_date:
                date_changed = True
                with state_lock:
                    for sid in stats:
                        stats[sid]["totalSesi"] = 0
            current_simulated_date = client_date
            self.send_json({"ok": 1, "dateChanged": date_changed})
        else:
            self.send_response(404)
            self.end_headers()

def main():
    # Start timer simulator thread
    t = threading.Thread(target=simulator_loop, daemon=True)
    t.start()
    
    server_address = ('', PORT)
    httpd = HTTPServer(server_address, MockMasterHandler)
    print(f"Started Mock Master server on http://localhost:{PORT}")
    print(f"To test E2E dashboard logic: open http://localhost:{PORT}/ in your browser.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down Mock Master server.")
        sys.exit(0)

if __name__ == "__main__":
    main()
