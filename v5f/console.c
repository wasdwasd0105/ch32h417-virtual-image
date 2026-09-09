/*
 * Single-character console on USART1 (115200 8N1) through the WCH-LinkE virtual
 * COM port.  Lists the images on the card and mounts one, shows status and
 * statistics, runs the SD self-test, resets the chip.
 */
#include "console.h"
#include "display.h"
#include "leds.h"
#include "debug.h"
#include "sd_sdio.h"
#include "msc_core.h"
#include "image_store.h"
#include "usb_transport.h"
#include "app_config.h"
#include "usbss_device.h"
#include "usbfs_host.h"
#include "systime.h"
#include <string.h>

__attribute__((aligned(16))) static uint8_t test_buf[2][16384];
static volatile int pending;        /* key waiting for console_run_pending() */

static void console_help(void)
{
    printf("  X  delete /.Spotlight-V100 on the card (repair aid when macOS refuses to mount the volume)\r\n");
    printf("  D  LCD: toggle a red/green/blue/page test cycle   V  LCD: halve the SPI clock (wraps to 50 MHz)\r\n");
    printf("  N  next image in /" IMG_SUBDIR " (0 = none) = a short press of the PC12 key; a long press = U\r\n");
    printf("  C  CRC-32 of the whole mounted image through the format mapping (compare with zlib.crc32 of the raw file)\r\n");
    printf("[con] USB link lab: Z=pause/resume supervisor  j/J=USBHS off/on  q/Q=USBSS off/on  W=reset+wait  R=hard reset  P=re-plug  L=re-insert the card LUN for the host (after ejecting it there)\r\n");
    printf("[con] l=list images  1-9=mount image N  e=eject (E=force)  s=status  p=stats  u=USB  k=USB-FS host  t=SD self-test  i=SD re-init  r=reset\r\n");
}

#if KEY_ENABLE
/* push button PC12 -> GND (internal pull-up): short press = next image, long press = mode toggle */
static uint32_t key_t0;
static bool     key_down, key_long_done;

static void key_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOC, ENABLE);
    gpio.GPIO_Pin  = GPIO_Pin_12;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &gpio);
}

static void key_poll(void)
{
    bool down = GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_12) == 0;
    uint32_t now = systime_us();
    if (!pending && img_deferred_due())
        pending = 'M';                              /* the swap gap is over: mount the selected image */
    if (down && !key_down)
    {
        key_down = true;
        key_t0 = now;
        key_long_done = false;
        return;
    }
    if (down && key_down && !key_long_done && now - key_t0 >= (uint32_t)KEY_LONG_MS * 1000u)
    {
        key_long_done = true;                       /* long press acts while still held */
        if (pending) { printf("[key] busy\r\n"); return; }
        sd_cfg.try_uhs ^= 1;
        printf(sd_cfg.try_uhs ? "[key] long press: mode 2 (UHS-I, full speed, display paused)\r\n"
                              : "[key] long press: mode 1 (LCD live, safe SD clock)\r\n");
        pending = 'i';
        return;
    }
    if (!down && key_down)
    {
        uint32_t held = now - key_t0;
        key_down = false;
        if (key_long_done || held < (uint32_t)KEY_DEBOUNCE_MS * 1000u)
            return;
        if (pending) { printf("[key] busy\r\n"); return; }
        printf("[key] short press: next image\r\n");
        pending = 'N';
    }
}
#endif

void console_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef usart = {0};
#if KEY_ENABLE
    key_init();
#endif

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO | RCC_HB2Periph_USART1 | RCC_HB2Periph_GPIOA, ENABLE);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF7);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF7);
    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpio);

    usart.USART_BaudRate = 115200;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
    systime_init();
    console_help();
}

static void console_sd_status(void)
{
    if (sd_card.present)
        printf("[SD] present: %s S/N %08lX MID %02X, %lu MiB, %s, %s, SDIO_CK %lu.%lu MHz, negedge %d\r\n",
               sd_card.product, (unsigned long)sd_card.serial, sd_card.mid,
               (unsigned long)(sd_card.block_count / 2048u),
               sd_card.high_capacity ? "SDHC/SDXC" : "SDSC",
               sd_card.uhs_mode == 50 ? "UHS-I SDR50 1.8V" : sd_card.uhs_mode == 25 ? "UHS-I SDR25 1.8V" :
               sd_card.uhs_mode == 12 ? "UHS-I SDR12 1.8V" : sd_card.high_speed ? "High Speed 3.3V" : "Default Speed 3.3V",
               (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u),
               sd_card.signal_1v8 ? sd_cfg.negedge_uhs : sd_cfg.negedge);
    else
        printf("[SD] no card\r\n");
    printf("[SD] crc_err %lu retries %lu fallbacks %lu fifo_err %lu | last stage: %s STA %08lX\r\n",
           (unsigned long)sd_stat_crc_errors, (unsigned long)sd_stat_retries,
           (unsigned long)sd_stat_fallbacks, (unsigned long)sd_stat_fifo_errors,
           sd_last_stage, (unsigned long)sd_last_sta);
    printf("[SD] at last snap: DCTRL %08lX DLEN %lu FIFOCNT %lu | DMA1ch1 CFGR %08lX CNTR %lu MADDR %08lX INTFR %08lX MUX0_3 %08lX\r\n",
           (unsigned long)sd_last_dctrl, (unsigned long)sd_last_dlen, (unsigned long)sd_last_fifocnt, (unsigned long)sd_last_dma_cfgr,
           (unsigned long)sd_last_dma_cntr, (unsigned long)sd_last_dma_maddr, (unsigned long)sd_last_dma_intfr, (unsigned long)sd_last_mux);
    img_print_status();
}

