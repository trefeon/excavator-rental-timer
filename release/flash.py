import os
import sys
import subprocess
import time

try:
    import serial.tools.list_ports
    import esptool
except ImportError:
    print("Installing required Python packages (esptool, pyserial)...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "esptool", "pyserial"])
    import serial.tools.list_ports

def get_ports():
    ports = serial.tools.list_ports.comports()
    # Keywords often found in ESP32/ESP8266 USB-to-Serial driver descriptions
    esp_keywords = ["CH340", "CH341", "CP210", "CP210x", "FTDI", "USB Serial Device", "JTAG", "UART"]
    
    likely_ports = []
    other_ports = []
    
    for p in ports:
        desc = p.description.upper()
        if any(keyword.upper() in desc for keyword in esp_keywords):
            likely_ports.append((p.device, p.description))
        else:
            other_ports.append((p.device, p.description))
            
    return likely_ports, other_ports

def main():
    print("=========================================")
    print("   Excavator Rental Timer - Flasher      ")
    print("=========================================\n")

    options = [
        {"name": "Master (WEBUI) - ESP32", "file": "master_WEBUI_merged.bin", "chip": "esp32", "baud": 460800},
        {"name": "Master (non-WEBUI) - ESP32", "file": "master_nonWEBUI_merged.bin", "chip": "esp32", "baud": 460800},
        {"name": "Slave Unit - ESP32-C3", "file": "slave_c3_merged.bin", "chip": "esp32c3", "baud": 460800},
        {"name": "Slave Unit - ESP32 (Standard)", "file": "slave_esp32_merged.bin", "chip": "esp32", "baud": 460800}
    ]

    while True:
        print("Available Firmware:")
        for i, opt in enumerate(options, 1):
            print(f"  {i}. {opt['name']}")
        
        print()
        while True:
            try:
                choice = int(input(f"Select firmware to flash (1-{len(options)}): "))
                if 1 <= choice <= len(options):
                    selected = options[choice-1]
                    break
            except ValueError:
                pass
            print("Invalid choice. Try again.")

        # Check if file exists
        if getattr(sys, 'frozen', False):
            script_dir = os.path.dirname(sys.executable)
        else:
            script_dir = os.path.dirname(os.path.abspath(__file__))
        bin_path = os.path.join(script_dir, selected['file'])
        
        if not os.path.exists(bin_path):
            print(f"\n[ERROR] Firmware file '{selected['file']}' not found!")
            print("Please ensure this script is in the same folder as the .bin files.")
            input("Press Enter to exit...")
            return

        print("\nDetecting USB/COM Ports...")
        likely_ports, other_ports = get_ports()
        all_ports = likely_ports + other_ports
        port = ""
        
        if not all_ports:
            print("[WARNING] No COM ports detected!")
            print("Please plug in the device. If it's plugged in, you might be missing the CH340 or CP210x USB drivers.")
            port = input("Enter COM port manually (e.g., COM3) or leave blank for auto-detect: ").strip()
        elif len(likely_ports) == 1 and len(other_ports) == 0:
            port = likely_ports[0][0]
            print(f"Auto-selected ESP device: {likely_ports[0][1]}")
        else:
            print("\nAvailable connected devices:")
            idx = 1
            for p_dev, p_desc in likely_ports:
                print(f"  {idx}. {p_desc}  <-- [Likely ESP Board]")
                idx += 1
            for p_dev, p_desc in other_ports:
                print(f"  {idx}. {p_desc}")
                idx += 1
                
            print()
            p_choice = input("Select device number (or enter COM port manually, e.g. COM4): ").strip()
            try:
                choice_idx = int(p_choice) - 1
                if choice_idx < len(likely_ports):
                    port = likely_ports[choice_idx][0]
                else:
                    port = other_ports[choice_idx - len(likely_ports)][0]
            except (ValueError, IndexError):
                port = p_choice

        erase_choice = input("\nDo you want to completely erase all previous data before flashing? (y/N): ").strip().lower()
        erase_all = erase_choice == 'y'

        target_port = f"{port}" if port else "Auto-detect"
        print(f"\nFlashing {selected['name']} to {target_port}...")
        print("Please wait, this will take about 15-30 seconds...")
        time.sleep(1)

        args = ["--chip", selected["chip"]]
        if port:
            args.extend(["--port", port])
        args.extend(["--baud", str(selected["baud"]), "write-flash"])
        if erase_all:
            args.append("--erase-all")
        args.extend(["0x0", bin_path])
        
        print(f"\n> esptool {' '.join(args)}\n")
        
        # In a PyInstaller executable, we cannot run sys.executable -m esptool
        # We must call esptool.main directly.
        # esptool.main calls sys.exit(), so we must catch SystemExit to stay in the loop.
        result = 0
        try:
            import esptool
            esptool.main(args)
        except SystemExit as e:
            if e.code != 0 and e.code is not None:
                result = e.code
        except Exception as e:
            print(f"Exception during flashing: {e}")
            result = 1
        
        # Force a hardware reset by releasing DTR and RTS
        if port and result == 0:
            try:
                import serial
                s = serial.Serial(port)
                s.dtr = False
                s.rts = False
                s.close()
            except Exception:
                pass
        
        print("\n=========================================")
        if result == 0:
            print(" [SUCCESS] Firmware flashed successfully!")
            print(" The device will now reboot and run the new firmware.")
        else:
            print(" [ERROR] Flashing failed.")
            print(" Troubleshooting:")
            print("  - Check your USB cable (some are power-only, you need a data cable).")
            print("  - Try holding the 'BOOT' button on the board while it connects.")
            print("  - Make sure the COM port isn't being used by another program (like a Serial Monitor).")
            print("  - Try reinstalling the CH340 or CP210x USB drivers if the board isn't recognized.")
        print("=========================================")
        
        repeat = input("\nPress Enter to flash another board, or type 'q' to quit: ").strip().lower()
        if repeat == 'q':
            break
        print("\n\n")

if __name__ == "__main__":
    main()
