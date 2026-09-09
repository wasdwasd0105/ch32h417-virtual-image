#!/bin/bash
# Program the V5F + V3F images and leave the chip running -- RESUMABLY.
#
# The WCH-LinkE flash write on this board is unreliable (random "error writing
# to flash" / libusb bulk timeouts, after which the LinkE is wedged until the
# board is power-cycled).  So every session:
#   1. verifies each image against flash and SKIPS the ones already correct,
#   2. never erases while a verified V5F is on the chip (every erase on this
#      driver is a MASS erase), and writes the boot core into the blank region,
#   3. writes the V5F image before the boot core, so a failed run never leaves
#      a bootable V3F next to a corrupt V5F (that pair crash-loops the chip and
#      makes attaches land mid-reset).
# Progress therefore survives a power cycle: re-run until it says FLASHED.
# Rules that still apply: "reset halt" not "halt"; never hammer (spaced
# retries, every session ends in "reset run" so the core is not left halted).
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
CFG="$PROJ/tools/wch-h417.cfg"
V3F="${V3F_BIN:-$PROJ/build/v3f.bin}"; V5F="${V5F_BIN:-$PROJ/build/v5f.bin}"
V5F_ADDR=$((0x00010000))
MAX_TRIES="${MAX_TRIES:-12}"; GAP="${GAP:-6}"; nc=0
[ -f "$V3F" ] && [ -f "$V5F" ] || { echo "missing $V3F / $V5F - run make first"; exit 1; }
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] && python3 -c "import serial,sys,time; s=serial.Serial(sys.argv[1],115200,timeout=0.1); s.write(b'r'); s.flush(); time.sleep(0.05); s.close()" "$PORT" 2>/dev/null

# Tcl body: per image "verify, else erase range + write + verify"; stops at the first error.
TCL="$PROJ/build/flash_resumable.tcl"
cat > "$TCL" <<TEOF
# EVERY erase on the wch_riscv driver is a mass erase -- "flash erase_sector A B"
# ignores the range and "flash write_image erase" wipes the chip before writing
# (both verified the hard way).  So: erase only when the V5F must be (re)written,
# write the V5F first, then write the boot core WITHOUT erasing into the region
# the mass erase left blank.  A boot-core write that fails verification (region
# not blank any more) falls back to the full sequence.  Progress survives a
# failed session as long as the V5F verifies.
init
reset halt
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
    echo "final check"
    verify_image "$V5F" $V5F_ADDR
    verify_image "$V3F" 0
    echo "FLASH_RESUMABLE_OK"
} err]} { echo "FLASH_RESUMABLE_ERROR: \$err" }
catch {reset run}
shutdown
TEOF

for n in $(seq 1 "$MAX_TRIES"); do
    pkill -9 -f "openocd -f" 2>/dev/null; sleep "$GAP"
    out=$(timeout 300 "$OO/openocd" -f "$CFG" -f "$TCL" 2>&1)
    printf '%s\n' "$out" > "$PROJ/build/flash_attempt.log"
    echo "$out" | grep -E "^V5F:|^V3F:" | sed "s/^/attempt $n: /"
    if echo "$out" | grep -q "FLASH_RESUMABLE_OK"; then
        echo "FLASHED on attempt $n"
        # the flash session usually leaves the core halted despite its own "reset run": start it from a fresh session
        sleep 2
        if timeout 60 "$OO/openocd" -f "$CFG" -c init -c "reset run" -c shutdown >/dev/null 2>&1; then echo "core started (reset run)"; else echo "reset run failed: power-cycle the board or run it by hand"; fi
        exit 0
    fi
    echo "attempt $n failed: $(echo "$out" | grep -E 'FLASH_RESUMABLE_ERROR|failed to connect with riscvchip|libusb' | head -1 | cut -c1-120)"
    if echo "$out" | grep -q "failed to connect with riscvchip"; then
        nc=$((nc+1))
        if [ "$nc" -ge 3 ]; then
            echo ">>> WCH-Link wedged: power-cycle the board (both cables), then re-run (progress is kept)."; exit 2
        fi
        sleep 8
    else
        nc=0
    fi
done
echo "gave up after $MAX_TRIES attempts (power-cycle the board, then re-run; progress is kept)"; exit 1
