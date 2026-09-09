#!/usr/bin/env python3
# Filesystem stress for the writable image: many files of mixed sizes, deletes, appends and one
# large file, each fsync'ed -- the pattern of a Finder copy.  "write" records a manifest;
# "verify" re-reads every file uncached and compares it with the deterministic content.
#   fs_stress.py write  <mountpoint> <seed> <manifest.json>
#   fs_stress.py verify <mountpoint> <seed> <manifest.json>
import os,sys,json,random,fcntl,time
mode,mnt,seed,man=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4]
SIZES=[4096,65536,1<<20,5<<20,20<<20]
def gen(idx,n): return random.Random(seed*1000003+idx).randbytes(n)
def wr(path,parts,append=False):
    fl=os.O_WRONLY|os.O_CREAT|(os.O_APPEND if append else os.O_TRUNC)
    fd=os.open(path,fl,0o644); fcntl.fcntl(fd,fcntl.F_NOCACHE,1)
    for idx,n in parts:
        b=gen(idx,n); off=0
        while off<n: off+=os.write(fd,b[off:off+(1<<20)])
    os.fsync(fd); os.close(fd)
d=os.path.join(mnt,"stress")
if mode=="write":
    rng=random.Random(seed); files={}; t0=time.time(); total=0
    for k in range(4): os.makedirs(f"{d}/d{k}",exist_ok=True)
    def mk(i):
        global total
        n=rng.choice(SIZES); p=f"{d}/d{i%4}/f{i:04d}.bin"; wr(p,[(i,n)]); files[p]=[[i,n]]; total+=n
    for i in range(120): mk(i)
    for p in list(files)[::3]: os.unlink(p); del files[p]
    for i in range(120,200): mk(i)
    big=f"{d}/big.bin"; wr(big,[(9999,256<<20)]); files[big]=[[9999,256<<20]]; total+=256<<20
    for j,p in enumerate(list(files)[5::11][:10]):
        n=rng.choice(SIZES[:3]); wr(p,[(10000+j,n)],append=True); files[p].append([10000+j,n]); total+=n
    for p in list(files)[1::7]: os.unlink(p); del files[p]
    json.dump(files,open(man,"w"))
    dt=time.time()-t0; print(f"wrote {total/2**20:.0f} MiB in {dt:.1f} s ({total/2**20/dt:.1f} MiB/s), {len(files)} files kept")
else:
    files=json.load(open(man)); ok=bad=0; t0=time.time(); nbytes=0
    for p,parts in files.items():
        try:
            fd=os.open(p,os.O_RDONLY); fcntl.fcntl(fd,fcntl.F_NOCACHE,1)
        except OSError as e: print(f"  MISSING {p}: {e}"); bad+=1; continue
        pos=0; good=True
        for idx,n in parts:
            exp=gen(idx,n); got=b""
            while len(got)<n:
                c=os.read(fd,n-len(got))
                if not c: break
                got+=c
            if got!=exp:
                m=next((k for k in range(min(len(got),n)) if got[k]!=exp[k]),min(len(got),n))
                print(f"  BAD {os.path.relpath(p,mnt)}: differs at byte {pos+m} of {sum(x[1] for x in parts)} (part {idx}, got {len(got)}/{n})"); good=False; break
            pos+=n; nbytes+=n
        if os.read(fd,1): print(f"  BAD {os.path.relpath(p,mnt)}: longer than expected"); good=False
        os.close(fd); ok+=good; bad+=not good
    extra=[os.path.join(r,f) for r,_,fs in os.walk(d) for f in fs if os.path.join(r,f) not in files and not f.startswith("._")]
    for p in extra[:5]: print(f"  UNEXPECTED {os.path.relpath(p,mnt)}")
    dt=time.time()-t0; print(f"verify: {ok} ok, {bad} bad, {len(extra)} unexpected; read {nbytes/2**20:.0f} MiB in {dt:.1f} s")
    sys.exit(1 if bad or extra else 0)
