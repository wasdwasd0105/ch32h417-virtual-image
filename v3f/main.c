/*
 * V3F boot core for the nanoCH32H417 USB 3.0 microSD card reader.
 *
 * The V3F core is the first to run after reset.  It configures the system
 * clocks (400 MHz SYSCLK, 100 MHz HCLK), raises the VIO18 I/O supply to
 * 3.3 V (the SD card pins PB10/PB11/PE8..PE11 live in that domain), then
 * wakes the V5F core at flash offset 0x10000 and goes to sleep.  Everything
 * else (USB, SD card, mass storage) runs on the V5F core.
 *
 * Copyright (C) 2026 wasdwasd0105
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file at the root of this project.
 */
#include "debug.h"

#ifndef APP_BOOT_DELAY_MS
#define APP_BOOT_DELAY_MS 2500
#endif

/* Flash-session hold: tools/flash_blank.tcl writes 0xF1A5F1A5 here right after
 * "reset halt" (the address comes from the ELF symbol).  While it is set the boot
 * core never wakes the V5F, so a half-written application can never crash and
 * reset the chip in the middle of a flash write.  Lives in a NOLOAD section that
 * the startup code does not zero; the script clears it before "reset run". */
#define DEBUG_HOLD_MAGIC 0xF1A5F1A5u
__attribute__((section(".debug_hold"), used)) volatile uint32_t debug_hold_flag;

static int debug_hold(void)
{
    if (debug_hold_flag != DEBUG_HOLD_MAGIC)
        return 0;
    printf("[V3F] debugger hold flag set at %p: NOT waking V5F.  The flash tool sets this and\r\n"
           "[V3F] clears it when it finishes; a session that died left it behind.  SRAM keeps it\r\n"
           "[V3F] across RESET -- POWER-CYCLE the board (both cables) to clear it.\r\n",
           (void *)&debug_hold_flag);
    while (1)
    {
        Delay_Ms(3000);
        printf("[V3F] still held by the debugger hold flag: power-cycle to clear\r\n");
    }
}
/* VIO18 level for the SD-card pins: 3 = 3.3 V (default), 2 = 2.5 V, 1 = 1.8 V,
 * -1 = leave the power-on strap value untouched.  Overridable from the
 * Makefile (VIO18_LEVEL=...). */
#ifndef APP_VIO18_LEVEL
#define APP_VIO18_LEVEL 3
#endif

int main(void)
{
    SystemInit();
    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    printf("\r\n[V3F] CH32H417 USB3 ISO mounter boot core (build %s %s)\r\n", __DATE__, __TIME__);
    printf("[V3F] SYSCLK=%lu HCLK=%lu VIO18 strap=%d, waiting %u ms before start\r\n",
           (unsigned long)SystemClock, (unsigned long)HCLKClock,
           (int)PWR_GetVIO18InitialStatus(), (unsigned)APP_BOOT_DELAY_MS);

    /* Debug-attach window: give WCH-Link time to connect and halt the chip
     * after a power cycle before any I/O-domain / power configuration or the
     * second core is started (recovery aid during development). */
    for (unsigned t = 0; t < APP_BOOT_DELAY_MS; t += 50)
    {
        debug_hold();
        Delay_Ms(50);
    }
    debug_hold();

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR, ENABLE);
#if APP_VIO18_LEVEL >= 0
    /* SD card signalling is 3.3 V: the SD pins PB10/PB11/PE8..PE11 sit in the
     * VIO18 domain, which powers up at 1.8 V on this board (XO strap floating). */
    PWR_VIO18ModeCfg(PWR_VIO18CFGMODE_SW);
#if APP_VIO18_LEVEL == 3
    PWR_VIO18LevelCfg(PWR_VIO18Level_MODE3);   /* 011b = 3.3 V */
#elif APP_VIO18_LEVEL == 2
    PWR_VIO18LevelCfg(PWR_VIO18Level_MODE2);   /* 010b = 2.5 V */
#else
    PWR_VIO18LevelCfg(PWR_VIO18Level_MODE1);   /* 001b = 1.8 V */
#endif
    Delay_Ms(5);                                /* let VIO18 settle */
    printf("[V3F] VIO18 set to level %d\r\n", APP_VIO18_LEVEL);
#else
    printf("[V3F] VIO18 left at strap default\r\n");
#endif

    printf("[V3F] waking V5F @0x%08X\r\n", (unsigned)Core_V5F_StartAddr);
    NVIC_WakeUp_V5F(Core_V5F_StartAddr);

    /* Idle in SLEEP (WFI), never STOP. */
    while (1)
    {
        __WFI();
    }
}
