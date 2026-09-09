/*
 * Status page for the ISO mounter on the ST7789 panel.  Rendered from the
 * main context only (idle loops between USB commands), one 12x24 text row at
 * a time, and only rows whose content changed -- a full row costs ~2 ms at
 * 50 MHz SPI, so a busy transfer loses well under 1% to the display.
 */
#include <stdio.h>
#include <string.h>
#include "ch32h417.h"
#include "app_config.h"
#include "display.h"
#include "lcd_st7789.h"
#include "sd_sdio.h"
#include "image_store.h"
#include "msc_core.h"
#include "usb_transport.h"
#include "systime.h"
#include "debug.h"

#define COLS 19
#define TITLE_TEXT   "Virtual Image Mounter"
#define TITLE_PITCH  11                            /* per-character advance in the title bar */
#define TITLE_X      ((LCD_W - (int)(sizeof(TITLE_TEXT) - 1) * TITLE_PITCH) / 2)
#define ROWS 10
#define ROW_Y(r) ((r) * 24)

#define C_BLACK  RGB565(0, 0, 0)
#define C_WHITE  RGB565(255, 255, 255)
#define C_GREY   RGB565(200, 200, 200)
#define C_DIM    RGB565(120, 120, 120)
#define C_TITLE  RGB565(0, 60, 150)
#define C_GREEN  RGB565(60, 220, 90)
#define C_YELLOW RGB565(255, 210, 40)
#define C_RED    RGB565(255, 60, 60)
#define C_CYAN   RGB565(60, 210, 240)
#define C_ORANGE RGB565(255, 150, 40)

static bool     ready;
static char     shown[ROWS][COLS + 1];
static uint16_t shown_fg[ROWS], shown_bg[ROWS];
static uint32_t last_draw_us, last_rate_us, last_rd, last_wr;
static uint32_t rd_kbps, wr_kbps;                 /* 1000-based KB/s */
static int      bar_rd_w = -1, bar_wr_w = -1;
#ifndef APP_VIO18_LEVEL
#define APP_VIO18_LEVEL 3
#endif
static bool     io_1v8 = (APP_VIO18_LEVEL == 1);  /* VIO18 level: boot core's setting, then as reported by the SD driver */
static uint32_t max_hz_sel = LCD_SPI_MAX_HZ;
static bool     diag;                             /* console 'D': keep cycling red/green/blue/page */
static uint8_t  diag_step;
static uint32_t diag_us;

/* draw a row only when its text or colours changed */
static void put_row(int r, const char *text, uint16_t fg, uint16_t bg)
{
    char buf[COLS + 1];
    int i = 0;
    while (i < COLS && text[i]) { buf[i] = text[i]; i++; }
    while (i < COLS) buf[i++] = ' ';
    buf[COLS] = 0;
    if (memcmp(buf, shown[r], COLS) == 0 && shown_fg[r] == fg && shown_bg[r] == bg)
        return;
    lcd_draw_text(LCD_MARGIN_X, ROW_Y(r), buf, fg, bg, 1);
    memcpy(shown[r], buf, COLS + 1);
    shown_fg[r] = fg;
    shown_bg[r] = bg;
}

static void fmt_rate(char *out, size_t n, const char *tag, uint32_t kbps)
{
    snprintf(out, n, "%s %3lu.%lu MB/s", tag, (unsigned long)(kbps / 1000u), (unsigned long)((kbps / 100u) % 10u));
}

static void bar(int y, uint32_t kbps, uint16_t color, int *last_w)
{
    int w = (int)((uint64_t)kbps * 216u / 50000u);   /* full scale 50 MB/s */
    if (w > 216) w = 216;
    if (w == *last_w)
        return;
    if (w > *last_w)
        lcd_fill_rect(12 + (*last_w < 0 ? 0 : *last_w), y, w - (*last_w < 0 ? 0 : *last_w), 8, color);
    else
        lcd_fill_rect(12 + w, y, *last_w - w, 8, C_BLACK);
    *last_w = w;
}

static void draw_frame(void)
{
    memset(shown, 0, sizeof shown);
    bar_rd_w = bar_wr_w = -1;
    lcd_clear(C_BLACK);
    /* title bar: 21 characters at an 11 px pitch (231 px) -- these glyphs leave
     * their 12th column blank, so condensing loses nothing */
    lcd_fill_rect(0, 0, LCD_W, 24, C_TITLE);
    lcd_draw_text_pitch(TITLE_X, 0, TITLE_TEXT, C_WHITE, C_TITLE, 1, TITLE_PITCH);
    last_draw_us = 0;                                 /* redraw the page at the next poll */
}

/* full-screen red / green / blue: an un-initialised panel stays white, a
 * panel that ignores DC shows noise -- tells the two failure modes apart */
