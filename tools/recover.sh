#!/bin/bash
# Wait for a REAL power cycle (LinkE port disappears, then reappears), then
# program once with tools/flash_once.sh and report the card status.
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"
LOG="$PROJ/build/recover.log"; : > "$LOG"
log(){ echo "[$(date +%T)] $*" | tee -a "$LOG"; }
end=$(( $(date +%s) + 3600 ))
if ls /dev/cu.usbmodem* >/dev/null 2>&1; then
    log "unplug BOTH cables now (waiting for the LinkE port to disappear)..."
    until ! ls /dev/cu.usbmodem* >/dev/null 2>&1 || [ "$(date +%s)" -gt "$end" ]; do sleep 1; done
    ls /dev/cu.usbmodem* >/dev/null 2>&1 && { log "timed out waiting for unplug"; exit 1; }
    log "board unplugged"
fi
log "now plug it back in (LinkE first, then USB 3)..."
until ls /dev/cu.usbmodem* >/dev/null 2>&1 || [ "$(date +%s)" -gt "$end" ]; do sleep 1; done
ls /dev/cu.usbmodem* >/dev/null 2>&1 || { log "timed out waiting for replug"; exit 1; }
log "board back; settling 4 s, then flashing"
sleep 4
tools/flash_once.sh 2>&1 | tee -a "$LOG"
if grep -q FLASHED "$LOG"; then
    log "flash OK; capturing card status + streaming self-test"
    sleep 4; timeout 30 python3 tools/console.py "st" 12 2>&1 | grep -vE "^\s*$" | tee -a "$LOG"
else
    log "flash FAILED (see above)"
fi
log "done"
