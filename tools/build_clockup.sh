#!/bin/bash
# Build build/clockup.bin: SystemInit-from-RAM helper used by tools/flash_blank.sh.
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"; cd "$P"
TC="${TOOLCHAIN_BIN:-/Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC12/bin}"
CLOCK="${CLOCK:-480}"
case "$CLOCK" in
  400) DEF=-DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSE=400000000;;
  480) DEF=-DSYSCLK_480M_CoreCLK_V5F_240M_V3F_120M_HSE=480000000;;
  600) DEF=-DSYSCLK_600M_CoreCLK_V5F_300M_V3F_150M_HSE=600000000;;
  *) echo "unknown CLOCK=$CLOCK"; exit 1;;
esac
ARCH="-march=rv32imac_zba_zbb_zbc_zbs_xw -mabi=ilp32 -msmall-data-limit=0"
INC="-Iv3f -Ilib/Core -Ilib/Debug -Ilib/Peripheral/inc -Ilib/Startup"
mkdir -p build/clockup
"$TC/riscv-wch-elf-gcc" $ARCH -Os -std=gnu99 -fsigned-char -ffunction-sections -fdata-sections -DCore_V3F $DEF $INC -c v3f/system_ch32h417.c -o build/clockup/system.o
"$TC/riscv-wch-elf-gcc" $ARCH -c tools/clockup/entry.S -o build/clockup/entry.o
"$TC/riscv-wch-elf-gcc" $ARCH -nostartfiles -nostdlib -Wl,--gc-sections -T tools/clockup/ram.ld build/clockup/entry.o build/clockup/system.o -o build/clockup.elf
"$TC/riscv-wch-elf-objcopy" -O binary build/clockup.elf build/clockup.bin
"$TC/riscv-wch-elf-nm" -u build/clockup.elf | sed 's/^/undefined: /'
"$TC/riscv-wch-elf-size" build/clockup.elf
echo "built build/clockup.bin ($(stat -f%z build/clockup.bin) bytes) for CLOCK=$CLOCK"
