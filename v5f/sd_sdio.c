/*
 * microSD card driver on the CH32H417 SDIO host controller.
 *
 * The CH32H417 SDIO block is register compatible with the classic STM32 SDIO
 * (POWER/CLKCR/ARG/CMD/RESP/DTIMER/DLEN/DCTRL/STA/ICR/MASK/FIFO).  Data moves
 * through DMA1 channel 1 (DMAMUX request 111 = SDIO).  All transfers are
 * blocking; the USB side runs from interrupts so this does not stall USB.
 */
#include "sd_sdio.h"
#include "app_config.h"
#include "debug.h"
#include "systime.h"
#include <string.h>

/* ---- register bits ----------------------------------------------------- */
#define CLKCR_CLKEN     (1u << 8)
#define CLKCR_PWRSAV    (1u << 9)
#define CLKCR_BYPASS    (1u << 10)
#define CLKCR_WIDBUS_4  (1u << 11)
#define CLKCR_NEGEDGE   (1u << 13)
#define CLKCR_HWFC_EN   (1u << 14)

#define CMD_WAITRESP_SHORT (1u << 6)
#define CMD_WAITRESP_LONG  (3u << 6)
#define CMD_CPSMEN         (1u << 10)

#define DCTRL_DTEN      (1u << 0)
#define DCTRL_DTDIR     (1u << 1)   /* 1 = card -> controller */
#define DCTRL_DMAEN     (1u << 3)
#define DCTRL_BLK512    (9u << 4)
#define DCTRL_BLK64     (6u << 4)

#define STA_CCRCFAIL    (1u << 0)
#define STA_DCRCFAIL    (1u << 1)
#define STA_CTIMEOUT    (1u << 2)
#define STA_DTIMEOUT    (1u << 3)
#define STA_TXUNDERR    (1u << 4)
#define STA_RXOVERR     (1u << 5)
#define STA_CMDREND     (1u << 6)
#define STA_CMDSENT     (1u << 7)
#define STA_DATAEND     (1u << 8)
#define STA_STBITERR    (1u << 9)
#define STA_DBCKEND     (1u << 10)
#define STA_TXACT       (1u << 12)
#define STA_RXACT       (1u << 13)
#define STA_RXDAVL      (1u << 21)
#define STA_STATIC_MASK 0x000007FFu   /* incl. STBITERR */
#define STA_DATA_ERR    (STA_DCRCFAIL | STA_DTIMEOUT | STA_TXUNDERR | STA_RXOVERR | STA_STBITERR)

/* R1 card status */
#define R1_ERROR_MASK   0xFDFFE008u
#define R1_APP_CMD      (1u << 5)
#define R1_READY_FOR_DATA (1u << 8)
#define R1_STATE(r)     (((r) >> 9) & 0xFu)
#define R1_STATE_TRAN   4u

#define OCR_BUSY        (1u << 31)
#define OCR_HCS         (1u << 30)
#define OCR_S18R        (1u << 24)   /* ACMD41: request 1.8 V signalling */
#define OCR_S18A        (1u << 24)   /* OCR: card accepts 1.8 V signalling */
#define OCR_VOLTAGE     0x00FF8000u

#define SD_DTIMER       0xFFFFFFFFu   /* hardware data timeout in SDIO_CK cycles (~85 s at 50 MHz) */
#define CMD_WAIT_US     1000000u      /* software limit waiting for a response (1 s) */
#define DATA_WAIT_US    5000000u      /* software limit for a data transfer (5 s)  */
#define BUSY_WAIT_US    5000000u      /* max programming busy after a write (5 s)   */
#define ACMD41_TRIES    2000u
#define DATA_POLL_LIMIT 0x02000000u   /* CMD6 status FIFO poll */

sd_card_t sd_card;
sd_config_t sd_cfg = { SD_CLKDIV_HIGHSPEED, SD_CLKDIV_HIGHSPEED_3V3, SD_CLKDIV_DEFAULT, SD_TRY_HIGH_SPEED, SD_NEGEDGE, 3, 1, SD_TRY_UHS, SD_NEGEDGE_UHS };
static bool bus_1v8;   /* VIO18 (the SD pins' I/O supply) currently at 1.8 V */
/* other users of the VIO18 domain (the kit's LCD) may want to know; weak so the reader project links without it */
__attribute__((weak)) void sd_io_voltage_changing(bool v1v8) { (void)v1v8; }   /* just before the level moves */
__attribute__((weak)) void sd_io_voltage_changed(bool v1v8) { (void)v1v8; }    /* after it settled */
uint8_t sd_cfg_user_div;   /* console overrode the dividers */
const char *sd_last_stage = "-";
uint32_t sd_last_sta, sd_last_dcount;
uint32_t sd_last_dma_cfgr, sd_last_dma_cntr, sd_last_dma_maddr, sd_last_dma_intfr, sd_last_mux, sd_last_fifocnt, sd_last_dctrl, sd_last_dlen;
#define STAGE(x) (sd_last_stage = (x))
#define SNAP() do { sd_last_sta = SDIO->STA; sd_last_dcount = SDIO->DCOUNT; sd_last_dma_cfgr = DMA1_Channel1->CFGR; sd_last_dma_cntr = DMA1_Channel1->CNTR; \
    sd_last_dma_maddr = DMA1_Channel1->MADDR; sd_last_dma_intfr = DMA1->INTFR; sd_last_mux = DMAMUX->CFGR0_3; sd_last_fifocnt = SDIO->FIFOCNT; \
    sd_last_dctrl = SDIO->DCTRL; sd_last_dlen = SDIO->DLEN; } while (0)
volatile uint32_t sd_stat_crc_errors, sd_stat_retries, sd_stat_fallbacks, sd_stat_cmd12_late, sd_last_cmd12_us, sd_stat_fifo_errors;

static uint32_t hclk_hz = 100000000u;
static uint32_t cur_clkdiv;

uint32_t sd_clkdiv_to_hz(uint32_t div)
{
    return hclk_hz / (div + 2u);
}

/* ------------------------------------------------------------------------ */
const char *sd_err_str(sd_err_t e)
{
    switch (e)
    {
        case SD_OK:              return "ok";
        case SD_ERR_NOCARD:      return "no card";
        case SD_ERR_TIMEOUT:     return "timeout";
        case SD_ERR_CRC:         return "crc";
        case SD_ERR_RESP:        return "bad response";
        case SD_ERR_PARAM:       return "bad parameter";
        case SD_ERR_UNSUPPORTED: return "unsupported card";
        case SD_ERR_DATA:        return "data error";
        case SD_ERR_BUSY:        return "busy";
        case SD_ERR_NEED_POWER_CYCLE: return "stuck in 1.8V switch (power-cycle the card)";
        default:                 return "?";
    }
}

/* ------------------------------------------------------------------------ */
static void sd_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOB | RCC_HB2Periph_GPIOE | RCC_HB2Periph_AFIO, ENABLE);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_SDIO, ENABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);

    /* CMD = PB10, CK = PB11 */
    gpio.GPIO_Pin   = GPIO_Pin_10 | GPIO_Pin_11;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(GPIOB, &gpio);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF8);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource11, GPIO_AF8);

    /* D0..D3 = PE8..PE11 */
    gpio.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11;
    GPIO_Init(GPIOE, &gpio);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource8,  GPIO_AF8);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource9,  GPIO_AF8);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource10, GPIO_AF8);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource11, GPIO_AF8);
}

