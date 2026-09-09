#!/usr/bin/env python3
"""Throughput test through the mounted card volume (no admin rights needed).
Writes then reads a file with F_NOCACHE so the page cache is bypassed.
usage: bench.py /Volumes/Untitled [size_mib]"""
import fcntl, os, sys, time

def main():
    vol = sys.argv[1] if len(sys.argv) > 1 else "/Volumes/Untitled"
    mib = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    path = os.path.join(vol, "ch32h417_bench.bin")
    chunk = os.urandom(1 << 20)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
    t0 = time.time()
    for _ in range(mib):
        os.write(fd, chunk)
    os.fsync(fd); os.close(fd)
    tw = time.time() - t0
    print(f"write: {mib} MiB in {tw:.2f} s = {mib/tw:.1f} MB/s")
    fd = os.open(path, os.O_RDONLY)
    fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
    t0 = time.time(); n = 0; bad = 0
    while True:
        b = os.read(fd, 1 << 20)
        if not b:
            break
        if b != chunk[:len(b)]:
            bad += 1
        n += len(b)
    tr = time.time() - t0
    os.close(fd)
    print(f"read : {n/(1<<20):.0f} MiB in {tr:.2f} s = {n/(1<<20)/tr:.1f} MB/s, mismatching MiB blocks: {bad}")
    os.remove(path)

if __name__ == "__main__":
    main()
