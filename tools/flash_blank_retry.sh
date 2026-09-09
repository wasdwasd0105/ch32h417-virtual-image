#!/bin/bash
# Loop tools/flash_blank.sh (clockup + halt-verify + hold flag) for the intermittent WCH-Link,
# until FLASH_BLANK_OK or the chip stops answering the debugger.  Usage: flash_blank_retry.sh <img> [tries]
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
IMG="${1:-build/merge.bin}"; TRIES="${2:-15}"; LOG="$PROJ/build/flash_blank_retry.log"; : > "$LOG"
log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
noans=0
for n in $(seq 1 "$TRIES"); do
    pkill -9 -f "openocd -f" 2>/dev/null; sleep 1
    out=$(timeout 180 tools/flash_blank.sh "$IMG" 2>&1)
    if echo "$out" | grep -q "FLASH_BLANK_OK"; then log "FLASHED on try $n"; exit 0; fi
    if echo "$out" | grep -q "successfully examined"; then
        noans=0; log "try $n: reached the chip but write failed: $(echo "$out" | grep -E 'Error|did not halt' | head -1 | cut -c1-90)"
    else
        noans=$((noans+1)); log "try $n: chip did not answer the debugger ($noans in a row)"
        [ "$noans" -ge 4 ] && { log "link wedged: needs RESET taps (attach_under_reset.sh) or a power cycle"; exit 2; }
    fi
    sleep 2
done
log "gave up after $TRIES tries"; exit 1