static void sd_set_clock(uint32_t div, bool wide)
{
    uint32_t v = SDIO->CLKCR;
#if SD_MAX_HZ_3V3
    /* the 3.3 V bus does not run reliably at the top divider on this board */
    if (!sd_card.signal_1v8)
        while (div < 0xFFu && hclk_hz / (div + 2u) > (uint32_t)SD_MAX_HZ_3V3)
            div++;
#endif
    v &= ~(0xFFu | CLKCR_WIDBUS_4 | (1u << 12) | CLKCR_BYPASS | CLKCR_PWRSAV | CLKCR_NEGEDGE | CLKCR_HWFC_EN);
    v |= (div & 0xFFu) | CLKCR_CLKEN;
    if (wide)
        v |= CLKCR_WIDBUS_4;
    if (sd_card.signal_1v8 ? sd_cfg.negedge_uhs : sd_cfg.negedge)
        v |= CLKCR_NEGEDGE;          /* sampling edge follows the signalling mode */
    if (sd_cfg.hwfc)
        v |= CLKCR_HWFC_EN;
    SDIO->CLKCR = v;
    cur_clkdiv = div;
    sd_card.clk_hz = hclk_hz / (div + 2u);
}

/* The slowest divider this card has needed since it was inserted.  A marginal
 * card would otherwise re-discover its data errors after every re-init (each
 * rediscovery is a burst of CRC failures), because sd_reinit() puts the High
 * Speed divider back to full speed.  Identification still runs at the full
 * divider -- the only setting that works on this board -- and the floor is
 * applied straight afterwards, on the path that is known to be clean. */
static uint8_t  sd_learned_div;
static uint32_t sd_learned_serial;

/* The post-init probe transfer below is a deliberate internal step, not host
 * traffic: on a marginal card it fails at the init clock and used to charge the
 * user-visible counters (always "crc_err 2 retries 2") and trigger a fallback,
 * even though the clock is about to be set explicitly anyway.  While this is
 * set, errors are neither counted nor acted on. */
static bool     sd_quiet_probe;
uint32_t        sd_stat_probe_errors;   /* probe transfers that failed at the init clock */

/* slow the bus down one notch after repeated data errors; returns false at the floor */
static bool sd_fallback_clock(void)
{
    uint32_t nd = cur_clkdiv == 0 ? 1u : (cur_clkdiv < 2 ? 2u : cur_clkdiv * 2u);
    if (sd_quiet_probe)
        return false;                               /* the probe sets the clock itself */
    if (nd > 16u)
        return false;
    sd_set_clock(nd, true);
    if (nd > sd_learned_div)
    {
        sd_learned_div = (uint8_t)nd;
        sd_learned_serial = sd_card.serial;
    }
    sd_stat_fallbacks++;
    printf("[SD] data errors: SDIO_CK lowered to %lu.%lu MHz\r\n",
           (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
    return true;
}

/* ------------------------------------------------------------------------ */
/* Send a command.  resp_words: 0 = no response, 1 = short (R1/R3/R6/R7),
 * 4 = long (R2).  ignore_crc for R3 (OCR) whose CRC field is fixed 0x7F.   */
static sd_err_t sd_cmd(uint8_t idx, uint32_t arg, int resp_words, bool ignore_crc, uint32_t *resp)
{
    uint32_t sta, t0 = systime_us();

    SDIO->ICR = STA_STATIC_MASK;
    SDIO->ARG = arg;
    SDIO->CMD = (idx & 0x3Fu) | CMD_CPSMEN |
                (resp_words == 0 ? 0u : (resp_words == 4 ? CMD_WAITRESP_LONG : CMD_WAITRESP_SHORT));

    if (resp_words == 0)
    {
        do
        {
            sta = SDIO->STA;
            if (systime_elapsed_us(t0) > CMD_WAIT_US)
                return SD_ERR_TIMEOUT;
        } while (!(sta & (STA_CMDSENT | STA_CTIMEOUT)));
        SDIO->ICR = STA_STATIC_MASK;
        return (sta & STA_CTIMEOUT) ? SD_ERR_TIMEOUT : SD_OK;
    }

    do
    {
        sta = SDIO->STA;
        if (systime_elapsed_us(t0) > CMD_WAIT_US)
        {
            SDIO->ICR = STA_STATIC_MASK;
            return SD_ERR_TIMEOUT;
        }
    } while (!(sta & (STA_CMDREND | STA_CCRCFAIL | STA_CTIMEOUT)));

    if (sta & STA_CTIMEOUT)
    {
        SDIO->ICR = STA_CTIMEOUT;
        return SD_ERR_TIMEOUT;
    }
    if ((sta & STA_CCRCFAIL) && !ignore_crc)
    {
        SDIO->ICR = STA_CCRCFAIL;
        return SD_ERR_CRC;
    }
    SDIO->ICR = STA_STATIC_MASK;

    if (resp)
    {
        resp[0] = SDIO->RESP1;
        if (resp_words == 4)
        {
            resp[1] = SDIO->RESP2;
            resp[2] = SDIO->RESP3;
            resp[3] = SDIO->RESP4;
        }
    }
    return SD_OK;
}

/* R1 command: checks response index and card error bits */
static sd_err_t sd_cmd_r1(uint8_t idx, uint32_t arg, uint32_t *r1)
{
    uint32_t r;
    sd_err_t e = sd_cmd(idx, arg, 1, false, &r);
    if (e != SD_OK)
        return e;
    if ((SDIO->RESPCMD & 0x3Fu) != idx)
        return SD_ERR_RESP;
    if (r & R1_ERROR_MASK)
        return SD_ERR_RESP;
    if (r1)
        *r1 = r;
    return SD_OK;
}

static sd_err_t sd_acmd_r1(uint8_t idx, uint32_t arg, uint32_t *r1)
{
    uint32_t r;
    sd_err_t e = sd_cmd_r1(55, (uint32_t)sd_card.rca << 16, &r);
    if (e != SD_OK)
        return e;
    if (!(r & R1_APP_CMD))
        return SD_ERR_RESP;
    return sd_cmd_r1(idx, arg, r1);
}

/* Wait until the card is back in TRANSFER state (write programming done). */
static sd_err_t sd_wait_transfer_state(uint32_t max_us)
{
    uint32_t r1, t0 = systime_us();
    for (;;)
    {
        sd_err_t e = sd_cmd_r1(13, (uint32_t)sd_card.rca << 16, &r1);
        if (e != SD_OK)
            return e;
        if (R1_STATE(r1) == R1_STATE_TRAN)
            return SD_OK;
        if (systime_elapsed_us(t0) > max_us)
            return SD_ERR_BUSY;
        Delay_Us(50);
    }
}

/* Is the card still answering?  Used before declaring it removed. */
static bool sd_still_present(void)
{
    uint32_t r1;
    for (int i = 0; i < 3; i++)
    {
        if (sd_cmd_r1(13, (uint32_t)sd_card.rca << 16, &r1) == SD_OK)
            return true;
        Delay_Us(200);
    }
    return false;
}

/* ------------------------------------------------------------------------ */
static void sd_dma_setup(void *mem, uint32_t bytes, bool to_card)
{
    DMA_InitTypeDef dma = {0};

    DMA_Cmd(DMA1_Channel1, DISABLE);
    DMA_DeInit(DMA1_Channel1);
    DMA_ClearFlag(DMA1, DMA1_FLAG_GL1 | DMA1_FLAG_TC1 | DMA1_FLAG_HT1 | DMA1_FLAG_TE1);

    dma.DMA_PeripheralBaseAddr = (uint32_t)&SDIO->FIFO;
    dma.DMA_Memory0BaseAddr    = (uint32_t)mem;
    dma.DMA_DIR                = to_card ? DMA_DIR_PeripheralDST : DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize         = bytes / 4u;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_VeryHigh;
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &dma);
    DMA_MuxChannelConfig(DMA_MuxChannel1, 111);   /* SDIO request */
    DMA_Cmd(DMA1_Channel1, ENABLE);
}

static void sd_dma_stop(void)
{
    DMA_Cmd(DMA1_Channel1, DISABLE);
    DMA_ClearFlag(DMA1, DMA1_FLAG_GL1 | DMA1_FLAG_TC1 | DMA1_FLAG_HT1 | DMA1_FLAG_TE1);
}

/* Wait for the data path to finish; returns a data error if any. */
static sd_err_t sd_wait_data_end(bool need_dma_tc)
{
    uint32_t sta, t0 = systime_us();
    do
    {
        sta = SDIO->STA;
        if (systime_elapsed_us(t0) > DATA_WAIT_US)
        {
            SDIO->ICR = STA_STATIC_MASK;
            return SD_ERR_TIMEOUT;
        }
    } while (!(sta & (STA_DATAEND | STA_DATA_ERR)));

    if (sta & STA_DATA_ERR)
    {
        SDIO->ICR = STA_STATIC_MASK;
        if (sta & STA_DTIMEOUT)
            return SD_ERR_TIMEOUT;
        if (sta & STA_DCRCFAIL)
            return SD_ERR_CRC;
        return SD_ERR_DATA;
    }

    if (need_dma_tc)
    {
        while (DMA_GetFlagStatus(DMA1, DMA1_FLAG_TC1) == RESET)
        {
            if (systime_elapsed_us(t0) > DATA_WAIT_US)
            {
                SDIO->ICR = STA_STATIC_MASK;
                return SD_ERR_TIMEOUT;
            }
        }
    }
    SDIO->ICR = STA_STATIC_MASK;
    return SD_OK;
}

/* ------------------------------------------------------------------------ */
/* CMD6: switch function group 1 (bus speed mode): 1 = High Speed / SDR25,
 * 2 = SDR50, 3 = SDR104, 4 = DDR50.                                          */
static sd_err_t sd_switch_function(uint8_t fn)
{
    uint32_t status[16];
    sd_err_t e;
    uint32_t sta, n = 0, i = 0;

    SDIO->DCTRL  = 0;
    SDIO->ICR    = STA_STATIC_MASK;
    SDIO->DTIMER = SD_DTIMER;
    SDIO->DLEN   = 64;
    SDIO->DCTRL  = DCTRL_BLK64 | DCTRL_DTDIR | DCTRL_DTEN;

    e = sd_cmd_r1(6, 0x80FFFFF0u | fn, NULL);  /* mode 1 (set), group 1 -> function fn */
    if (e != SD_OK)
    {
        SDIO->DCTRL = 0;
        SDIO->ICR = STA_STATIC_MASK;
        return e;
    }

    /* read 64 bytes of switch status from the FIFO */
    for (;;)
    {
        sta = SDIO->STA;
        if (sta & STA_DATA_ERR)
        {
            SDIO->DCTRL = 0;
            SDIO->ICR = STA_STATIC_MASK;
            return SD_ERR_DATA;
        }
        while ((SDIO->STA & STA_RXDAVL) && i < 16)
            status[i++] = SDIO->FIFO;
        if ((sta & STA_DATAEND) && i >= 16)
            break;
        if (++n > DATA_POLL_LIMIT)
        {
            SDIO->DCTRL = 0;
            SDIO->ICR = STA_STATIC_MASK;
            return SD_ERR_TIMEOUT;
        }
    }
    SDIO->DCTRL = 0;
    SDIO->ICR = STA_STATIC_MASK;

    /* Switch status is big-endian: bytes 12..13 = group 1 supported functions,
     * byte 16 low nibble = selected function of group 1.  FIFO word 3 holds
     * bytes 12..15, word 4 holds bytes 16..19 (little-endian words).        */
    {
        uint8_t *b = (uint8_t *)status;
        uint16_t supported = (uint16_t)(b[12] << 8) | b[13];
        uint8_t  selected  = b[16] & 0x0F;
        if (!(supported & (1u << fn)) || selected != fn)
            return SD_ERR_UNSUPPORTED;
    }
    return SD_OK;
}

/* ------------------------------------------------------------------------ */
/* ---- UHS-I: 1.8 V signalling --------------------------------------------- */
/* The SD pins sit in the chip's VIO18 I/O domain, whose LDO level is software
 * selectable, so the host side of the UHS-I voltage switch costs nothing.
 * (On the nanoCH32H417 the slot's 47 k pull-ups go to 3.3 V, so an idle 1.8 V
 * bus is clamped at ~2.4 V by the protection diodes -- out of spec, ~20 uA a
 * line; move R25-R29 to VIO18 for a clean setup.)                            */
static void sd_set_vio18(bool v1v8)
{
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR, ENABLE);
    if (bus_1v8 != v1v8)
        sd_io_voltage_changing(v1v8);
    PWR_VIO18ModeCfg(PWR_VIO18CFGMODE_SW);
    PWR_VIO18LevelCfg(v1v8 ? PWR_VIO18Level_MODE1 : PWR_VIO18Level_MODE3);
    bus_1v8 = v1v8;
    Delay_Ms(6);
    sd_io_voltage_changed(v1v8);
}

