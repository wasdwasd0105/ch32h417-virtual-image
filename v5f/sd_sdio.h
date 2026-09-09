/*
 * microSD card driver on the CH32H417 SDIO host controller (4-bit, DMA).
 *
 * Pins (nanoCH32H417): CMD=PB10, CK=PB11, D0..D3=PE8..PE11, all AF8.
 */
#ifndef SD_SDIO_H
#define SD_SDIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    SD_OK = 0,
    SD_ERR_NOCARD,
    SD_ERR_TIMEOUT,
    SD_ERR_CRC,
    SD_ERR_RESP,
    SD_ERR_PARAM,
    SD_ERR_UNSUPPORTED,
    SD_ERR_DATA,
    SD_ERR_BUSY,
    SD_ERR_NEED_POWER_CYCLE,   /* card stuck mid voltage switch: only a power cycle recovers it */
} sd_err_t;

typedef struct
{
    bool     present;        /* card initialised and believed present */
    bool     high_capacity;  /* SDHC/SDXC (block addressing)          */
    bool     spec_v2;        /* answered CMD8                          */
    bool     high_speed;     /* switched to 50 MHz via CMD6            */
    bool     signal_1v8;     /* bus signalling is 1.8 V (UHS-I)        */
    uint8_t  uhs_mode;       /* 0: none, 12/25/50: SDR12/SDR25/SDR50   */
    uint16_t rca;
    uint32_t block_count;    /* number of 512-byte blocks              */
    uint32_t clk_hz;         /* current SDIO_CK                        */
    uint32_t cid[4];
    uint32_t csd[4];
    char     product[6];     /* product name from CID (5 chars + NUL)  */
    uint32_t serial;         /* product serial number from CID         */
    uint8_t  mid;            /* manufacturer ID                        */
} sd_card_t;

extern sd_card_t sd_card;

/* run-time tunables (defaults from app_config.h) */
typedef struct
{
    uint8_t clkdiv_hs;        /* SDIO_CK divider after a successful High Speed switch (1.8 V UHS-I) */
    uint8_t clkdiv_hs_3v3;    /* same, but while the bus signals at 3.3 V (see app_config.h) */
    uint8_t clkdiv_ds;        /* divider for Default Speed cards                       */
    uint8_t try_high_speed;   /* attempt CMD6 High Speed                               */
    uint8_t negedge;          /* CLKCR NEGEDGE: generate SDIO_CK on the falling HCLK edge */
    uint8_t retries;          /* transfer retries on CRC/data errors                   */
    uint8_t hwfc;             /* CLKCR HWFC_EN: pause SDIO_CK when the FIFO starves    */
    uint8_t try_uhs;          /* attempt the 1.8 V switch + SDR50                      */
    uint8_t negedge_uhs;      /* NEGEDGE used while the bus is at 1.8 V (SDR50 wants 0) */
} sd_config_t;
extern const char *sd_last_stage;   /* phase of the last failed transfer */
extern uint32_t    sd_last_sta;     /* SDIO STA snapshot at that failure  */
extern uint32_t    sd_stat_probe_errors; /* internal post-init probe reads that failed at the init clock */
extern uint32_t    sd_last_dcount;
extern sd_config_t sd_cfg;
extern uint8_t sd_cfg_user_div;

extern uint32_t sd_last_dma_cfgr, sd_last_dma_cntr, sd_last_dma_maddr, sd_last_dma_intfr, sd_last_mux, sd_last_fifocnt, sd_last_dctrl, sd_last_dlen;
extern volatile uint32_t sd_stat_crc_errors;   /* data CRC / data errors seen      */
extern volatile uint32_t sd_stat_retries;      /* transfers retried               */
extern volatile uint32_t sd_stat_fallbacks;    /* clock slow-downs after errors    */
extern volatile uint32_t sd_stat_cmd12_late;   /* CMD12 answered late but accepted */
extern volatile uint32_t sd_last_cmd12_us;

sd_err_t sd_reinit(void);
uint32_t sd_clkdiv_to_hz(uint32_t div);

sd_err_t sd_init(void);
void     sd_deinit(void);
bool     sd_is_ready(void);
sd_err_t sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t count);
sd_err_t sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t count);
const char *sd_err_str(sd_err_t e);

/* ---- streamed multi-block transfers ------------------------------------
 * One CMD18/CMD25 covers a whole transfer (up to SD_STREAM_MAX_SECTORS); the
 * data is handed over chunk by chunk as buffers become available, so the card
 * never sees a STOP between chunks (some SDXC cards pay 10-20 ms per stop).
 * Contract:
 *  - write: sd_stream_push() returns once the chunk is queued to the SDIO FIFO
 *    and every chunk pushed BEFORE it has been accepted by the card.  The
 *    buffer just pushed must stay untouched until the next push / end().
 *  - read : sd_stream_push() returns once the chunk is in memory and its CRCs
 *    have been checked.
 * CRC/underrun errors are retried internally from the last verified block,
 * with the same policy (retries, clock fallback) as sd_read/write_blocks.
 * After an error the stream is closed; call sd_stream_abort() on any early
 * exit (it is harmless on a closed stream).
 */
#define SD_STREAM_MAX_SECTORS 32768u      /* 16 MiB per command (DLEN is 25 bits) */
typedef struct
{
    uint32_t lba;          /* first LBA of the transfer                          */
    uint32_t total;        /* bytes in the transfer                               */
    uint32_t pushed;       /* bytes handed to the DMA                             */
    uint32_t verified;     /* bytes known accepted by (write) / read from the card */
    uint32_t base;         /* byte offset at which the current CMD18/25 started   */
    uint32_t pending_addr; /* read: argument for the CMD17/18 sent with chunk 0   */
    const uint8_t *prev_buf;   /* write: last chunk pushed (may still be unverified) */
    uint32_t prev_off, prev_end;
    uint32_t run_base, run_end; /* write: byte span of the DPSM run in progress (DLEN = run_end - run_base) */
    uint32_t run_hint;          /* write: bytes the caller pushes before it settles (0: up to total) */
    uint8_t  attempts;
    bool     write, open, started, multi;
} sd_stream_t;
sd_err_t sd_stream_begin(sd_stream_t *s, uint32_t lba, uint32_t count, bool write);
sd_err_t sd_stream_push(sd_stream_t *s, uint8_t *buf, uint32_t count);
sd_err_t sd_stream_end(sd_stream_t *s);
void     sd_stream_run(sd_stream_t *s, uint32_t bytes);  /* write: length of the next DPSM run */
sd_err_t sd_stream_settle(sd_stream_t *s);               /* write: every pushed block acknowledged; stream stays open */
void     sd_stream_abort(sd_stream_t *s);
extern volatile uint32_t sd_stat_settle_n, sd_stat_settle_us, sd_stat_settle_max, sd_stat_dataend_us;
extern volatile uint32_t sd_stat_ready_n, sd_stat_ready_us, sd_stat_ready_max, sd_stat_ready_polls;
extern volatile uint32_t sd_stat_fifo_errors;  /* TXUNDERR / RXOVERR seen (flow control gaps) */

#endif
