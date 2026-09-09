#!/bin/bash
# Wait for a real power cycle (LinkE port disappears, then reappears), then program
# build/merge_hold.bin (or $1) with tools/flash_blank.sh and print the boot log.
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"; IMG="${1:-build/merge.bin}"
LOG="$PROJ/build/recover_blank.log"; : > "$LOG"; log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
end=$(( $(date +%s) + 1800 ))
if ls /dev/cu.usbmodem* >/dev/null 2>&1; then
    log "unplug BOTH cables now..."
    until ! ls /dev/cu.usbmodem* >/dev/null 2>&1 || [ "$(date +%s)" -gt "$end" ]; do sleep 0.5; done
    log "board unplugged; plug it back in (LinkE first, then USB 3)"
fi
until ls /dev/cu.usbmodem* >/dev/null 2>&1 || [ "$(date +%s)" -gt "$end" ]; do sleep 0.5; done
ls /dev/cu.usbmodem* >/dev/null 2>&1 || { log "timed out"; exit 1; }
log "board back; flashing $IMG in 3 s"; sleep 3
pkill -9 -f "openocd -f" 2>/dev/null
tools/flash_blank.sh "$IMG" 2>&1 | grep -E "hold flag|FLASH_BLANK|Error" | tee -a "$LOG"
sleep 10
PORT=$(ls /dev/cu.usbmodem* | head -1)
python3 - "$PORT" <<'PY' | tee -a "$LOG"
import serial,sys,time
s=serial.Serial(sys.argv[1],115200,timeout=0.1); s.reset_input_buffer(); s.write(b'r'); s.flush(); time.sleep(9)
for l in s.read(200000).decode('latin1').splitlines():
    if any(k in l for k in ("[V3F] VIO18","[V3F] debugger","[LCD]","[SD] GF","[img]","[USB]")): print("  "+l.strip()[:170])
PY
log "done"
