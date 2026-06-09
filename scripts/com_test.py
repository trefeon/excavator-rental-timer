import serial, time, subprocess, threading

def read_serial(port):
    try:
        with serial.Serial(port, 115200, timeout=1) as s:
            t0 = time.time()
            while time.time() - t0 < 6:
                line = s.readline()
                if line:
                    print(f'[{port}]', line.decode('utf-8', 'ignore').strip())
    except Exception as e:
        print(f'[{port}] Error:', e)

t1 = threading.Thread(target=read_serial, args=('COM6',))
t2 = threading.Thread(target=read_serial, args=('COM26',))
t3 = threading.Thread(target=read_serial, args=('COM27',))
t1.start()
t2.start()
t3.start()

time.sleep(2)
try:
    from urllib.request import urlopen, Request
    import json
    print("Sending command...")
    req = Request("http://192.168.4.1/api/command", data=b'{"id":2,"cmd":"ADD_TIME","val":60}', headers={"Content-Type": "application/json"})
    urlopen(req, timeout=3)
except Exception as e:
    print("HTTP Error:", e)

t1.join()
t2.join()
t3.join()
