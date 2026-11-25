#!/usr/bin/env python3
"""Capture clawbot CSV lines from the USB Serial/JTAG console.

Usage:
    python tools/log_serial.py COM10 ../../../../patent/applications/data/2026-12-04/P2/P2_S01.csv

Requires: pip install pyserial
"""
import sys
import time

def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port, out_path = sys.argv[1], sys.argv[2]

    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial missing: pip install pyserial", file=sys.stderr)
        return 1

    ser = serial.Serial(port, 115200, timeout=0.2)
    t0 = time.time()
    n = 0
    try:
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            f.write("host_ms,line\n")
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                try:
                    line = raw.decode("utf-8", errors="replace").strip()
                except Exception:
                    continue
                if line.startswith("CSV,"):
                    f.write("%d,%s\n" % (int((time.time() - t0) * 1000), line))
                    n += 1
                elif line:
                    print(line)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("captured %d CSV rows -> %s" % (n, out_path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
