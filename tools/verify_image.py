#!/usr/bin/env python3
"""Check a mounted virtual CD/disk against the checksums recorded when the test
images were built, and measure sequential read throughput (page cache bypassed).

    tools/verify_image.py /Volumes/ISOTEST build/tests/md5s.txt
"""
import sys, os, hashlib, time, fcntl

def md5_nocache(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
    except OSError:
        pass
    h = hashlib.md5(); n = 0; t0 = time.time()
    while True:
        b = os.read(fd, 4 << 20)
        if not b:
            break
        h.update(b); n += len(b)
    os.close(fd)
    return h.hexdigest(), n, time.time() - t0

def main():
    vol, md5file = sys.argv[1], sys.argv[2]
    ok = bad = missing = 0
    for line in open(md5file):
        line = line.strip()
        if not line:
            continue
        want, path = line.split(None, 1)
        rel = path[2:] if path.startswith("./") else os.path.basename(path)
        full = os.path.join(vol, rel)
        if not os.path.exists(full):
            # ISO 9660 without Joliet may upper-case names; try a case-insensitive lookup
            d, b = os.path.split(full)
            cands = [f for f in os.listdir(d)] if os.path.isdir(d) else []
            m = [f for f in cands if f.lower() == b.lower()]
            if m:
                full = os.path.join(d, m[0])
        if not os.path.exists(full):
            print(f"MISSING  {rel}"); missing += 1; continue
        got, n, dt = md5_nocache(full)
        state = "ok " if got == want else "BAD"
        if got == want: ok += 1
        else: bad += 1
        speed = f"  {n/dt/1e6:.1f} MB/s" if n >= (8 << 20) and dt > 0 else ""
        print(f"{state}  {rel}  ({n} bytes){speed}")
    print(f"result: {ok} ok, {bad} bad, {missing} missing")
    sys.exit(0 if bad == 0 and missing == 0 else 1)

main()
