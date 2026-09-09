/*
 * ST7789 240x240 driver: SPI2 master (mode 3, 8-bit, polled), GPIO DC/CS/RESET.
 * No framebuffer -- rectangles and glyphs are streamed straight to the panel.
 */
#include "ch32h417.h"
#include "debug.h"
#include "app_config.h"
#include "lcd_st7789.h"
#include "font12x24.h"
#include "system_ch32h417.h"

#define DC_HIGH()  GPIO_SetBits(GPIOD, GPIO_Pin_9)
#define DC_LOW()   GPIO_ResetBits(GPIOD, GPIO_Pin_9)
#define CS_HIGH()  GPIO_SetBits(GPIOB, GPIO_Pin_12)
#define CS_LOW()   GPIO_ResetBits(GPIOB, GPIO_Pin_12)
#define RST_HIGH() GPIO_SetBits(GPIOD, GPIO_Pin_8)
#define RST_LOW()  GPIO_ResetBits(GPIOD, GPIO_Pin_8)

static uint32_t spi_hz;
static uint32_t spi_max_hz = LCD_SPI_MAX_HZ;
static int      xoff, yoff;

void lcd_set_max_hz(uint32_t hz)
{
    spi_max_hz = hz ? hz : LCD_SPI_MAX_HZ;
}

static inline void spi_tx8(uint8_t b)
{
    while (!(SPI2->STATR & SPI_I2S_FLAG_TXE))
        ;
    SPI2->DATAR = b;
}

static void spi_wait_idle(void)
{
    while (!(SPI2->STATR & SPI_I2S_FLAG_TXE))
        ;
    while (SPI2->STATR & SPI_I2S_FLAG_BSY)
        ;
}

static void cmd(uint8_t c)
{
    spi_wait_idle();
    DC_LOW();
    spi_tx8(c);
    spi_wait_idle();
    DC_HIGH();
}

static void data(const uint8_t *d, unsigned n)
{
    while (n--)
        spi_tx8(*d++);
}

static void cmd_data(uint8_t c, const uint8_t *d, unsigned n)
{
    cmd(c);
    data(d, n);
}

static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t ca[4], ra[4];
    x0 += xoff; x1 += xoff; y0 += yoff; y1 += yoff;
    ca[0] = (uint8_t)(x0 >> 8); ca[1] = (uint8_t)x0; ca[2] = (uint8_t)(x1 >> 8); ca[3] = (uint8_t)x1;
    ra[0] = (uint8_t)(y0 >> 8); ra[1] = (uint8_t)y0; ra[2] = (uint8_t)(y1 >> 8); ra[3] = (uint8_t)y1;
    cmd_data(0x2A, ca, 4);          /* CASET */
    cmd_data(0x2B, ra, 4);          /* RASET */
    cmd(0x2C);                      /* RAMWR */
}

static void spi_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    SPI_InitTypeDef  spi  = {0};
    uint32_t div = 2, mode = 0;

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOB | RCC_HB2Periph_GPIOD | RCC_HB2Periph_AFIO, ENABLE);
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_SPI2, ENABLE);

    /* control lines: CS (PB12), RESET (PD8), DC (PD9) */
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_Init(GPIOD, &gpio);
    CS_HIGH(); DC_HIGH(); RST_HIGH();

    /* SPI2: SCK = PB13, MOSI = PB15 (both AF5) */
    gpio.GPIO_Pin  = GPIO_Pin_13 | GPIO_Pin_15;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &gpio);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource13, GPIO_AF5);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource15, GPIO_AF5);

    /* SCK = HCLK / 2^(mode+1): the largest rate not above LCD_SPI_MAX_HZ */
    while (mode < 7 && HCLKClock / div > spi_max_hz)
    {
        div <<= 1;
        mode++;
    }
    spi_hz = HCLKClock / div;

    spi.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode              = SPI_Mode_Master;
    spi.SPI_DataSize          = SPI_DataSize_8b;
    spi.SPI_CPOL              = SPI_CPOL_High;
    spi.SPI_CPHA              = SPI_CPHA_2Edge;
    spi.SPI_NSS               = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = (uint16_t)(mode << 3);
    spi.SPI_FirstBit          = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI2, &spi);
    SPI_Cmd(SPI2, ENABLE);
}

