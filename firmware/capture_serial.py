"""Wait for the ESP32-C3 USB CDC port to appear, then capture serial output.

ESP32-C3 powers down its USB Serial/JTAG peripheral during deep sleep, so the
board only enumerates after a reset/wake. Run this first, then press RST.

Usage: python capture_serial.py [seconds] [outfile]
"""
import sys
import time

import serial

DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 45
OUT = sys.argv[2] if len(sys.argv) > 2 else "serial_capture.log"


def find_port(timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for i in range(1, 13):
            name = f"COM{i}"
            try:
                s = serial.Serial(name, 115200, timeout=0.2)
                return name, s
            except Exception:
                continue
        time.sleep(0.5)
    return None, None


port, ser = find_port()
if not port:
    print("[capture] no serial port appeared within timeout", flush=True)
    sys.exit(1)

print(f"[capture] opened {port}, reading {DURATION}s -> {OUT}", flush=True)
lines = []
t0 = time.time()
with open(OUT, "w", encoding="utf-8", errors="replace") as fh:
    while time.time() - t0 < DURATION:
        try:
            raw = ser.readline()
        except Exception as exc:  # port dropped (board reset / sleep)
            fh.write(f"\n[capture] port error: {exc}\n")
            fh.flush()
            # try to reopen
            time.sleep(1)
            try:
                ser.close()
            except Exception:
                pass
            port2, ser = find_port(timeout=30)
            if not port2:
                fh.write("[capture] port did not come back\n")
                break
            port = port2
            fh.write(f"[capture] reopened {port}\n")
            fh.flush()
            continue
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        fh.write(text + "\n")
        fh.flush()
        print(text, flush=True)

try:
    ser.close()
except Exception:
    pass
print(f"[capture] done ({time.time() - t0:.1f}s)", flush=True)
