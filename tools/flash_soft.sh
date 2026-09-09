#!/bin/bash
# Flash without touching the board: the console's 'r' command does a full chip
# reset, the boot core then waits BOOT_DELAY_MS before waking the app core, and
# the attach-under-reset loop catches the chip in that window (the debugger
# cannot attach to the running app core).  Needs the firmware's console alive
# on the LinkE COM port.  Usage: tools/flash_soft.sh [image] [tries]
PROJ="$(cd "$(dirname "$0")/.." && pwd)"; cd "$PROJ"
IMG="${1:-build/merge.bin}"; TRIES="${2:-6}"
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "no LinkE COM port"; exit 1; }
tools/flashctl.sh kill >/dev/null 2>&1
tools/flashctl.sh start-tap "$IMG" | tail -1
python3 - "$PORT" "$TRIES" <<'PY'
import serial, sys, time, pathlib, re
log = pathlib.Path("build/attach_under_reset.log")
for attempt in range(1, int(sys.argv[2]) + 1):
    try:
        s = serial.Serial(sys.argv[1], 115200, timeout=0.1); s.write(b'r'); s.close()
        print("soft reset #%d" % attempt, flush=True)
    except Exception as e:
        print("serial:", e, flush=True)
    for _ in range(25):
        time.sleep(1)
        if re.search("FLASHED|gave up", log.read_text()):
            break
    if re.search("FLASHED|gave up", log.read_text()):
        break
t = log.read_text()
print("\n".join(l for l in t.splitlines() if re.search("attempt|FLASH|gave up|Error", l))[-500:])
sys.exit(0 if "FLASHED" in t else 1)
PY
rc=$?
tools/flashctl.sh kill >/dev/null 2>&1
exit $rc
