#!/bin/bash
# Recovery when the chip no longer answers the debugger (garbage or nothing running,
# "WCH-Link failed to connect with riscvchip"): poll "init; halt; flash" as fast as
# possible while the user taps the board's RESET button; an attempt that lands right
# after a release catches the core before it runs away.  Programs $1 (merge image).
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"; IMG="${1:-build/merge.bin}"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
NM="${TOOLCHAIN_BIN:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC12/bin}/riscv-wch-elf-nm"
HOLD=$("$NM" build/v3f.elf 2>/dev/null | awk '/ debug_hold_flag$/{print "0x"$1}')
LOG="$PROJ/build/attach_under_reset.log"; : > "$LOG"; log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
[ -f build/clockup.bin ] || cp ../usb3_sdcard_reader/build/clockup.bin build/ 2>/dev/null
end=$(( $(date +%s) + ${2:-900} )); n=0
log "polling attach+flash of $IMG (hold flag ${HOLD:-none}); TAP RESET once, then WAIT ~20 s before the next tap (a tap during the write kills it)"
while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1)); pkill -9 -f "openocd -f" 2>/dev/null
    out=$(timeout 120 "$OO/openocd" -f tools/wch-h417.cfg -c "set IMG \"$IMG\"" ${HOLD:+-c "set HOLD $HOLD"} -f build/flash_window_blank.tcl 2>&1)
    if echo "$out" | grep -q "successfully examined"; then
        log "attempt $n: chip answered"; echo "$out" | grep -E "hold flag|FLASH_BLANK|Error" | head -4 | tee -a "$LOG"
        if echo "$out" | grep -q "FLASH_BLANK_OK"; then log "FLASHED on attempt $n"; exit 0; fi
    fi
done
log "gave up after $n attempts"; exit 1
