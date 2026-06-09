import serial
import time
import threading

def monitor(port):
    try:
        s = serial.Serial(port, 115200, timeout=1)
        while True:
            line = s.readline()
            if line:
                print(f"[{port}] {line.decode('utf-8', errors='ignore').strip()}", flush=True)
    except Exception as e:
        print(f"Error on {port}: {e}")

ports = ['COM6', 'COM26', 'COM27']
for p in ports:
    threading.Thread(target=monitor, args=(p,), daemon=True).start()

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass
