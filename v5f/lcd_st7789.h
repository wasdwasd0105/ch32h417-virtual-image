/*
 * 1.54" 240x240 IPS TFT (ST7789) on the nanoCH32H417's FPC-12P "SPI-LCD" connector.
 * Wiring (board schematic J8): DC=PD9, CS=PB12, SCK=PB13 (SPI2 AF5), SDA=PB15
 * (SPI2_MOSI AF5), RESET=PD8; the backlight is hard-wired on (LEDA=VDDIO).
 * Note: PB12/PB13/PD9 are in the chip's VIO18 I/O domain, PB15/PD8 in VDDIO.
 */
#ifndef LCD_ST7789_H
#define LCD_ST7789_H
#include <stdint.h>
#include <stdbool.h>

#define LCD_W 240
#define LCD_H 240
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

void     lcd_init(void);                                     /* safe to call again (re-init) */
void     lcd_set_max_hz(uint32_t hz);                        /* takes effect at the next lcd_init */
uint32_t lcd_spi_hz(void);                                   /* configured SCK */
void     lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void     lcd_clear(uint16_t color);
/* 12x24 font (scale 1) or 24x48 (scale 2); draws the background too */
void     lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
/* the same with an explicit per-character advance: a pitch below the glyph
 * width condenses the text (safe while the glyphs' rightmost columns are
 * blank -- the next character's background covers them anyway) */
void     lcd_draw_text_pitch(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale, int pitch);

#endif
