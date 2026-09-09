#!/bin/bash
# Build test images for the ISO mounter and (optionally) copy them to a card.
#   tools/make_test_images.sh [dest_dir]
# Produces build/tests/{test_iso.iso, test_img.img, iso_src/, md5s.txt}:
#   test_iso.iso  ISO 9660 + Joliet image (hdiutil makehybrid) of iso_src/: a 150 MiB random
#                 file for throughput checks, a text file and a small binary.
#   test_img.img  64 MiB raw FAT32 disk image (MBR) with one file, for the .img (disk) path.
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"; T="$P/build/tests"; rm -rf "$T"; mkdir -p "$T/iso_src/sub"
head -c $((150*1024*1024)) /dev/urandom > "$T/iso_src/big_random.bin"
head -c 12345 /dev/urandom > "$T/iso_src/sub/small.bin"
printf 'Hello from the CH32H417 ISO mounter test image.\nLine two.\n' > "$T/iso_src/readme.txt"
( cd "$T/iso_src" && find . -type f | sort | xargs md5 -r ) > "$T/md5s.txt"
hdiutil makehybrid -quiet -iso -joliet -default-volume-name ISOTEST -o "$T/test_iso.iso" "$T/iso_src"
# raw FAT32 disk image: create a dmg, add a file, convert to raw (.cdr), rename
hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -volname IMGTEST -layout MBRSPUD -o "$T/test_img.dmg"
DEV=$(hdiutil attach -nobrowse "$T/test_img.dmg" | awk '/\/Volumes\//{print $1; exit}')
MP=$(hdiutil info | awk -v d="$DEV" '$1==d{print $NF}')
printf 'file inside the raw disk image\n' > "$MP/inside.txt"
head -c $((8*1024*1024)) /dev/urandom > "$MP/rand8m.bin"; md5 -r "$MP/rand8m.bin" >> "$T/md5s.txt"
hdiutil detach -quiet "$DEV"
hdiutil convert -quiet "$T/test_img.dmg" -format UDTO -o "$T/test_img"; mv "$T/test_img.cdr" "$T/test_img.img"; rm -f "$T/test_img.dmg"
ls -la "$T"/*.iso "$T"/*.img
if [ -n "$1" ]; then
    cp "$T/test_iso.iso" "$T/test_img.img" "$1/" && sync && echo "copied to $1"
    ls -la "$1"/test_iso.iso "$1"/test_img.img
fi
