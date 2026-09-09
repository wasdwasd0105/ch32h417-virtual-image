# Mini fsck for a FAT32 image read as a plain file (no privileges). Reports FSInfo vs real free count,
# FAT mirror consistency, dirty flags, deleted entries, cross-links and orphaned clusters.
import os,struct,fcntl,sys
p=sys.argv[1]; fd=os.open(p,os.O_RDONLY); fcntl.fcntl(fd,fcntl.F_NOCACHE,1)
mbr=os.pread(fd,512,0); part=struct.unpack_from("<I",mbr,0x1BE+8)[0]; base=part*512
bs=os.pread(fd,512,base)
bps,spc,rsvd,nfats=struct.unpack_from("<HBHB",bs,11); sects=struct.unpack_from("<I",bs,32)[0]
fatsz=struct.unpack_from("<I",bs,36)[0]; rootclus=struct.unpack_from("<I",bs,44)[0]; fsinfo=struct.unpack_from("<H",bs,48)[0]
nclus=(sects-rsvd-nfats*fatsz)//spc; cb=spc*bps; data0=base+(rsvd+nfats*fatsz)*bps
fi=os.pread(fd,512,base+fsinfo*bps); free_hint,next_free=struct.unpack_from("<II",fi,0x1E8)
fat=os.pread(fd,fatsz*bps,base+rsvd*bps); fat2=os.pread(fd,fatsz*bps,base+(rsvd+fatsz)*bps)
E=lambda i: struct.unpack_from("<I",fat,i*4)[0]&0x0FFFFFFF if 0<=i<nclus+2 else 0x0FFFFFF7
f1=E(1); print(f"FAT[1] flags: clean-shutdown={'yes' if f1&0x08000000 else 'NO (dirty)'}, no-disk-error={'yes' if f1&0x04000000 else 'NO'}")
print("FAT mirror:", "identical" if fat[:(nclus+2)*4]==fat2[:(nclus+2)*4] else "MISMATCH between FAT1 and FAT2")
free=sum(1 for i in range(2,nclus+2) if E(i)==0)
print(f"FSInfo hint {free_hint} free ({free_hint*cb/2**20:.0f} MiB), real free {free} of {nclus} ({free*cb/2**20:.0f} MiB)")
def cluster_off(c): return data0+(c-2)*cb
def chain(c):
    seen=[]; seenset=set()
    while 2<=c<0x0FFFFFF8:
        if c in seenset or len(seen)>nclus: return seen,"LOOP"
        seenset.add(c)
        if c>=nclus+2: return seen,"OUT-OF-RANGE"
        seen.append(c); c=E(c)
        if c==0: return seen,"ENDS-IN-FREE"
    return seen,"ok"
owner={}; problems=[]; deleted=[]; files=0; dirs=0
def walk(c,path):
    global files,dirs
    if not 2<=c<nclus+2: problems.append(f"dir {path}: start cluster {c} out of range"); return
    cl,st=chain(c)
    if st!="ok": problems.append(f"dir {path}: chain {st}")
    if path=="":
        for x in cl: owner[x]="/"
    for cc in cl:
        for ent in range(cb//32):
            e=os.pread(fd,32,cluster_off(cc)+ent*32)
            if e[0]==0: return
            name=e[:11].decode('latin1'); attr=e[11]
            if attr==0x0F: continue
            fc=(struct.unpack_from("<H",e,20)[0]<<16)|struct.unpack_from("<H",e,26)[0]; size=struct.unpack_from("<I",e,28)[0]
            if e[0]==0xE5: deleted.append((path+"/"+name.strip(),fc,size)); continue
            if name.strip() in (".",".."): continue
            if fc>=nclus+2: problems.append(f"{path}/{name.strip()}: first cluster {fc} out of range"); continue
            if fc: 
                ch,st=chain(fc)
                if st!="ok": problems.append(f"{path}/{name.strip()}: chain {st}")
                for x in ch:
                    if x in owner: problems.append(f"cross-link cluster {x}: {owner[x]} and {path}/{name.strip()}")
                    owner[x]=path+"/"+name.strip()
                if not attr&0x10 and (len(ch)*cb<size or (size and (len(ch)-1)*cb>=size)): problems.append(f"{path}/{name.strip()}: size {size} vs {len(ch)} clusters")
            if attr&0x10: dirs+=1; walk(fc,path+"/"+name.strip())
            else: files+=1
walk(rootclus,"")
used=sum(1 for i in range(2,nclus+2) if E(i)!=0); orphans=used-len(owner)
print(f"tree: {files} files, {dirs} dirs, {len(owner)} clusters referenced; FAT marks {used} used -> {orphans} orphaned clusters ({orphans*cb/2**20:.0f} MiB)")
for d in deleted[:8]: print(f"  deleted entry: {d[0]!r} first cluster {d[1]} size {d[2]}")
for pr in problems[:10]: print("  PROBLEM:", pr)
print("RESULT:", "consistent" if not problems and orphans==0 else "INCONSISTENT")
os.close(fd)