/* Sequential read test: 4 MiB streamed like the USB path, then re-read and compare 512 KiB */
static void console_selftest(void)
{
    const uint32_t lba0 = 2048, chunks = 256, per = 32;
    uint32_t t0, t1, errs = 0, mism = 0, done = 0;
    sd_err_t e;

    if (!sd_card.present) { printf("[test] no card\r\n"); return; }
    printf("[test] reading %lu MiB from LBA %lu at %lu.%lu MHz...\r\n", (unsigned long)(chunks * per / 2048u),
           (unsigned long)lba0, (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
    t0 = systime_us();
    {
        sd_stream_t st;
        e = sd_stream_begin(&st, lba0, chunks * per, false);
        if (e != SD_OK)
            errs++;
        else
        {
            for (uint32_t i = 0; i < chunks; i++)
            {
                e = sd_stream_push(&st, test_buf[i & 1], per);
                if (e != SD_OK) { errs++; break; }
                done++;
            }
            if (e == SD_OK) e = sd_stream_end(&st); else sd_stream_abort(&st);
            if (e != SD_OK) errs++;
        }
    }
    t1 = systime_us();
    {
        uint32_t us = t1 - t0 ? t1 - t0 : 1;
        uint32_t kbps = (uint32_t)(((uint64_t)done * per * 512u * 1000u) / us);
        printf("[test] %lu/%lu chunks ok, %lu failed, %lu ms -> %lu.%lu MB/s\r\n",
               (unsigned long)done, (unsigned long)chunks, (unsigned long)errs, (unsigned long)(us / 1000u),
               (unsigned long)(kbps / 1000u), (unsigned long)((kbps / 100u) % 10u));
    }
    for (uint32_t i = 0; i < 32 && sd_card.present; i++)
    {
        if (sd_read_blocks(test_buf[0], lba0 + i * per, per) != SD_OK ||
            sd_read_blocks(test_buf[1], lba0 + i * per, per) != SD_OK) { errs++; continue; }
        if (memcmp(test_buf[0], test_buf[1], per * 512u) != 0) mism++;
    }
    printf("[test] re-read compare: %lu mismatching chunks of 32\r\n", (unsigned long)mism);
}

/* One key, from the serial console or from a keyboard on the USB-FS host port. */
void console_feed(int c)
{
    switch (c)
    {
        case 's': console_sd_status(); usbfs_host_status(); break;
        case 'k': usbfs_host_status(); break;
        case 'p':
            printf("[stat] probe-read fails at the init clock (marginal card, harmless): %lu\r\n", (unsigned long)sd_stat_probe_errors);
            printf("[stat] cmds %lu, read %lu sectors, written %lu sectors | SD crc_err %lu retries %lu fallbacks %lu fifo_err %lu | write-behind: continued %lu, flushed %lu\r\n",
                   (unsigned long)msc_stat_commands, (unsigned long)msc_stat_read_sectors,
                   (unsigned long)msc_stat_write_sectors, (unsigned long)sd_stat_crc_errors,
                   (unsigned long)sd_stat_retries, (unsigned long)sd_stat_fallbacks, (unsigned long)sd_stat_fifo_errors,
                   (unsigned long)msc_stat_wb_continued, (unsigned long)msc_stat_wb_flushes);
            printf("[wb] run-end waits %lu: avg %lu us (to DATAEND %lu us) max %lu us | ready polls %lu in %lu waits: avg %lu us max %lu us\r\n",
                   (unsigned long)sd_stat_settle_n, (unsigned long)(sd_stat_settle_n ? sd_stat_settle_us / sd_stat_settle_n : 0),
                   (unsigned long)(sd_stat_settle_n ? sd_stat_dataend_us / sd_stat_settle_n : 0), (unsigned long)sd_stat_settle_max,
                   (unsigned long)sd_stat_ready_polls, (unsigned long)sd_stat_ready_n,
                   (unsigned long)(sd_stat_ready_n ? sd_stat_ready_us / sd_stat_ready_n : 0), (unsigned long)sd_stat_ready_max);
            break;
        case 'u':
        {
            static const char *names[12] = { "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive",
                                             "Polling", "Recovery", "HotReset", "Compliance", "Loopback" };
            printf("[usb] transport %s, configured %d, enum-state %d, SS link irqs %lu, LMP rx %lu, warm resets %lu\r\n",
                   usb_active_xport == USB_XPORT_SS ? "SuperSpeed" : usb_active_xport == USB_XPORT_HS ? "HighSpeed" : "none",
                   usb_configured, USB_Enum_Status, (unsigned long)usbss_link_irqs,
                   (unsigned long)usbss_link_lmp_rx, (unsigned long)usbss_link_warm_rst);
            {
                bool valid, att = usb_host_attached_probe(&valid);
                printf("[usb] USB 3 port D+/D-: %s\r\n", !valid ? "n/a (USB 2.0 controller on)" : att ? "pulled down (host attached)" : "floating (nothing attached)");
            }
            usb_link_dump();
            printf("[usb] SS link state changes:");
            for (int i = 0; i < 12; i++)
                if (usbss_link_state_hist[i])
                    printf(" %s=%u", names[i], usbss_link_state_hist[i]);
            printf("\r\n");
            break;
        }
        case 'U':
            sd_cfg.try_uhs ^= 1;
            printf(sd_cfg.try_uhs ? "[con] mode 2: UHS-I 1.8 V, full speed, display paused\r\n"
                                  : "[con] mode 1: LCD live, 3.3 V, safe SD clock\r\n");
            pending = 'i';
            break;
        case 'n':
            if (sd_card.signal_1v8) { sd_cfg.negedge_uhs ^= 1; printf("[con] NEGEDGE(1.8V)=%d, re-init\r\n", sd_cfg.negedge_uhs); }
            else { sd_cfg.negedge ^= 1; printf("[con] NEGEDGE(3.3V)=%d, re-init\r\n", sd_cfg.negedge); }
            if (!pending) pending = 'i';
            break;
        case 'r':
            printf("[con] reset\r\n");
            Delay_Ms(10);
            NVIC_SystemReset();
            break;
        case 'h': case '?': console_help(); break;
        case 'Z': case 'j': case 'J': case 'q': case 'Q': case 'W': case 'R': usb_sup_manual(c); break;
        /* everything that touches the card waits for an idle moment */
        case 'l': case 'e': case 'E': case 't': case 'i': case 'X': case 'D': case 'V': case 'N': case 'C': case 'P': case 'L':
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            if (pending)
                printf("[con] busy\r\n");
            else
                pending = c;
            break;
        default: break;
    }
}

void console_poll(void)
{
#if KEY_ENABLE
    key_poll();                                     /* before the early return: runs every idle-loop pass */
#endif
    leds_poll();
    if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == RESET)
        return;
    console_feed(USART_ReceiveData(USART1));
}

bool console_has_pending(void)
{
    return pending != 0;
}

void console_run_pending(void)
{
    int c = pending;
    if (!c)
        return;
    pending = 0;
    switch (c)
    {
        case 'l':
            if (!sd_card.present) { printf("[img] no card\r\n"); break; }
            if (img_scan() >= 0)
                img_print_list();
            break;
        case 'e': case 'E':
            if (!img_cur.mounted) { printf("[img] nothing mounted\r\n"); break; }
            if (msc_image_locked() && c == 'e')
            {
                printf("[img] the host has locked the medium (it is mounted there): eject it on the computer, or press E to force\r\n");
                break;
            }
            img_unmount();
            break;
        case 't': console_selftest(); break;
        case 'X':
            if (!sd_card.present) { printf("[img] no card\r\n"); break; }
            img_purge_tree("/.Spotlight-V100");
            break;
        case 'C': img_crc_check(); break;
        case 'N':
            img_cycle_next();                       /* the LUN goes empty; a polling host drops the old volume, a non-polling one gets the re-plug */
            break;
        case 'M':
            img_deferred_mount();                   /* the LUN comes back: not-ready-to-ready change, new volume */
            break;
        case 'P':
            printf("[usb] manual re-plug\r\n");
            usb_device_replug();
            break;
        case 'L':
            msc_sd_reinsert();
            printf("[MSC] card LUN re-inserted for the host: out for 2.5 s, then back\r\n");
            break;
#if LCD_ENABLE
        case 'D': display_toggle_diag(); break;
        case 'V': display_cycle_clock(); break;
#endif
        case 'i':
        {
            sd_err_t e = sd_reinit();
            if (e == SD_OK)
                printf("[SD] re-init: ok\r\n");
            else
                printf("[SD] re-init: %s at %s (STA %08lX)\r\n", sd_err_str(e), sd_last_stage, (unsigned long)sd_last_sta);
            if (e == SD_OK)
                img_auto_mount();
            break;
        }
        default:            /* '1'..'9' */
            if (!sd_card.present) { printf("[img] no card\r\n"); break; }
            if (msc_image_locked())
            {
                printf("[img] the host has locked the mounted image: eject it on the computer first (or E to force)\r\n");
                break;
            }
            if (img_count == 0 && img_scan() <= 0) { printf("[img] no images on the card\r\n"); break; }
            img_mount_index((uint32_t)(c - '1'), false);
            break;
    }
}
