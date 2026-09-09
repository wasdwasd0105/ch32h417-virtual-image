#!/bin/bash
# For a chip that is ALREADY BLANK (a flash_blank attempt died after its mass erase): poll attach +
# write + verify WITHOUT the erase.  A write that fails at offset 0 left nothing behind, so it can
# simply be retried; a partial write (offset > 0) needs the normal erase+write loop again.
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"; IMG="${1:-build/merge.bin}"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
NM="${TOOLCHAIN_BIN:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC12/bin}/riscv-wch-elf-nm"
HOLD=$("$NM" build/v3f.elf 2>/dev/null | awk '/ debug_hold_flag$/{print "0x"$1}')
LOG="$PROJ/build/flash_writeonly.log"; : > "$LOG"; log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
end=$(( $(date +%s) + ${2:-600} )); n=0
log "write-only polling of $IMG (no erase; hold flag ${HOLD:-none})"
while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1)); pkill -9 -f "openocd -f" 2>/dev/null
    out=$(timeout 150 "$OO/openocd" -f tools/wch-h417.cfg -c "set IMG \"$IMG\"" ${HOLD:+-c "set HOLD $HOLD"} -f build/flash_window_writeonly.tcl 2>&1)
    if echo "$out" | grep -q "successfully examined"; then
        log "attempt $n: chip answered"; echo "$out" | grep -E "hold flag|FLASH_BLANK|Error|after clockup|pc |0x40021000" | head -8 | tee -a "$LOG"
        if echo "$out" | grep -q "FLASH_BLANK_OK"; then log "FLASHED on attempt $n"; exit 0; fi
        if echo "$out" | grep -q "at offset 0x0000[1-9a-fA-F]\|at offset 0x000[1-9a-fA-F]"; then log "PARTIAL write: run the erase+write loop instead"; exit 2; fi
    fi
done
log "gave up after $n attempts"; exit 1
