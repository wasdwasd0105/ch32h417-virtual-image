#!/bin/bash
# Program a CH32H417 whose flash is blank or corrupt (no firmware running).
# See tools/flash_blank.tcl. Usage: tools/flash_blank.sh [image.bin]
P="$(cd "$(dirname "$0")/.." && pwd)"; cd "$P"
OO="${OPENOCD_DIR:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin}"
B="${1:-build/merge.bin}"
[ -f build/clockup.bin ] || tools/build_clockup.sh
pkill -9 -f openocd 2>/dev/null; sleep 1
HOLD=$("${TOOLCHAIN_BIN:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC12/bin}/riscv-wch-elf-nm" build/v3f.elf 2>/dev/null | awk '/ debug_hold_flag$/{print "0x"$1}')
[ -n "$HOLD" ] && echo "boot-core hold flag at $HOLD" || echo "(no hold flag symbol in build/v3f.elf: older boot core)"
timeout 240 "$OO/openocd" -f "${CFG:-tools/wch-h417.cfg}" -c "set IMG \"$B\"" ${HOLD:+-c "set HOLD $HOLD"} -f tools/flash_blank.tcl 2>&1 \
  | grep -vE "Listening|telnet|tcl server|gdb port|Licensed|bug reports|openocd.org|autoselect|already selected|Open On-Chip|Ready for|^Info : (WCH|wlink|clock|\[wch|starting)"
