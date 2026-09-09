#!/usr/bin/env python3
"""Dump the debug UART (USART1 -> WCH-LinkE virtual COM port) for a while."""
import glob, sys, time
import serial

def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port found")
    return ports[0]

def main():
    port = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith("/dev/") else find_port()
    seconds = float(sys.argv[-1]) if len(sys.argv) > 1 and not sys.argv[-1].startswith("/dev/") else 15.0
    with serial.Serial(port, 115200, timeout=0.2) as s:
        print(f"--- monitoring {port} @115200 for {seconds:.0f}s ---", flush=True)
        end = time.time() + seconds
        while time.time() < end:
            data = s.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()

if __name__ == "__main__":
    main()
