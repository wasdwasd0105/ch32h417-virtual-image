#!/usr/bin/env python3
# Quick-format the FAT32 volume inside a raw image FILE (e.g. rwdisk.img on the card volume):
# both FATs, the root directory and FSInfo are rewritten; the boot sectors stay.
# Verifies every sector it wrote by reading it back uncached.
import os,struct,fcntl,sys
p=sys.argv[1]; label=(sys.argv[2] if len(sys.argv)>2 else "RWDISK").upper().ljust(11)[:11].encode()
fd=os.open(p,os.O_RDWR); fcntl.fcntl(fd,fcntl.F_NOCACHE,1)
mbr=os.pread(fd,512,0); part=struct.unpack_from("<I",mbr,0x1BE+8)[0]; base=part*512
bs=os.pread(fd,512,base)
bps,spc,rsvd,nfats=struct.unpack_from("<HBHB",bs,11); sects=struct.unpack_from("<I",bs,32)[0]
fatsz=struct.unpack_from("<I",bs,36)[0]; rootclus=struct.unpack_from("<I",bs,44)[0]; fsinfo=struct.unpack_from("<H",bs,48)[0]
assert bps==512 and rootclus==2, (bps,rootclus)
nclus=(sects-rsvd-nfats*fatsz)//spc; cb=spc*bps; data0=base+(rsvd+nfats*fatsz)*bps
fat=bytearray(fatsz*bps); struct.pack_into("<III",fat,0,0x0FFFFFF8,0x0FFFFFFF,0x0FFFFFFF)
root=bytearray(cb); root[0:11]=label; root[11]=0x08
fi=bytearray(os.pread(fd,512,base+fsinfo*bps)); struct.pack_into("<II",fi,0x1E8,nclus-1,3)
writes=[(base+(rsvd+k*fatsz)*bps,bytes(fat)) for k in range(nfats)]+[(data0,bytes(root)),(base+fsinfo*bps,bytes(fi))]
for attempt in range(3):
    for off,b in writes: os.pwrite(fd,b,off)
    os.fsync(fd)
    bad=[off for off,b in writes if os.pread(fd,len(b),off)!=b]
    if not bad: print(f"quick-format ok: {nclus} clusters x {cb//1024} KiB, label {label.decode().strip()}"); break
    print(f"attempt {attempt+1}: {len(bad)} region(s) read back different, rewriting")
else: sys.exit("quick-format FAILED to verify")
os.close(fd)
