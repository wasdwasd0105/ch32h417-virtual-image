#!/usr/bin/env python3
"""Send console keys to the board over the WCH-LinkE COM port and print the replies.
usage: console.py "<keys>" [seconds]   e.g. console.py "3i t" 25  (space = 1 s pause)"""
import glob, sys, time
import serial

def main():
    keys = sys.argv[1] if len(sys.argv) > 1 else "h"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    with serial.Serial(port, 115200, timeout=0.1) as s:
        end = time.time() + seconds
        def pump(until):
            while time.time() < until:
                d = s.read(4096)
                if d:
                    sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
        for k in keys:
            if k == " ":
                pump(time.time() + 1.0)
            else:
                s.write(k.encode()); s.flush()
                pump(time.time() + 0.3)
        pump(end)
        print()

if __name__ == "__main__":
    main()