static inline bool sd_dat0_high(void)
{
    return GPIO_ReadInputDataBit(GPIOE, GPIO_Pin_8) != 0;      /* SD_D0 = PE8, readable in AF mode */
}

/* CMD11 VOLTAGE_SWITCH sequence (SD Physical Layer 4.2.4.2): after the R1
 * response the card drives DAT[3:0] low; the host stops the clock, switches
 * its I/O to 1.8 V, waits, restarts the clock; the card raises DAT within 1 ms. */
static sd_err_t sd_voltage_switch(void)
{
    sd_err_t e;
    uint32_t t0;

    e = sd_cmd_r1(11, 0, NULL);
    if (e != SD_OK)
        return e;                                   /* card declined: still at 3.3 V */
    Delay_Us(30);                                   /* a few clocks: card pulls DAT low */
    SDIO->CLKCR &= ~CLKCR_CLKEN;                    /* clock stopped (low) */
    Delay_Us(100);
    if (sd_dat0_high())
    {
        SDIO->CLKCR |= CLKCR_CLKEN;                 /* card did not start the switch */
        return SD_ERR_UNSUPPORTED;
    }
    sd_set_vio18(true);                             /* host I/O -> 1.8 V (>= 5 ms) */
    Delay_Ms(5);
    SDIO->CLKCR |= CLKCR_CLKEN;
    t0 = systime_us();
    while (!sd_dat0_high())
        if (systime_elapsed_us(t0) > 3000u)
            return SD_ERR_NEED_POWER_CYCLE;         /* card did not come back at 1.8 V */
    Delay_Us(200);
    return SD_OK;
}

/* Power-on the SDIO block at the identification clock and run CMD0 / CMD8 /
 * ACMD41.  request_1v8: set S18R in ACMD41 so a UHS card offers the switch. */