void display_test_pattern(void)
{
    lcd_clear(RGB565(255, 0, 0)); Delay_Ms(500);
    lcd_clear(RGB565(0, 255, 0)); Delay_Ms(500);
    lcd_clear(RGB565(0, 0, 255)); Delay_Ms(500);
    draw_frame();
}

void display_reinit(void)
{
    if (io_1v8)
        return;                                       /* the panel cannot take commands at 1.8 V (tested: blank) */
    lcd_init();
    printf("[LCD] re-init on SPI2 at %lu.%lu MHz, CS/SCK/DC lines at %s\r\n",
           (unsigned long)(lcd_spi_hz() / 1000000u), (unsigned long)((lcd_spi_hz() / 100000u) % 10u), io_1v8 ? "1.8 V" : "3.3 V");
    draw_frame();
}

/* called by the SD driver right after it moved the VIO18 domain (the panel's
 * CS/SCK/DC lines live there): re-initialise at the new logic level */
/* The panel's CS/SCK/DC lines live in the VIO18 domain and only work at 3.3 V.
 * Before the SD driver drops the domain to 1.8 V (mode 2, UHS-I), leave a note
 * on the page -- the panel keeps showing that frame -- and stop touching the
 * panel; when the driver returns to 3.3 V, re-initialise and go live again. */
void sd_io_voltage_changing(bool v1v8)
{
    if (!ready || io_1v8 == v1v8 || !v1v8)
        return;
    put_row(2, "M2 UHS-I LCD paused", C_YELLOW, C_BLACK);
    put_row(9, "hold key to resume", C_DIM, C_BLACK);
}

void sd_io_voltage_changed(bool v1v8)
{
    if (!ready || io_1v8 == v1v8)
        return;
    io_1v8 = v1v8;
    if (!v1v8)
        display_reinit();
    else
        printf("[LCD] VIO18 at 1.8 V (mode 2): display paused\r\n");
}

/* console 'D': toggle the continuous test cycle (re-initialises the panel first) */
void display_toggle_diag(void)
{
    diag = !diag;
    diag_step = 0;
    diag_us = 0;
    display_reinit();
    printf("[LCD] test cycle %s\r\n", diag ? "ON: red / green / blue / status page, 1 s each" : "off");
}

/* console: cycle the SPI clock limit 50 -> 25 -> 12.5 -> 6.25 MHz and re-init */
void display_cycle_clock(void)
{
    max_hz_sel = max_hz_sel > 7000000u ? max_hz_sel / 2u : 50000000u;
    lcd_set_max_hz(max_hz_sel);
    display_reinit();
}

void display_init(void)
{
    lcd_init();
    memset(shown, 0, sizeof shown);
    draw_frame();                                     /* no colour test at boot: 'D' cycles it on demand */
    put_row(1, "booting...", C_GREY, C_BLACK);
    ready = true;
    printf("[LCD] ST7789 240x240 on SPI2 at %lu.%lu MHz, rotation %d, CS/SCK/DC lines at %s\r\n",
           (unsigned long)(lcd_spi_hz() / 1000000u), (unsigned long)((lcd_spi_hz() / 100000u) % 10u), LCD_ROTATION, io_1v8 ? "1.8 V (panel needs 3.3 V!)" : "3.3 V");
}

