#!/usr/bin/env python3
"""Wrap a raw disk image into the virtual-disk formats the mounter understands, for testing:
   fixed VHD, dynamic VHD (2 MiB blocks, all-zero blocks left unallocated), monolithicSparse VMDK
   (64 KiB grains, all-zero grains left unallocated) and monolithicFlat VMDK (descriptor + -flat file).
   usage: mkvdisk.py raw.img outbase"""
import sys, struct, os, uuid, time
raw = open(sys.argv[1], "rb").read(); base = sys.argv[2]
size = len(raw); assert size % 512 == 0; sectors = size // 512

def chs(total):
    if total > 65535 * 16 * 255: total = 65535 * 16 * 255
    if total >= 65535 * 16 * 63:
        spt, heads = 255, 16; cyl_heads = total // spt
    else:
        spt = 17; cyl_heads = total // spt; heads = max((cyl_heads + 1023) // 1024, 4)
        if cyl_heads >= heads * 1024 or heads > 16:
            spt, heads = 31, 16; cyl_heads = total // spt
        if cyl_heads >= heads * 1024:
            spt, heads = 63, 16; cyl_heads = total // spt
    return cyl_heads // heads, heads, spt

def vhd_footer(disk_type, data_offset):
    c, h, s = chs(sectors)
    f = bytearray(512)
    struct.pack_into(">8sIIQI4sI4sQQHBBI", f, 0, b"conectix", 2, 0x10000, data_offset,
                     int(time.time()) - 946684800, b"py  ", 0x10000, b"Mac ", size, size, c, h, s, disk_type)
    f[68:84] = uuid.uuid4().bytes
    struct.pack_into(">I", f, 64, (~sum(f)) & 0xFFFFFFFF)
    return bytes(f)

# --- fixed VHD: raw + footer
open(base + "_fixed.vhd", "wb").write(raw + vhd_footer(2, 0xFFFFFFFFFFFFFFFF))

# --- dynamic VHD
BS = 2 << 20; spb = BS // 512; nblk = (size + BS - 1) // BS
bat_sectors = (nblk * 4 + 511) // 512
hdr = bytearray(1024)
struct.pack_into(">8sQQIII", hdr, 0, b"cxsparse", 0xFFFFFFFFFFFFFFFF, 1536, 0x10000, nblk, BS)
struct.pack_into(">I", hdr, 36, (~sum(hdr)) & 0xFFFFFFFF)
bat = bytearray(bat_sectors * 512); bat[:] = b"\xff" * len(bat)
out = bytearray(); out += vhd_footer(3, 512); out += hdr; out += bat
next_sector = 3 + bat_sectors
alloc = 0
for b in range(nblk):
    blk = raw[b * BS:(b + 1) * BS].ljust(BS, b"\0")
    if not any(blk): continue
    struct.pack_into(">I", bat, b * 4, next_sector)
    out += b"\xff" * 512 + blk          # sector bitmap (all present) + data
    next_sector += 1 + spb; alloc += 1
out[1536:1536 + len(bat)] = bat
out += vhd_footer(3, 512)
open(base + "_dyn.vhd", "wb").write(out)
print(f"dynamic VHD: {alloc}/{nblk} blocks allocated")

# --- monolithicSparse VMDK
GS = 128; GTES = 512
ngrains = (sectors + GS - 1) // GS; ngt = (ngrains + GTES - 1) // GTES
gd_sectors = (ngt * 4 + 511) // 512; gt_sectors = ngt * (GTES * 4 // 512)
desc_off, desc_size = 1, 20
rgd_off = desc_off + desc_size
gd_off = rgd_off + gd_sectors + gt_sectors
overhead = gd_off + gd_sectors + gt_sectors
overhead = (overhead + GS - 1) // GS * GS
c, h, s = sectors // (16 * 63), 16, 63
desc = (f'# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\ncreateType="monolithicSparse"\n\n'
        f'# Extent description\nRW {sectors} SPARSE "{os.path.basename(base)}_sparse.vmdk"\n\n'
        f'# The Disk Data Base\n#DDB\n\nddb.virtualHWVersion = "4"\nddb.geometry.cylinders = "{c}"\nddb.geometry.heads = "16"\n'
        f'ddb.geometry.sectors = "63"\nddb.adapterType = "ide"\n').encode()
hdr = bytearray(512)
struct.pack_into("<IIIQQQQIQQQBccccH", hdr, 0, 0x564D444B, 1, 3, sectors, GS, desc_off, desc_size, GTES,
                 rgd_off, gd_off, overhead, 0, b"\n", b" ", b"\r", b"\n", 0)
gts = bytearray(gt_sectors * 512)
next_sector = overhead; alloc = 0
grains = bytearray()
for g in range(ngrains):
    gr = raw[g * GS * 512:(g + 1) * GS * 512].ljust(GS * 512, b"\0")
    if not any(gr): continue
    struct.pack_into("<I", gts, g * 4, next_sector); grains += gr; next_sector += GS; alloc += 1
def gd(first_gt_sector):
    d = bytearray(gd_sectors * 512)
    for i in range(ngt): struct.pack_into("<I", d, i * 4, first_gt_sector + i * (GTES * 4 // 512))
    return d
out = bytearray(hdr) + desc.ljust(desc_size * 512, b"\0")
out += gd(rgd_off + gd_sectors) + gts + gd(gd_off + gd_sectors) + gts
out = out.ljust(overhead * 512, b"\0") + grains
open(base + "_sparse.vmdk", "wb").write(out)
print(f"sparse VMDK: {alloc}/{ngrains} grains allocated, overhead {overhead} sectors")

# --- monolithicFlat VMDK: descriptor + flat data file
flat = os.path.basename(base) + "_flat-flat.vmdk"
open(base + "_flat-flat.vmdk", "wb").write(raw)
open(base + "_flat.vmdk", "w").write(
    f'# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\ncreateType="monolithicFlat"\n\n'
    f'# Extent description\nRW {sectors} FLAT "{flat}" 0\n\n# The Disk Data Base\n#DDB\n\nddb.virtualHWVersion = "4"\n'
    f'ddb.geometry.cylinders = "{c}"\nddb.geometry.heads = "16"\nddb.geometry.sectors = "63"\nddb.adapterType = "ide"\n')
print("done")