static sd_err_t sd_ident(bool request_1v8, uint32_t *ocr_out)
{
    sd_err_t e;
    uint32_t r;
    sd_card.spec_v2 = false;
    SDIO->POWER = 0;
    SDIO->CLKCR = 0;
    Delay_Ms(2);
    SDIO->CLKCR = 0xFFu;
    SDIO->POWER = 3;
    Delay_Ms(2);
    sd_set_clock(0xFFu, false);
    /* The SD spec wants the supply ramped and at least 74 clock cycles on CLK
     * before the first command; cheap cards really do need it (a fast card
     * tolerates far less).  The clock is running now, so just wait. */
    Delay_Ms(SD_POWERUP_MS);

    STAGE("cmd0");
    for (int t = 0; ; t++)                          /* CMD0 GO_IDLE_STATE */
    {
        e = sd_cmd(0, 0, 0, false, NULL);
        if (e == SD_OK)
            break;
        if (t >= SD_CMD0_TRIES - 1)
        {
            SNAP();
            return SD_ERR_NOCARD;
        }
        Delay_Ms(2);
    }
    Delay_Ms(2);
    STAGE("cmd8");
    for (int t = 0; ; t++)                          /* CMD8 SEND_IF_COND */
    {
        e = sd_cmd(8, 0x1AA, 1, false, &r);
        if (e == SD_OK)
        {
            if ((r & 0xFFFu) != 0x1AAu)
            {
                SNAP();
                return SD_ERR_UNSUPPORTED;
            }
            sd_card.spec_v2 = true;
            break;
        }
        if (e == SD_ERR_TIMEOUT)
            break;                                  /* no CMD8 = a v1 card, legal */
        if (t >= SD_CMD8_TRIES - 1)                 /* CRC/garbage: a marginal bus, not an absent card */
        {
            SNAP();
            return SD_ERR_NOCARD;
        }
        Delay_Ms(2);
    }
    {
        uint32_t tries = 0, ocr = 0;
        uint32_t arg = OCR_VOLTAGE | (sd_card.spec_v2 ? OCR_HCS : 0u) |
                       ((request_1v8 && sd_card.spec_v2) ? OCR_S18R : 0u);
        STAGE("acmd41");
        do
        {
            e = sd_cmd(55, 0, 1, false, &r);
            if (e != SD_OK)
            {
                if (tries < SD_ACMD41_SOFT_TRIES) { Delay_Ms(2); continue; }   /* a slow card can NAK early */
                STAGE("cmd55");
                SNAP();
                return SD_ERR_NOCARD;
            }
            e = sd_cmd(41, arg, 1, true, &ocr);
            if (e != SD_OK)
            {
                if (tries < SD_ACMD41_SOFT_TRIES) { Delay_Ms(2); continue; }
                SNAP();
                return SD_ERR_NOCARD;
            }
            if (ocr & OCR_BUSY)
                break;
            Delay_Ms(1);
        } while (++tries < ACMD41_TRIES);
        if (!(ocr & OCR_BUSY))
        {
            SNAP();
            return SD_ERR_TIMEOUT;                  /* card never left power-up busy */
        }
        sd_card.high_capacity = (ocr & OCR_HCS) != 0;
        *ocr_out = ocr;
    }
    return SD_OK;
}

static sd_err_t sd_init_once(void)
{
    sd_err_t e;
    uint32_t r, resp[4];
    RCC_ClocksTypeDef clocks;

    memset(&sd_card, 0, sizeof(sd_card));
    RCC_GetClocksFreq(&clocks);
    hclk_hz = clocks.HCLK_Frequency;
    {
        /* default-speed divider for <= 25 MHz at the actual HCLK (unless the console set one) */
        uint32_t ds = (hclk_hz + 25000000u - 1u) / 25000000u;
        ds = ds > 2u ? ds - 2u : 0u;
        if (!sd_cfg_user_div)
            sd_cfg.clkdiv_ds = (uint8_t)ds;
    }

    sd_gpio_init();

    /* reset the controller */
    RCC_HB2PeriphResetCmd(RCC_HB2Periph_SDIO, ENABLE);
    RCC_HB2PeriphResetCmd(RCC_HB2Periph_SDIO, DISABLE);

    uint32_t ocr = 0;
    sd_card.signal_1v8 = false;
    sd_card.uhs_mode = 0;
    if (sd_cfg.try_uhs)
    {
        /* Probe at 1.8 V first: a card that is already in 1.8 V mode (from an
         * earlier switch, no power cycle since) answers; a fresh 3.3 V card
         * cannot hear 1.8 V levels and stays silent -- and is never driven
         * with 3.3 V while it might be in 1.8 V mode. */
        sd_set_vio18(true);
        e = sd_ident(true, &ocr);
        if (e == SD_OK && !(ocr & OCR_S18A))
            sd_card.signal_1v8 = true;              /* already switched: carry on at 1.8 V */
        if (!sd_card.signal_1v8)
        {
            sd_set_vio18(false);
            e = sd_ident(true, &ocr);
            if (e != SD_OK)
                return e;
            if ((ocr & OCR_S18A) && sd_card.spec_v2)
            {
                e = sd_voltage_switch();
                if (e == SD_OK)
                    sd_card.signal_1v8 = true;
                else if (e == SD_ERR_NEED_POWER_CYCLE)
                {
                    /* it may have switched without signalling: re-identify at 1.8 V */
                    e = sd_ident(false, &ocr);
                    if (e != SD_OK)
                        return SD_ERR_NEED_POWER_CYCLE;
                    sd_card.signal_1v8 = true;
                }
                /* any other error: the card declined, we are still at 3.3 V */
            }
        }
    }
    else
    {
        sd_set_vio18(false);
        e = sd_ident(false, &ocr);
        if (e != SD_OK)
            return e;
    }

    /* CMD2 ALL_SEND_CID */
    e = sd_cmd(2, 0, 4, false, resp);
    if (e != SD_OK)
        return e;
    memcpy(sd_card.cid, resp, sizeof(resp));

    /* CMD3 SEND_RELATIVE_ADDR */
    e = sd_cmd(3, 0, 1, false, &r);
    if (e != SD_OK)
        return e;
    sd_card.rca = (uint16_t)(r >> 16);

    /* CMD9 SEND_CSD */
    e = sd_cmd(9, (uint32_t)sd_card.rca << 16, 4, false, resp);
    if (e != SD_OK)
        return e;
    memcpy(sd_card.csd, resp, sizeof(resp));

    /* capacity */
    if (((sd_card.csd[0] >> 30) & 3u) == 1u)
    {
        /* CSD v2: C_SIZE = bits [69:48] */
        uint32_t c_size = ((sd_card.csd[1] & 0x3Fu) << 16) | (sd_card.csd[2] >> 16);
        sd_card.block_count = (c_size + 1u) * 1024u;
    }
    else
    {
        uint32_t read_bl_len = (sd_card.csd[1] >> 16) & 0xFu;
        uint32_t c_size      = ((sd_card.csd[1] & 0x3FFu) << 2) | (sd_card.csd[2] >> 30);
        uint32_t c_size_mult = (sd_card.csd[2] >> 15) & 7u;
        uint64_t bytes = (uint64_t)(c_size + 1u) << (c_size_mult + 2u + read_bl_len);
        sd_card.block_count = (uint32_t)(bytes / 512u);
    }

    /* CID: MID byte 0, PNM bytes 3..7, PSN bytes 9..12 (big endian) */
    {
        uint8_t cid[16];
        for (int i = 0; i < 4; i++)
        {
            cid[i * 4 + 0] = (uint8_t)(sd_card.cid[i] >> 24);
            cid[i * 4 + 1] = (uint8_t)(sd_card.cid[i] >> 16);
            cid[i * 4 + 2] = (uint8_t)(sd_card.cid[i] >> 8);
            cid[i * 4 + 3] = (uint8_t)(sd_card.cid[i]);
        }
        sd_card.mid = cid[0];
        memcpy(sd_card.product, &cid[3], 5);
        sd_card.product[5] = 0;
        sd_card.serial = ((uint32_t)cid[9] << 24) | ((uint32_t)cid[10] << 16) |
                         ((uint32_t)cid[11] << 8) | cid[12];
    }

    /* CMD7 SELECT_CARD */
    e = sd_cmd_r1(7, (uint32_t)sd_card.rca << 16, NULL);
    if (e != SD_OK)
        return e;

    /* ACMD6 SET_BUS_WIDTH = 4 bit */
    e = sd_acmd_r1(6, 2, NULL);
    if (e != SD_OK)
        return e;
    sd_set_clock(sd_cfg.clkdiv_ds, true);

    /* CMD16 SET_BLOCKLEN 512 (ignored by SDHC but harmless) */
    e = sd_cmd_r1(16, 512, NULL);
    if (e != SD_OK)
        return e;

    if (sd_card.signal_1v8)
    {
        /* UHS-I access modes: SDR50 (100 MHz max) else SDR25 (50 MHz) else SDR12 */
        if (sd_cfg.try_high_speed && sd_switch_function(2) == SD_OK)
        {
            Delay_Us(100);
            sd_set_clock(sd_cfg.clkdiv_hs, true);
            sd_card.high_speed = true;
            sd_card.uhs_mode = 50;
        }
        else if (sd_switch_function(1) == SD_OK)
        {
            uint32_t d = (hclk_hz + 50000000u - 1u) / 50000000u;   /* largest clock <= 50 MHz */
            Delay_Us(100);
            sd_set_clock(d > 2u ? d - 2u : 0u, true);
            sd_card.high_speed = true;
            sd_card.uhs_mode = 25;
        }
        else
        {
            sd_set_clock(sd_cfg.clkdiv_ds, true);
            sd_card.uhs_mode = 12;
        }
    }
    else if (sd_cfg.try_high_speed && sd_card.spec_v2 && sd_switch_function(1) == SD_OK)
    {
        /* 3.3 V signalling: its own divider -- the top rate is marginal here */
        Delay_Us(100);
        sd_set_clock(sd_cfg.clkdiv_hs_3v3, true);
        sd_card.high_speed = true;
    }
    else
    {
        sd_set_clock(sd_cfg.clkdiv_ds, true);
    }

    e = sd_wait_transfer_state(BUSY_WAIT_US);
    if (e != SD_OK)
        return e;

    sd_card.present = true;
    return SD_OK;
}

