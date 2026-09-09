#!/bin/bash
# Flash by catching the chip in its BOOT WINDOW: the V3F boot core brings the PLL
# up, prints, and waits BOOT_DELAY_MS before waking the V5F.  A plain "halt" that
# lands inside that window gives the WCH-Link flash loader a core on the full
# clock (after "reset halt" the loader runs at the reset clock and drops words,
# which is why tools/flash_once.sh needs so many retries).  Polls continuously;
# press the board's RESET button (every ~5 s) until it reports FLASHED.
# Same resumable logic as flash_once.sh: verifies first, writes V5F then V3F.
#   usage: tools/flash_window.sh [max_seconds]
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
CFG="$PROJ/tools/wch-h417.cfg"; V3F="$PROJ/build/v3f.bin"; V5F="$PROJ/build/v5f.bin"; V5F_ADDR=$((0x00010000))
MAXS="${1:-600}"; LOG="$PROJ/build/flash_window.log"; : > "$LOG"
log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
TCL="$PROJ/build/flash_window.tcl"
cat > "$TCL" <<TEOF
init
halt
proc need {img addr} { return [expr {[catch {verify_image \$img \$addr}] != 0}] }
proc full_sequence {} {
    echo "full sequence: mass erase, V5F, V3F"
    flash erase_sector wch_riscv 0 last
    flash write_image "$V5F" $V5F_ADDR
    verify_image "$V5F" $V5F_ADDR
    flash write_image "$V3F" 0
    verify_image "$V3F" 0
}
if {[catch {
    if {[need "$V5F" $V5F_ADDR]} {
        full_sequence
    } elseif {[need "$V3F" 0]} {
        echo "V5F: already correct; V3F: writing into the blank boot region"
        if {[catch { flash write_image "$V3F" 0 ; verify_image "$V3F" 0 }]} {
            echo "V3F: region not blank any more"
            full_sequence
        }
    } else { echo "both images already correct" }
    verify_image "$V5F" $V5F_ADDR
    verify_image "$V3F" 0
    echo "FLASH_RESUMABLE_OK"
} err]} { echo "FLASH_RESUMABLE_ERROR: \$err" }
catch {reset run}
shutdown
TEOF
log "polling for the boot window (press RESET on the board every ~5 s); up to ${MAXS} s"
end=$(( $(date +%s) + MAXS )); n=0
while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1))
    out=$(timeout 120 "$OO/openocd" -f "$CFG" -f "$TCL" 2>&1)
    printf '%s\n' "$out" > "$PROJ/build/flash_attempt.log"
    if echo "$out" | grep -q "FLASH_RESUMABLE_OK"; then
        log "FLASHED on poll $n"; echo "$out" | grep -E "full sequence|already|wrote|verified" | tee -a "$LOG"
        sleep 2
        "$OO/openocd" -f "$CFG" -c init -c "reset run" -c shutdown >/dev/null 2>&1 && log "core started (reset run)" || log "reset run failed"
        exit 0
    fi
    if echo "$out" | grep -q "full sequence\|writing into"; then log "poll $n: reached the write but failed: $(echo "$out" | grep -E 'Error' | head -1 | cut -c1-100)"; fi
    if echo "$out" | grep -q "failed to connect with riscvchip"; then log "poll $n: link wedged (failed to connect) -- power-cycle the board (both cables) and keep this running"; sleep 3; fi
    sleep 0.2
done
log "gave up after ${MAXS} s ($n polls)"; exit 2
