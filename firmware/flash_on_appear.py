"""Grab the ESP32-C3 USB CDC port the moment it enumerates and flash immediately.

The board only exposes COM4/COM3 while it is awake; it drops USB as soon as it
goes into deep sleep, so a plain `esptool write-flash` almost always loses the
race. This polls for the port and fires the flash on the first hit.

Usage: python flash_on_appear.py [timeout_seconds]
"""
import subprocess
import sys
import time

import serial

ESPKTOOL = r"C:\Users\lyx\.platformio\packages\tool-esptool\esptool.exe"
BIN = r"D:\code\inksight\firmware\.pio\build\epd_398_nz07_a0_c3_promini\firmware_merged.bin"
PORTS = ("COM4", "COM3")

timeout = int(sys.argv[1]) if len(sys.argv) > 1 else 240
deadline = time.time() + timeout


def port_ready(name):
    try:
        s = serial.Serial(name, 115200, timeout=0.05)
        s.close()
        return True
    except Exception:
        return False


attempt = 0
while time.time() < deadline:
    for port in PORTS:
        if not port_ready(port):
            continue
        attempt += 1
        print(f"[flash] {port} is up (attempt {attempt}), flashing...", flush=True)
        try:
            proc = subprocess.run(
                [
                    ESPKTOOL, "--chip", "esp32c3", "--port", port,
                    "--baud", "921600",
                    "--before", "default-reset", "--after", "hard-reset",
                    "write-flash", "0x0", BIN,
                ],
                capture_output=True, text=True, timeout=240,
            )
        except subprocess.TimeoutExpired:
            print("[flash] esptool timed out, retrying", flush=True)
            continue
        tail = (proc.stdout or "")[-2500:]
        print(tail, flush=True)
        if proc.returncode == 0 and "Hash of data verified" in (proc.stdout or ""):
            print("[flash] SUCCESS - firmware written and verified", flush=True)
            sys.exit(0)
        print(f"[flash] failed rc={proc.returncode}, retrying", flush=True)
    time.sleep(0.2)

print("[flash] timed out waiting for the port", flush=True)
sys.exit(1)
