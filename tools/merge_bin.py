#!/usr/bin/env python3
"""Merge the V3F and V5F flash images into the single image WCH-Link expects.

Flash layout used by the WCH linker scripts:
  0x00000000  V3F image (Link_v3f.ld, 64 KiB region)
  0x00010000  V5F image (Link_v5f.ld, started by the V3F core via NVIC_WakeUp_V5F)
"""
import sys

V5F_OFFSET = 0x10000

def main():
    if len(sys.argv) != 4:
        print("usage: merge_bin.py v3f.bin v5f.bin merge.bin", file=sys.stderr)
        sys.exit(2)
    v3f = open(sys.argv[1], "rb").read()
    v5f = open(sys.argv[2], "rb").read()
    if len(v3f) > V5F_OFFSET:
        print(f"error: V3F image is {len(v3f)} bytes, larger than {V5F_OFFSET}", file=sys.stderr)
        sys.exit(1)
    img = v3f + b"\xff" * (V5F_OFFSET - len(v3f)) + v5f
    open(sys.argv[3], "wb").write(img)
    print(f"merge: V3F {len(v3f)} bytes @0x0, V5F {len(v5f)} bytes @0x{V5F_OFFSET:X}, total {len(img)} bytes -> {sys.argv[3]}")

if __name__ == "__main__":
    main()
