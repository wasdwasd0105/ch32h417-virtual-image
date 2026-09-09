#!/bin/bash
# Poll until the WCH-Link can examine the chip, then program $1 with tools/flash_blank.sh and print the boot log.
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"; IMG="${1:-build/merge.bin}"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
LOG="$PROJ/build/flash_when_up.log"; : > "$LOG"; log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
end=$(( $(date +%s) + ${2:-600} )); n=0
log "waiting for the chip to answer the debugger (plug the USB 3.0 cable in: it powers the board)"
while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1)); pkill -9 -f "openocd -f" 2>/dev/null
    if timeout 30 "$OO/openocd" -f tools/wch-h417.cfg -c init -c shutdown 2>&1 | grep -q "successfully examined"; then
        log "chip answered on poll $n; flashing $IMG"; sleep 1
        tools/flash_blank.sh "$IMG" 2>&1 | grep -E "hold flag|FLASH_BLANK|Error" | tee -a "$LOG"
        grep -q "FLASH_BLANK_OK" "$LOG" || { log "flash failed; retrying in 5 s"; sleep 5; continue; }
        sleep 10; PORT=$(ls /dev/cu.usbmodem* | head -1)
        python3 - "$PORT" <<'PY' | tee -a "$LOG"
import serial,sys,time
s=serial.Serial(sys.argv[1],115200,timeout=0.1); s.reset_input_buffer(); s.write(b'r'); s.flush(); time.sleep(9)
for l in s.read(200000).decode('latin1').splitlines():
    if any(k in l for k in ("[V3F] VIO18","[V3F] debugger","[LCD]","[SD] GF","[img]","[USB]")): print("  "+l.strip()[:170])
PY
        log "done"; exit 0
    fi
    sleep 4
done
log "timed out"; exit 1
