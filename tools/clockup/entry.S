/* RAM helper: bring the V3F core up to full PLL speed (SystemInit), then stop in ebreak.
 * Loaded by tools/flash_blank.sh at 0x20140000 before the WCH-Link flash loader runs. */
    .section .text.entry,"ax",@progbits
    .globl _start
_start:
    .option push
    .option norelax
    la   sp, _estack
    .option pop
    call SystemInit
    ebreak
1:  j 1b

/* SystemInit() calls the vendor GPIO_IPD_Unused(); not needed here. */
    .globl GPIO_IPD_Unused
GPIO_IPD_Unused:
    ret
