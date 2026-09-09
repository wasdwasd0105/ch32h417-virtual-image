#!/bin/bash
# Manage the two flashing loops from a separate process, so the caller's command line never
# contains the loop script names (a grep from the calling shell would match itself).
#   flashctl.sh status | kill | start-retry [img] | start-tap [img] | wait [max_seconds]
cd "$(dirname "$0")/.."
PR='bash tools/flash_blank_retry[.]sh'; PT='bash tools/attach_under_reset[.]sh'
loops(){ ps -ax -o pid,command | grep -E "$PR|$PT" | grep -v grep; }
case "$1" in
  status)
    L=$(loops); [ -n "$L" ] && echo "$L" | cut -c1-80 || echo "no loop running"
    echo "retry: $(tail -1 build/flash_blank_retry.log 2>/dev/null | cut -c1-100)"
    echo "tap:   $(tail -1 build/attach_under_reset.log 2>/dev/null | cut -c1-100)";;
  kill)
    for p in $(loops | awk '{print $1}'); do kill "$p" 2>/dev/null; done; killall -9 openocd 2>/dev/null; echo "loops killed";;
  start-retry)
    "$0" kill >/dev/null; nohup tools/flash_blank_retry.sh "${2:-build/merge_400p.bin}" 14 > build/fbr_console.log 2>&1 &
    sleep 1; echo "retry loop started for ${2:-build/merge_400p.bin}";;
  start-tap)
    "$0" kill >/dev/null; sed 's/^    reset halt$/    halt/' tools/flash_blank.tcl > build/flash_window_blank.tcl
    nohup tools/attach_under_reset.sh "${2:-build/merge_400p.bin}" 1800 > build/aur_console.log 2>&1 &
    sleep 2; echo "tap loop started: $(head -1 build/attach_under_reset.log | cut -c1-80)";;
  wait)
    for i in $(seq 1 "${2:-100}"); do [ -z "$(loops)" ] && break; sleep 3; done; "$0" status;;
  *) echo "usage: $0 status|kill|start-retry|start-tap|wait";;
esac
