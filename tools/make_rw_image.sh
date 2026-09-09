#!/bin/bash
# Make a raw FAT32 (or exFAT) disk image for the mounter's writable-disk mode.
#   tools/make_rw_image.sh <size e.g. 4g|512m> <name.img> [dest_dir] [fat32|exfat]
# The mounter exposes a .img as a plain 512-byte-block disk; with "rw" on the
# second line of ISOMOUNT.TXT the host can write to it and the writes land
# inside the file on the card.
set -e
SIZE="$1"; NAME="$2"; DEST="$3"; FS="${4:-fat32}"
[ -n "$SIZE" ] && [ -n "$NAME" ] || { echo "usage: $0 <size> <name.img> [dest_dir] [fat32|exfat]"; exit 1; }
P="$(cd "$(dirname "$0")/.." && pwd)"; T="$P/build/images"; mkdir -p "$T"
VOL=$(echo "${NAME%.*}" | tr '[:lower:]' '[:upper:]' | cut -c1-11)
case "$FS" in fat32) FSARG="MS-DOS FAT32";; exfat) FSARG="ExFAT";; *) echo "fs must be fat32 or exfat"; exit 1;; esac
rm -f "$T/${NAME%.*}.dmg" "$T/${NAME%.*}.cdr" "$T/$NAME"
hdiutil create -quiet -size "$SIZE" -fs "$FSARG" -volname "$VOL" -layout MBRSPUD -o "$T/${NAME%.*}.dmg"
hdiutil convert -quiet "$T/${NAME%.*}.dmg" -format UDTO -o "$T/${NAME%.*}"
mv "$T/${NAME%.*}.cdr" "$T/$NAME"; rm -f "$T/${NAME%.*}.dmg"
ls -la "$T/$NAME" | awk '{print $5" bytes  "$9}'
if [ -n "$DEST" ]; then
    t0=$(date +%s); cp "$T/$NAME" "$DEST/" && sync; t1=$(date +%s)
    echo "copied to $DEST in $((t1-t0)) s"
    printf '%s\r\nrw\r\n' "$NAME" > "$DEST/ISOMOUNT.TXT" && sync
    echo "ISOMOUNT.TXT now selects $NAME read-write (takes effect at the next boot / console r)"
fi
