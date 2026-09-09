/*
 * nanoCH32H417 USB 3.0 ISO mounter - V5F application core.
 *
 *   Picks an .iso / .img file on the microSD card and exposes it over USB 3.0
 *   as a virtual CD/DVD drive (or disk), streamed straight from the card's
 *   sectors; the card itself is exposed as a second drive.
 *
 * Debug output / console: USART1 (PA9/PA10) 115200 8N1 -> WCH-LinkE COM port.
 *
 * Copyright (C) 2026 wasdwasd0105
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file at the root of this project.
 */
#include "debug.h"
#include "app_config.h"
#include "sd_sdio.h"
#include "msc_core.h"
#include "image_store.h"
#include "usb_transport.h"
#include "console.h"
#include "display.h"
#include "leds.h"
#include "usbfs_host.h"
#include "systime.h"

int main(void)
{
    sd_err_t e;

    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);
    systime_init();
    console_init();
    leds_init();
#if LCD_ENABLE
    display_init();
#endif

    printf("\r\n[V5F] CH32H417 USB3 ISO mounter (build %s %s)\r\n", __DATE__, __TIME__);
    printf("[V5F] core %lu MHz, HCLK %lu MHz, chip id %08lX\r\n",
           (unsigned long)(SystemCoreClock / 1000000u), (unsigned long)(HCLKClock / 1000000u),
           (unsigned long)DBGMCU_GetCHIPID());
    printf("[V5F] reset cause:%s%s%s%s%s\r\n",
           RCC_GetFlagStatus(RCC_FLAG_PORRST) ? " power-on" : "", RCC_GetFlagStatus(RCC_FLAG_PINRST) ? " pin" : "",
           RCC_GetFlagStatus(RCC_FLAG_SFTRST) ? " software" : "", RCC_GetFlagStatus(RCC_FLAG_IWDGRST) ? " watchdog" : "",
           RCC_GetFlagStatus(RCC_FLAG_LKUPRSTF) ? " core-lockup" : "");
    RCC_ClearFlag();

    e = sd_init();
    if (e == SD_OK)
    {
        printf("[SD] %s S/N %08lX MID %02X: %lu MiB, %s, %s, SDIO_CK %lu.%lu MHz\r\n",
               sd_card.product, (unsigned long)sd_card.serial, sd_card.mid,
               (unsigned long)(sd_card.block_count / 2048u),
               sd_card.high_capacity ? "SDHC/SDXC" : "SDSC",
               sd_card.uhs_mode ? (sd_card.uhs_mode == 50 ? "UHS-I SDR50 1.8V" : sd_card.uhs_mode == 25 ? "UHS-I SDR25 1.8V" : "UHS-I SDR12 1.8V")
                                : sd_card.high_speed ? "High Speed 3.3V" : "Default Speed 3.3V",
               (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
        img_auto_mount();
    }
    else
    {
        printf("[SD] init failed: %s at %s (STA %08lX; will retry when the host polls)\r\n",
               sd_err_str(e), sd_last_stage, (unsigned long)sd_last_sta);
    }

    msc_init();
    usb_device_start();
    printf("[USB] device started, waiting for host\r\n");

    /* last, so the USB 3.0 / high-speed stack has already settled its PLLs */
    usbfs_host_init();

    msc_task();
    return 0;
}