void display_poll(void)
{
    char line[40];
    uint32_t now;

    if (!ready || io_1v8)
        return;                                       /* mode 2: leave the frozen page alone */
    now = systime_us();
    if (diag)
    {
        static const uint16_t cols[3] = {RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255)};
        if (now - diag_us < 1000000u)
            return;
        diag_us = now;
        if (diag_step < 3)
        {
            lcd_clear(cols[diag_step++]);
            return;
        }
        diag_step = 0;
        draw_frame();                                 /* falls through: draw the page now */
    }
    if (now - last_draw_us < (uint32_t)DISPLAY_PERIOD_MS * 1000u)
        return;
    last_draw_us = now;

    /* transfer rates over the last interval (>= 500 ms) */
    if (now - last_rate_us >= 500000u)
    {
        uint32_t rd = msc_stat_read_sectors, wr = msc_stat_write_sectors, dt = now - last_rate_us;
        rd_kbps = (uint32_t)(((uint64_t)(rd - last_rd) * 512u * 1000u) / dt);
        wr_kbps = (uint32_t)(((uint64_t)(wr - last_wr) * 512u * 1000u) / dt);
        last_rd = rd; last_wr = wr; last_rate_us = now;
    }

    /* row 1-2: card */
    if (sd_card.present)
    {
        uint32_t gb10 = (uint32_t)(((uint64_t)sd_card.block_count * 512u) / 100000000u);   /* tenths of GB */
        snprintf(line, sizeof line, "SD  %s %lu.%luGB", sd_card.product, (unsigned long)(gb10 / 10u), (unsigned long)(gb10 % 10u));
        if (msc_sd_ejected())
            put_row(1, "SD  ejected by host", C_YELLOW, C_BLACK);
        else
            put_row(1, line, C_WHITE, C_BLACK);
        snprintf(line, sizeof line, "%s %s %lu.%luMHz",
                 sd_card.uhs_mode ? "M2" : "M1",
                 sd_card.uhs_mode ? "UHS-I 1.8V" : sd_card.high_speed ? "HS 3.3V" : "DS 3.3V",
                 (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
        put_row(2, line, C_GREY, C_BLACK);
    }
    else
    {
        put_row(1, "SD  no card", C_YELLOW, C_BLACK);
        put_row(2, "", C_GREY, C_BLACK);
    }

    /* row 3-4: mounted image */
    if (img_cur.mounted)
    {
        uint32_t mib = (uint32_t)(img_cur.size >> 20);
        snprintf(line, sizeof line, "%s %s", img_cur.kind == IMG_KIND_ISO ? "DVD" : "IMG", img_cur.name);
        put_row(3, line, C_WHITE, C_BLACK);
        uint32_t kp, kn;
        char pfx[8];
        img_key_pos(&kp, &kn);
        if (kn) snprintf(pfx, sizeof pfx, "%lu/%lu", (unsigned long)kp, (unsigned long)kn); else strcpy(pfx, "   ");
        if (mib >= 10240)
            snprintf(line, sizeof line, "%s %lu.%luGiB %s%s", pfx, (unsigned long)(mib / 1024u), (unsigned long)((mib % 1024u) * 10u / 1024u),
                     img_cur.kind == IMG_KIND_ISO ? "ISO" : img_fmt_tag(), img_cur.kind == IMG_KIND_ISO ? "" : img_cur.writable ? " RW" : " RO");
        else
            snprintf(line, sizeof line, "%s %luMiB %s%s", pfx, (unsigned long)mib,
                     img_cur.kind == IMG_KIND_ISO ? "ISO" : img_fmt_tag(), img_cur.kind == IMG_KIND_ISO ? "" : img_cur.writable ? " RW" : " RO");
        int un = msc_image_unacknowledged();
        if (un)
            put_row(4, un == 1 ? "host: eject old DVD" : "host: eject old IMG", C_YELLOW, C_BLACK);
        else
            put_row(4, line, C_GREY, C_BLACK);
    }
    else
    {
        uint32_t kp, kn;
        img_key_pos(&kp, &kn);
        if (sd_card.present && kn)
            snprintf(line, sizeof line, "0/%lu no image (key)", (unsigned long)kn);
        else
            strcpy(line, sd_card.present ? "no image mounted" : "");
        put_row(3, line, C_DIM, C_BLACK);
        {
            int un = msc_image_unacknowledged();     /* removed, but the host still shows it */
            put_row(4, un ? (un == 1 ? "host: eject old DVD" : "host: eject old IMG") : "", un ? C_YELLOW : C_GREY, C_BLACK);
        }
    }

    /* row 5: USB link */
    if (!usb_configured || (usb_active_xport == USB_XPORT_HS && usb_hs_suspended()))
        put_row(5, "USB waiting host", C_YELLOW, C_BLACK);
    else if (usb_active_xport == USB_XPORT_SS)
        put_row(5, "USB 3.0", C_GREEN, C_BLACK);
    else
        put_row(5, "USB 2.0", C_YELLOW, C_BLACK);

    /* rows 6-7: rates with bars, row 8: errors */
    fmt_rate(line, sizeof line, "R", rd_kbps);
    put_row(6, line, rd_kbps ? C_CYAN : C_DIM, C_BLACK);
    fmt_rate(line, sizeof line, "W", wr_kbps);
    put_row(7, line, wr_kbps ? C_ORANGE : C_DIM, C_BLACK);
    bar(ROW_Y(8) + 2, rd_kbps, C_CYAN, &bar_rd_w);
    bar(ROW_Y(8) + 14, wr_kbps, C_ORANGE, &bar_wr_w);
    /* row 9: the key hint, replaced by the SD error/retry counters once any occurred */
    if (sd_stat_crc_errors || sd_stat_retries)
    {
        snprintf(line, sizeof line, "err %lu retry %lu", (unsigned long)sd_stat_crc_errors, (unsigned long)sd_stat_retries);
        put_row(9, line, sd_stat_crc_errors ? C_RED : C_YELLOW, C_BLACK);
    }
    else
        put_row(9, "hold key: UHS1 mode", C_DIM, C_BLACK);
}