void lcd_init(void)
{
    static const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t pwctrl1[] = {0xA4, 0xA1};
    static const uint8_t pvgam[]   = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    static const uint8_t nvgam[]   = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    uint8_t v;

    SPI_Cmd(SPI2, DISABLE);
    spi_gpio_init();

    /* hardware reset */
    RST_LOW();  Delay_Ms(20);
    RST_HIGH(); Delay_Ms(120);

    CS_LOW();
    cmd(0x01); Delay_Ms(150);                          /* SWRESET */
    cmd(0x11); Delay_Ms(120);                          /* SLPOUT */
    v = 0x55; cmd_data(0x3A, &v, 1); Delay_Ms(10);     /* COLMOD: 16 bpp */
    /* MADCTL: orientation (LCD_ROTATION 0..3) and colour order */
    switch (LCD_ROTATION)
    {
        default: v = 0x00; xoff = 0;  yoff = 0;  break;  /* FPC at the bottom (RAM rows 0..239)   */
        case 1:  v = 0x60; xoff = 0;  yoff = 0;  break;  /* MX|MV                                 */
        case 2:  v = 0xC0; xoff = 0;  yoff = 80; break;  /* MX|MY: the 240 visible rows end at 319 */
        case 3:  v = 0xA0; xoff = 80; yoff = 0;  break;  /* MY|MV                                 */
    }
    if (LCD_BGR)
        v |= 0x08;
    cmd_data(0x36, &v, 1);
    cmd_data(0xB2, porctrl, sizeof porctrl);           /* PORCTRL */
    v = 0x35; cmd_data(0xB7, &v, 1);                   /* GCTRL */
    v = 0x19; cmd_data(0xBB, &v, 1);                   /* VCOMS */
    v = 0x2C; cmd_data(0xC0, &v, 1);                   /* LCMCTRL */
    v = 0x01; cmd_data(0xC2, &v, 1);                   /* VDVVRHEN */
    v = 0x12; cmd_data(0xC3, &v, 1);                   /* VRHS */
    v = 0x20; cmd_data(0xC4, &v, 1);                   /* VDVS */
    v = 0x0F; cmd_data(0xC6, &v, 1);                   /* FRCTRL2: 60 Hz */
    cmd_data(0xD0, pwctrl1, sizeof pwctrl1);           /* PWCTRL1 */
    cmd_data(0xE0, pvgam, sizeof pvgam);
    cmd_data(0xE1, nvgam, sizeof nvgam);
    cmd(0x21);                                         /* INVON: these IPS panels are inverted */
    cmd(0x13); Delay_Ms(10);                           /* NORON */
    spi_wait_idle();
    CS_HIGH();

    lcd_clear(0x0000);
    CS_LOW();
    cmd(0x29); Delay_Ms(20);                           /* DISPON, after the RAM is cleared */
    spi_wait_idle();
    CS_HIGH();
}

uint32_t lcd_spi_hz(void)
{
    return spi_hz;
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
    uint32_t n;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0)
        return;
    CS_LOW();
    set_window(x, y, x + w - 1, y + h - 1);
    for (n = (uint32_t)w * (uint32_t)h; n; n--)
    {
        spi_tx8(hi);
        spi_tx8(lo);
    }
    spi_wait_idle();
    CS_HIGH();
}

void lcd_clear(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_W, LCD_H, color);
}

void lcd_draw_text_pitch(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale, int pitch)
{
    uint8_t fh = (uint8_t)(fg >> 8), fl = (uint8_t)fg, bh = (uint8_t)(bg >> 8), bl = (uint8_t)bg;
    int gw, gh;

    if (scale < 1) scale = 1;
    gw = FONT_W * scale; gh = FONT_H * scale;
    if (pitch <= 0) pitch = gw;
    CS_LOW();
    for (; *s; s++, x += pitch)
    {
        unsigned ch = (unsigned char)*s;
        const uint16_t *g;
        int row, sy, col, sx;
        if (x + gw > LCD_W || y + gh > LCD_H)
            break;
        if (ch < 32 || ch > 126)
            ch = '?';
        g = font12x24[ch - 32];
        set_window(x, y, x + gw - 1, y + gh - 1);
        for (row = 0; row < FONT_H; row++)
        {
            uint16_t bits = g[row];
            for (sy = 0; sy < scale; sy++)
                for (col = 0; col < FONT_W; col++)
                {
                    bool on = (bits & (0x8000u >> col)) != 0;
                    for (sx = 0; sx < scale; sx++)
                    {
                        spi_tx8(on ? fh : bh);
                        spi_tx8(on ? fl : bl);
                    }
                }
        }
    }
    spi_wait_idle();
    CS_HIGH();
}

void lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    lcd_draw_text_pitch(x, y, s, fg, bg, scale, 0);
}