/* Initialise the card; if it fails after the High Speed switch (bus too fast
 * for this card/board), step the High Speed divider down and retry. */
/* One real transfer at the init clock, which the board needs before the clock
 * may be stepped down (an init-time lower divider fails outright).  Its result
 * does not matter and its errors are not the card's fault at this speed. */
static void sd_probe_read(uint8_t *buf)
{
    uint32_t c = sd_stat_crc_errors, r = sd_stat_retries, f = sd_stat_fallbacks;
    sd_err_t e;
    sd_quiet_probe = true;
    e = sd_read_blocks(buf, 0, 1);
    sd_quiet_probe = false;
    sd_stat_crc_errors = c;                         /* internal step: not host traffic */
    sd_stat_retries = r;
    sd_stat_fallbacks = f;
    if (e != SD_OK)
        sd_stat_probe_errors++;
}

sd_err_t sd_init(void)
{
    sd_err_t e = sd_init_once();
    /* A card that did not answer at all may simply have been mid power-up (cheap
     * cards take far longer than the spec's minimum).  Cut the power to the bus,
     * let it settle and try the whole identification again. */
    for (int t = 0; t < SD_NOCARD_RETRIES && e == SD_ERR_NOCARD; t++)
    {
        SDIO->POWER = 0;
        Delay_Ms(SD_NOCARD_SETTLE_MS);
        e = sd_init_once();
        if (e != SD_ERR_NOCARD)
            printf("[SD] card answered on power-up retry %d (%s)\r\n", t + 1, sd_err_str(e));
    }
    for (int step = 0; step < 3 && e != SD_OK && e != SD_ERR_NOCARD && e != SD_ERR_NEED_POWER_CYCLE; step++)
    {
        uint32_t nd = sd_cfg.clkdiv_hs == 0 ? 1u : sd_cfg.clkdiv_hs * 2u;
        if (nd > 8u)
            break;
        printf("[SD] init failed (%s) at div %u: retrying with SDIO_CK %lu.%lu MHz\r\n", sd_err_str(e), sd_cfg.clkdiv_hs,
               (unsigned long)(sd_clkdiv_to_hz(nd) / 1000000u), (unsigned long)((sd_clkdiv_to_hz(nd) / 100000u) % 10u));
        sd_cfg.clkdiv_hs = (uint8_t)nd;
        sd_stat_fallbacks++;
        SDIO->POWER = 0;
        Delay_Ms(20);
        e = sd_init_once();
    }
    if (e == SD_OK && sd_card.serial != sd_learned_serial)
        sd_learned_div = 0;                         /* a different card: start fresh */
#if SD_CLKDIV_3V3_SAFE
    /* Mode 1 (3.3 V, LCD live): the card only initialises correctly at the full
     * High Speed divider on this board, but the LCD-loaded VIO18 bus is not
     * reliable there.  Do one real transfer at the init clock, then step the
     * clock down exactly the way sd_fallback_clock() does -- that path tested
     * clean where every init-time attempt at a lower divider failed. */
    if (e == SD_OK && !sd_card.signal_1v8 && sd_card.high_speed && cur_clkdiv < (uint32_t)SD_CLKDIV_3V3_SAFE)
    {
        static __attribute__((aligned(16))) uint8_t probe[512];
        sd_probe_read(probe);
        sd_set_clock(SD_CLKDIV_3V3_SAFE, true);
        printf("[SD] mode 1 (LCD, 3.3 V): SDIO_CK stepped down to %lu.%lu MHz after init\r\n",
               (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
    }
#endif
    /* this card already proved it needs a slower bus: go straight there */
    if (e == SD_OK && sd_learned_div > cur_clkdiv)
    {
        static __attribute__((aligned(16))) uint8_t probe2[512];
        sd_probe_read(probe2);
        sd_set_clock(sd_learned_div, true);
        printf("[SD] this card needed a slower bus before: SDIO_CK set to %lu.%lu MHz\r\n",
               (unsigned long)(sd_card.clk_hz / 1000000u), (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
    }
    return e;
}

void sd_deinit(void)
{
    sd_dma_stop();
    SDIO->DCTRL = 0;
    SDIO->CLKCR = 0;
    SDIO->POWER = 0;
    sd_card.present = false;
}

sd_err_t sd_reinit(void)
{
    if (!sd_cfg_user_div)
    {
        sd_cfg.clkdiv_hs = SD_CLKDIV_HIGHSPEED;     /* retry full speed: the init ladder may have lowered it */
        sd_cfg.clkdiv_hs_3v3 = SD_CLKDIV_HIGHSPEED_3V3;
    }
    sd_deinit();
    Delay_Ms(20);
    return sd_init();
}

bool sd_is_ready(void)
{
    return sd_card.present;
}

/* ------------------------------------------------------------------------ */
static void sd_fail(void)
{
    /* transport-level failure: clean up, and mark the card absent only if it
     * no longer answers a status command */
    sd_dma_stop();
    SDIO->DCTRL = 0;
    SDIO->ICR = STA_STATIC_MASK;
    if (!sd_still_present())
    {
        printf("[SD] card not responding, marked removed\r\n");
        sd_card.present = false;
    }
}

static sd_err_t sd_read_blocks_once(uint8_t *buf, uint32_t lba, uint32_t count)
{
    sd_err_t e;
    uint32_t addr;

    if (!sd_card.present)
        return SD_ERR_NOCARD;
    if (count == 0 || ((uint32_t)buf & 3u) || count > 4096u || lba + count > sd_card.block_count)
        return SD_ERR_PARAM;

    addr = sd_card.high_capacity ? lba : lba * 512u;

    SDIO->DCTRL = 0;
    SDIO->ICR   = STA_STATIC_MASK;
    sd_dma_setup(buf, count * 512u, false);
    SDIO->DTIMER = SD_DTIMER;
    SDIO->DLEN   = count * 512u;
    SDIO->DCTRL  = DCTRL_BLK512 | DCTRL_DTDIR | DCTRL_DMAEN | DCTRL_DTEN;

    STAGE("cmd17/18");
    e = sd_cmd_r1(count > 1 ? 18 : 17, addr, NULL);
    if (e != SD_OK)
    {
        SNAP();
        sd_fail();
        return e;
    }

    STAGE("rd-data");
    e = sd_wait_data_end(true);
    if (e != SD_OK)
        SNAP();
    if (count > 1)
    {
        sd_err_t e2 = sd_cmd_r1(12, 0, NULL);   /* STOP_TRANSMISSION */
        if (e == SD_OK)
            e = e2;
    }
    sd_dma_stop();
    SDIO->DCTRL = 0;

    if (e != SD_OK)
    {
        if (e == SD_ERR_TIMEOUT)
            sd_fail();
        return e;
    }
    return SD_OK;
}

static sd_err_t sd_write_blocks_once(const uint8_t *buf, uint32_t lba, uint32_t count)
{
    sd_err_t e;
    uint32_t addr;

    if (!sd_card.present)
        return SD_ERR_NOCARD;
    if (count == 0 || ((uint32_t)buf & 3u) || count > 4096u || lba + count > sd_card.block_count)
        return SD_ERR_PARAM;

    addr = sd_card.high_capacity ? lba : lba * 512u;

    STAGE("wait-tran");
    e = sd_wait_transfer_state(BUSY_WAIT_US);    /* previous write finished? */
    if (e != SD_OK)
    {
        SNAP();
        if (e == SD_ERR_TIMEOUT)
            sd_fail();
        return e;
    }

    if (count > 1)
        (void)sd_acmd_r1(23, count, NULL);       /* pre-erase hint, optional */

    SDIO->DCTRL = 0;
    SDIO->ICR   = STA_STATIC_MASK;

    STAGE("cmd24/25");
    e = sd_cmd_r1(count > 1 ? 25 : 24, addr, NULL);
    if (e != SD_OK)
    {
        SNAP();
        sd_fail();
        return e;
    }

    sd_dma_setup((void *)buf, count * 512u, true);
    SDIO->DTIMER = SD_DTIMER;
    SDIO->DLEN   = count * 512u;
    SDIO->DCTRL  = DCTRL_BLK512 | DCTRL_DMAEN | DCTRL_DTEN;

    STAGE("data");
    e = sd_wait_data_end(false);
    if (e != SD_OK)
        SNAP();
    if (count > 1)
    {
        sd_err_t e2;
        uint32_t t1 = systime_us();
        STAGE(e == SD_OK ? "cmd12" : sd_last_stage);
        e2 = sd_cmd_r1(12, 0, NULL);
        if (e == SD_OK && e2 != SD_OK)
        {
            /* Some cards answer CMD12 late while busy programming.  If the card
             * still talks and is programming or already idle, the stop was
             * accepted: carry on and wait for busy below. */
            uint32_t r1;
            Delay_Us(200);
            if (sd_cmd_r1(13, (uint32_t)sd_card.rca << 16, &r1) == SD_OK &&
                (R1_STATE(r1) == 7u /* prg */ || R1_STATE(r1) == R1_STATE_TRAN))
            {
                sd_stat_cmd12_late++;
                sd_last_cmd12_us = systime_us() - t1;
            }
            else
            {
                SNAP();
                e = e2;
            }
        }
    }
    sd_dma_stop();
    SDIO->DCTRL = 0;

    if (e != SD_OK)
    {
        if (e == SD_ERR_TIMEOUT)
            sd_fail();
        return e;
    }

    /* wait for the card to finish programming (D0 busy -> tran state) */
    STAGE("prog-busy");
    e = sd_wait_transfer_state(BUSY_WAIT_US);
    if (e != SD_OK)
        SNAP();
    if (e == SD_ERR_TIMEOUT)
        sd_fail();
    return e;
}

/* ------------------------------------------------------------------------ */
/* retry wrappers: CRC / data errors are retried, and the bus clock is lowered
 * after repeated failures.  Timeouts mean the card is gone (no retry).      */
static bool sd_retryable(sd_err_t e)
{
    return e == SD_ERR_CRC || e == SD_ERR_DATA;
}

sd_err_t sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t count)
{
    sd_err_t e = SD_OK;
    for (uint32_t attempt = 0; ; attempt++)
    {
        e = sd_read_blocks_once(buf, lba, count);
        if (e == SD_OK || !sd_retryable(e) || !sd_card.present)
            return e;
        sd_stat_crc_errors++;
        if (attempt >= sd_cfg.retries)
            return e;
        sd_stat_retries++;
        (void)sd_wait_transfer_state(BUSY_WAIT_US);
        if (attempt >= 1 && !sd_fallback_clock())
            return e;
    }
}

sd_err_t sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t count)
{
    sd_err_t e = SD_OK;
    for (uint32_t attempt = 0; ; attempt++)
    {
        e = sd_write_blocks_once(buf, lba, count);
        if (e == SD_OK || !sd_retryable(e) || !sd_card.present)
            return e;
        sd_stat_crc_errors++;
        if (attempt >= sd_cfg.retries)
            return e;
        sd_stat_retries++;
        (void)sd_wait_transfer_state(BUSY_WAIT_US);
        if (attempt >= 1 && !sd_fallback_clock())
            return e;
    }
}

/* ======================================================================== */
/* streamed transfers: one CMD18/CMD25 per transfer, data moved in chunks    */
/* ======================================================================== */
static sd_err_t sd_err_from_sta(uint32_t sta)
{
    if (sta & (STA_TXUNDERR | STA_RXOVERR))
        sd_stat_fifo_errors++;
    if (sta & STA_DTIMEOUT)
        return SD_ERR_TIMEOUT;
    if (sta & STA_DCRCFAIL)
        return SD_ERR_CRC;
    return SD_ERR_DATA;
}

/* CMD12 after a write, tolerating cards that answer late while programming */
static sd_err_t sd_stop_after_write(void)
{
    uint32_t t1 = systime_us();
    sd_err_t e2;
    STAGE("cmd12");
    e2 = sd_cmd_r1(12, 0, NULL);
    if (e2 != SD_OK)
    {
        uint32_t r1;
        Delay_Us(200);
        if (sd_cmd_r1(13, (uint32_t)sd_card.rca << 16, &r1) == SD_OK &&
            (R1_STATE(r1) == 7u /* prg */ || R1_STATE(r1) == R1_STATE_TRAN))
        {
            sd_stat_cmd12_late++;
            sd_last_cmd12_us = systime_us() - t1;
            return SD_OK;
        }
        SNAP();
        return e2;
    }
    return SD_OK;
}

static sd_err_t sd_stream_run_finish(sd_stream_t *s);
volatile uint32_t sd_stat_settle_n, sd_stat_settle_us, sd_stat_settle_max, sd_stat_dataend_us;
volatile uint32_t sd_stat_ready_n, sd_stat_ready_us, sd_stat_ready_max, sd_stat_ready_polls;

/* (re)issue the block command for the not-yet-verified remainder */
static sd_err_t sd_stream_open(sd_stream_t *s)
{
    sd_err_t e;
    uint32_t sectors = (s->total - s->verified) / 512u;
    uint32_t addr = s->lba + s->verified / 512u;

    s->base    = s->verified;
    s->pushed  = s->verified;
    s->open    = false;
    s->started = false;
    s->multi   = sectors > 1u;
    if (!sd_card.high_capacity)
        addr *= 512u;

    if (s->write)
    {
        STAGE("wait-tran");
        e = sd_wait_transfer_state(BUSY_WAIT_US);    /* previous write finished? */
        if (e != SD_OK)
        {
            SNAP();
            if (e == SD_ERR_TIMEOUT)
                sd_fail();
            return e;
        }
        /* No ACMD23 pre-erase hint here: a stream may be closed early (write-behind),
         * and the SD spec leaves pre-erased-but-unwritten blocks undefined, which
         * could clobber neighbouring data on the card.                            */
        SDIO->DCTRL = 0;
        SDIO->ICR   = STA_STATIC_MASK;
        STAGE("cmd24/25");
        e = sd_cmd_r1(s->multi ? 25 : 24, addr, NULL);
        if (e != SD_OK)
        {
            SNAP();
            sd_fail();
            return e;
        }
        /* the data path starts with the first chunk (DMA must be armed first) */
    }
    else
    {
        SDIO->DCTRL = 0;
        SDIO->ICR   = STA_STATIC_MASK;
        s->pending_addr = addr;                       /* CMD17/18 goes out with the first chunk */
    }
    s->open = true;
    return SD_OK;
}

/* write failure: everything before the block in flight was acknowledged */
static void sd_stream_note_write_failure(sd_stream_t *s)
{
    uint32_t done, ack;
    if (!s->started)
        return;
    done = s->run_base + ((s->run_end - s->run_base) - (SDIO->DCOUNT & 0x1FFFFFFu));
    ack  = done ? ((done - 1u) / 512u) * 512u : 0u;
    if (ack > s->pushed)
        ack = s->pushed;
    if (ack > s->verified)
        s->verified = ack;
}

/* stop the data path / command and leave the card in tran state */
static void sd_stream_abort_hw(sd_stream_t *s, sd_err_t why)
{
    if (!s->open)
        return;
    sd_dma_stop();
    SDIO->DCTRL = 0;
    SDIO->ICR   = STA_STATIC_MASK;
    if (s->started || (s->write && (s->multi || s->pushed == s->base)))
        (void)sd_cmd_r1(12, 0, NULL);                /* STOP_TRANSMISSION (a settled CMD25 is still open) */
    if (s->write)
        (void)sd_wait_transfer_state(BUSY_WAIT_US);
    if (why == SD_ERR_TIMEOUT)
        sd_fail();
    s->open    = false;
    s->started = false;
    s->pushed  = s->verified;
}

void sd_stream_abort(sd_stream_t *s)
{
    sd_stream_abort_hw(s, SD_OK);
}

/* wait until the DMA finished moving one chunk; data errors abort */
static sd_err_t sd_stream_wait_dma(uint32_t t0)
{
    for (;;)
    {
        uint32_t sta = SDIO->STA;
        if (sta & STA_DATA_ERR)
        {
            SNAP();
            return sd_err_from_sta(sta);
        }
        if (DMA_GetFlagStatus(DMA1, DMA1_FLAG_TC1) != RESET)
            return SD_OK;
        if (systime_elapsed_us(t0) > DATA_WAIT_US)
        {
            SNAP();
            return SD_ERR_TIMEOUT;
        }
    }
}

static sd_err_t sd_stream_push_write(sd_stream_t *s, const uint8_t *buf, uint32_t bytes)
{
    sd_err_t e;
    uint32_t t0 = systime_us();

    if (!s->started && s->pushed > s->base)
    {
        /* Continuing an open CMD25 after a settled run: the card must have
         * released busy (READY_FOR_DATA) before the DPSM sends the next block. */
        uint32_t r1;
        STAGE("wb-ready");
        for (;;)
        {
            e = sd_cmd_r1(13, (uint32_t)sd_card.rca << 16, &r1);
            if (e != SD_OK)
            {
                SNAP();
                return e;
            }
            sd_stat_ready_polls++;
            if (r1 & R1_READY_FOR_DATA)
            {
                uint32_t dt = systime_elapsed_us(t0);
                sd_stat_ready_n++; sd_stat_ready_us += dt;
                if (dt > sd_stat_ready_max) sd_stat_ready_max = dt;
                break;
            }
            if (systime_elapsed_us(t0) > BUSY_WAIT_US)
            {
                SNAP();
                return SD_ERR_TIMEOUT;
            }
        }
    }
    STAGE("wr-data");
    sd_dma_setup((void *)buf, bytes, true);
    if (!s->started)
    {
        /* A DPSM run has an exact length: the caller announced with
         * sd_stream_run() how much it pushes before it settles, so DATAEND is
         * the moment the card acknowledged every block of the run.  A run cut
         * short by a retry resumes towards the same end. */
        uint32_t len = s->total - s->pushed;
        if (s->run_end > s->pushed)
            len = s->run_end - s->pushed;
        else if (s->run_hint && s->run_hint < len)
            len = s->run_hint;
        if (len < bytes)
            len = bytes;
        s->run_hint = 0;
        s->run_base = s->pushed;
        s->run_end  = s->pushed + len;
        SDIO->DTIMER = SD_DTIMER;
        SDIO->DLEN   = len;
        SDIO->DCTRL  = DCTRL_BLK512 | DCTRL_DMAEN | DCTRL_DTEN;
        s->started = true;
    }
    s->pushed += bytes;
    /* The FIFO holds 32 words, a chunk is at least 128: when the DMA is done
     * the card has taken the first block of this chunk, hence acknowledged
     * every block before it.  A rejected block stops the DPSM and therefore
     * the DMA, which we see as a data error here. */
    e = sd_stream_wait_dma(t0);
    if (e != SD_OK)
        sd_stream_note_write_failure(s);
    return e;
}

static sd_err_t sd_stream_push_read(sd_stream_t *s, uint8_t *buf, uint32_t bytes)
{
    sd_err_t e;
    uint32_t t0 = systime_us();
    uint32_t end_count = s->total - (s->pushed + bytes);   /* DCOUNT when this chunk is in */

    STAGE("rd-data");
    sd_dma_setup(buf, bytes, false);
    if (!s->started)
    {
        SDIO->DTIMER = SD_DTIMER;
        SDIO->DLEN   = s->total - s->base;
        SDIO->DCTRL  = DCTRL_BLK512 | DCTRL_DTDIR | DCTRL_DMAEN | DCTRL_DTEN;
        s->started = true;
        STAGE("cmd17/18");
        e = sd_cmd_r1(s->multi ? 18 : 17, s->pending_addr, NULL);
        if (e != SD_OK)
        {
            SNAP();
            sd_fail();
            return e;
        }
        STAGE("rd-data");
    }
    s->pushed += bytes;
    e = sd_stream_wait_dma(t0);
    if (e != SD_OK)
        return e;
    /* the chunk is in memory; wait for the verdict on its last block: either
     * the next block starts arriving (DCOUNT drops below the chunk boundary,
     * which the DPSM only does after a good CRC) or the transfer ends */
    for (;;)
    {
        uint32_t sta = SDIO->STA;
        if (sta & STA_DATA_ERR)
        {
            SNAP();
            return sd_err_from_sta(sta);
        }
        if (sta & STA_DATAEND)
        {
            while ((SDIO->STA & STA_RXACT) && systime_elapsed_us(t0) <= DATA_WAIT_US)
                ;
            Delay_Us(5);
            sta = SDIO->STA;
            if (sta & STA_DATA_ERR)
            {
                SNAP();
                return sd_err_from_sta(sta);
            }
            return SD_OK;
        }
        if ((SDIO->DCOUNT & 0x1FFFFFFu) < end_count)
            return SD_OK;
        if (systime_elapsed_us(t0) > DATA_WAIT_US)
        {
            SNAP();
            return SD_ERR_TIMEOUT;
        }
    }
}

/* After a failure: abort, decide whether to retry, reopen at the last verified
 * block and (writes) re-send the unverified tail of the previous chunk.
 * Returns SD_OK when the caller may re-send its own chunk. */
static sd_err_t sd_stream_recover(sd_stream_t *s, sd_err_t e)
{
    sd_stream_abort_hw(s, e);
    if (!sd_retryable(e) || !sd_card.present)
        return e;
    sd_stat_crc_errors++;
    if (s->attempts >= sd_cfg.retries)
        return e;
    s->attempts++;
    sd_stat_retries++;
    (void)sd_wait_transfer_state(BUSY_WAIT_US);
    if (s->attempts >= 2 && !sd_fallback_clock())
        return e;
    e = sd_stream_open(s);
    if (e != SD_OK)
        return e;
    if (s->write && s->prev_buf && s->verified < s->prev_end)
        e = sd_stream_push_write(s, s->prev_buf + (s->verified - s->prev_off), s->prev_end - s->verified);
    return e;
}

sd_err_t sd_stream_begin(sd_stream_t *s, uint32_t lba, uint32_t count, bool write)
{
    memset(s, 0, sizeof(*s));
    if (!sd_card.present)
        return SD_ERR_NOCARD;
    if (count == 0 || count > SD_STREAM_MAX_SECTORS || lba + count > sd_card.block_count || lba + count < lba)
        return SD_ERR_PARAM;
    s->lba   = lba;
    s->total = count * 512u;
    s->write = write;
    return sd_stream_open(s);
}

sd_err_t sd_stream_push(sd_stream_t *s, uint8_t *buf, uint32_t count)
{
    uint32_t bytes = count * 512u;
    uint32_t before = s->pushed;
    sd_err_t e;

    if (!s->open)
        return SD_ERR_DATA;
    if (count == 0 || ((uint32_t)buf & 3u) || before + bytes > s->total)
        return SD_ERR_PARAM;
    if (s->write && s->started && before < s->run_end && before + bytes > s->run_end)
        return SD_ERR_PARAM;                         /* a chunk may not straddle a run boundary */

    for (;;)
    {
        if (s->write)
        {
            if (s->started && s->pushed >= s->run_end)
                e = sd_stream_run_finish(s);         /* previous run complete: collect its verdict */
            else
                e = SD_OK;
            if (e == SD_OK)
            {
                uint32_t skip = s->pushed - before;   /* > 0 after a partial retry */
                e = sd_stream_push_write(s, buf + skip, bytes - skip);
                if (e == SD_OK)
                {
                    if (s->verified < before)
                        s->verified = before;
                    s->prev_buf = buf;
                    s->prev_off = before;
                    s->prev_end = before + bytes;
                    return SD_OK;
                }
            }
        }
        else
        {
            e = sd_stream_push_read(s, buf, bytes);
            if (e == SD_OK)
            {
                s->verified = before + bytes;
                return SD_OK;
            }
        }
        e = sd_stream_recover(s, e);
        if (e != SD_OK)
            return e;
    }
}

/* Wait until the DPSM finished the run in progress: DATAEND, CRC status of
 * the last block collected, card no longer busy.  Every block of the run is
 * then acknowledged by the card.  The CMD25 stays open for the next run. */
static sd_err_t sd_stream_run_finish(sd_stream_t *s)
{
    uint32_t sta, t0 = systime_us();

    STAGE("data-end");
    do
    {
        sta = SDIO->STA;
        if (systime_elapsed_us(t0) > DATA_WAIT_US)
        {
            SNAP();
            sd_stream_note_write_failure(s);
            return SD_ERR_TIMEOUT;
        }
    } while (!(sta & (STA_DATAEND | STA_DATA_ERR)));
    sd_stat_dataend_us += systime_elapsed_us(t0);
    /* let the DPSM collect the last CRC status so the verdict is final */
    while ((SDIO->STA & STA_TXACT) && systime_elapsed_us(t0) <= DATA_WAIT_US)
        ;
    Delay_Us(5);
    sta = SDIO->STA;
    if (sta & STA_DATA_ERR)
    {
        SNAP();
        sd_stream_note_write_failure(s);
        return sd_err_from_sta(sta);
    }
    sd_dma_stop();
    SDIO->DCTRL = 0;
    SDIO->ICR   = STA_STATIC_MASK;
    s->started  = false;
    s->verified = s->pushed;
    {
        uint32_t dt = systime_elapsed_us(t0);
        sd_stat_settle_n++; sd_stat_settle_us += dt;
        if (dt > sd_stat_settle_max) sd_stat_settle_max = dt;
    }
    s->prev_buf = NULL;                              /* nothing left unverified */
    return SD_OK;
}

/* End the write: finish the run in progress, CMD12, wait for programming. */
static sd_err_t sd_stream_finish_write(sd_stream_t *s)
{
    sd_err_t e;

    if (s->started)
    {
        if (s->pushed < s->run_end)
        {
            /* The run was announced longer than what was pushed: DATAEND can
             * never come, and stopping blindly drops the tail of the data
             * (that was the write-behind bug).  A correct caller never gets
             * here -- refuse rather than lose data silently. */
            sd_stream_abort_hw(s, SD_OK);
            return SD_ERR_PARAM;
        }
        e = sd_stream_run_finish(s);
        if (e != SD_OK)
            return e;
    }
    if (s->multi)
    {
        e = sd_stop_after_write();
        if (e != SD_OK)
        {
            s->open = false;
            return e;
        }
    }
    s->verified = s->pushed;
    s->open = false;
    STAGE("prog-busy");
    e = sd_wait_transfer_state(BUSY_WAIT_US);
    if (e != SD_OK)
    {
        SNAP();
        if (e == SD_ERR_TIMEOUT)
            sd_fail();
    }
    return e;
}

/* Announce how many bytes the next DPSM run carries (the bytes the caller
 * pushes before sd_stream_settle / sd_stream_end). */
void sd_stream_run(sd_stream_t *s, uint32_t bytes)
{
    s->run_hint = bytes;
}

/* Wait until the card acknowledged every block pushed so far; the CMD25 stays
 * open so a following sequential write can continue it without CMD12/CMD25
 * and the programming wait in between. */
sd_err_t sd_stream_settle(sd_stream_t *s)
{
    sd_err_t e;
    if (!s->open || !s->write)
        return SD_ERR_DATA;
    for (;;)
    {
        if (!s->started)
            return SD_OK;
        if (s->pushed < s->run_end)
        {
            sd_stream_abort_hw(s, SD_OK);
            return SD_ERR_PARAM;
        }
        e = sd_stream_run_finish(s);
        if (e == SD_OK || !s->open)
            return e;
        e = sd_stream_recover(s, e);
        if (e != SD_OK)
            return e;
    }
}

sd_err_t sd_stream_end(sd_stream_t *s)
{
    sd_err_t e;
    if (!s->open)
        return SD_ERR_DATA;
    if (!s->write)
    {
        if (s->pushed != s->total)
        {
            sd_stream_abort(s);
            return SD_ERR_PARAM;
        }
        /* the last read chunk already waited for DATAEND */
        sd_dma_stop();
        SDIO->DCTRL = 0;
        SDIO->ICR   = STA_STATIC_MASK;
        e = s->multi ? sd_cmd_r1(12, 0, NULL) : SD_OK;
        s->open = false;
        s->started = false;
        if (e != SD_OK)
            SNAP();
        return e;
    }
    /* A write stream may end short of what it announced (write-behind); the
     * run in progress is always exact, so nothing is ever stopped blindly. */
    for (;;)
    {
        e = sd_stream_finish_write(s);
        if (e == SD_OK || !s->open)
            return e;
        e = sd_stream_recover(s, e);
        if (e != SD_OK)
            return e;
    }
}
