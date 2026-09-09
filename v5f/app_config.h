/*
 * Build-time configuration for the USB 3.0 ISO mounter (V5F core).
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- SD card (SDIO controller, 4-bit bus) ------------------------------ */
/* SDIO_CK = HCLK / (div + 2); the dividers are derived from HCLK at run time. */
#define SD_CLKDIV_INIT        0xFEu
#define SD_CLKDIV_DEFAULT     2u
#define SD_CLKDIV_HIGHSPEED   0u      /* HCLK/2 after the CMD6 High Speed switch (UHS-I SDR50 at 1.8 V) */
/* Separate divider for High Speed while the bus signals at 3.3 V.  Measured on
 * this board 2026-09-06 with the Samsung: HCLK/2 (100 MHz) is marginal at 3.3 V
 * -- a steady trickle of CRC errors, recovered by retry, and the host eventually
 * sees I/O errors; the driver's own fallback lands on HCLK/3 (66.6 MHz), which
 * tested completely clean (256 MiB byte-identical, 138/138 stress files, FAT
 * consistent).  So start there instead of erroring down to it.  UHS-I at 1.8 V
 * keeps SD_CLKDIV_HIGHSPEED and runs the full 100 MHz. */
#define SD_CLKDIV_HIGHSPEED_3V3 0u    /* MUST be 0: see below */
#define SD_CLKDIV_3V3_SAFE    2u      /* mode 1: SDIO_CK after init = HCLK/(2+2) = 50 MHz at HCLK 200 -- the SD
                                       * spec's High Speed limit, i.e. no overclock at all; 0 = keep the init clock */
/* Do NOT set this to 1 (66.6 MHz) to avoid the 3.3 V CRC retries -- tried twice
 * on 2026-09-06, both times every data read timed out (STA RXACT|RXFIFOF, DCOUNT
 * unmoved: the FIFO fills and the DMA never drains).  The card only initialises
 * correctly when the CMD6 High Speed switch is followed by divider 0; the very
 * same divider 1 reached LATER through sd_fallback_clock() runs perfectly
 * (verified: 256 MiB byte-identical, 138/138 stress files, FAT consistent).  So
 * the divider is not the problem, the init sequence is.  Leaving this at 0 means
 * the card starts at 100 MHz, logs a few CRC errors at 3.3 V and the driver's own
 * fallback settles it at 66.6 MHz -- noisy but self-correcting and safe.  The
 * clean 100 MHz path is UHS-I at 1.8 V (console 'U'), which pauses the LCD. */
#define SD_TRY_HIGH_SPEED     1
#define SD_NEGEDGE            1       /* SDIO_CK on the falling HCLK edge: needed above 50 MHz */
/* ---- Two operating modes (console 'U' toggles, the LCD shows which) --------
 * Mode 1 "LCD"  : SD_TRY_UHS 0 -> bus at 3.3 V, the panel is live.  The card is
 *                 initialised at the full High Speed clock (it only initialises
 *                 correctly that way on this board) and then deliberately stepped
 *                 down to SD_CLKDIV_3V3_SAFE -- the same path the error fallback
 *                 takes, which tested clean; at the full 3.3 V clock the LCD-loaded
 *                 VIO18 bus throws CRC errors.
 * Mode 2 "UHS-I": SD_TRY_UHS 1 -> bus at 1.8 V SDR50, full speed, clean; the panel
 *                 needs 3.3 V so it freezes with a note until 'U' is pressed again.
 * The boot default is mode 1 (the display is why this build exists). */
/* ---- SD power-up tolerance (cheap/slow cards) --------------------------- */
#define SD_POWERUP_MS         20u     /* clock running before CMD0: the spec wants >=1 ms and 74 clocks; slow cards want more */
#define SD_CMD0_TRIES         5       /* CMD0 attempts, 2 ms apart */
#define SD_CMD8_TRIES         3       /* CMD8 attempts when the response is corrupt (a timeout means "v1 card", not an error) */
#define SD_ACMD41_SOFT_TRIES  20u     /* CMD55/ACMD41 errors tolerated while the card is still waking */
#define SD_NOCARD_RETRIES     2       /* whole identification retries after a silent card */
#define SD_NOCARD_SETTLE_MS   60u     /* bus powered off this long between those retries */

#define SD_TRY_UHS            0
#define SD_NEGEDGE_UHS        0       /* sampling edge at 1.8 V: SDR50 at 100 MHz is clean on the rising edge */

/* 3.3 V High Speed above ~90 MHz is marginal on this board (CRC errors on the
 * Samsung at 100 MHz, all recovered by retry but wasteful); UHS-I at 1.8 V is
 * clean at 100 MHz.  sd_set_clock() raises the divider until SDIO_CK is at or
 * below this while the bus signals at 3.3 V.  0 disables the cap.
 * No-op at CLOCK=350 (87.5 MHz); caps CLOCK=400P's 3.3 V mode to 66.6 MHz. */
/* DISABLED (0).  Measured on this board 2026-09-06: the SDIO data path only
 * works at HCLK 200 MHz with divider 0 (SDIO_CK 100 MHz).  Every attempt to run
 * the card slower -- 87.5 MHz (CLOCK=350, div 0) and 66.6 MHz (CLOCK=400P, div 1)
 * -- made every data read time out with STA RXACT|RXFIFOF and DCOUNT unmoved
 * (FIFO fills, DMA never drains).  So the 100 MHz 3.3 V CRC retries are the
 * lesser evil; UHS-I at 1.8 V (console 'U') is the clean 100 MHz path. */
#define SD_MAX_HZ_3V3         0u

/* ---- Mass storage transfer chunking ------------------------------------ */
#define MSC_CHUNK_SECTORS     32u     /* 16 KiB = one USB 3.0 burst */
#define MSC_CHUNK_BYTES       (MSC_CHUNK_SECTORS * 512u)
#define MSC_NBUF              3u      /* 2 in the USB queue + 1 held for SD verification */

/* ---- Images ------------------------------------------------------------ */
#define IMG_MAX               32u     /* images listed per scan                          */
#define IMG_NAME_MAX          96u     /* file name length kept (bytes, incl. NUL)         */
#define IMG_MAX_EXTENTS       256u    /* contiguous runs a mounted file may consist of    */
#define IMG_CONFIG_FILE       "ISOMOUNT.TXT"   /* in the card root: line 1 = image name, line 2 = "rw" (optional) */
#define IMG_SUBDIR            "imgs"  /* images are looked for in the root and in this folder; the KEY cycles this folder */

/* ---- User key: a push button between PC12 ("C12" on the top-left header, next to GND) and GND ---- */
#define KEY_ENABLE            1
#define KEY_LONG_MS           1000u   /* held this long = long press: toggle mode 1 (LCD) / mode 2 (UHS-I) */
#define KEY_DEBOUNCE_MS       30u
#define KEY_SWAP_GAP_MS       1000u   /* image swap: LUN empty this long before the next image appears (a host that misses the window still gets the medium-changed attention) */
#define IMG_SWAP_ACK_MS       2500u   /* a host that has not looked at a changed image LUN this long (counted from the first ignored change, i.e. the key press) gets a USB re-plug (re-enumerates the card too). Polling hosts (Windows, Linux, macOS for disk images) acknowledge within 1-2 s and never see one; macOS's optical driver never looks, so an ISO swap/removal there gets the re-plug. 0 = off: the old ISO icon then lingers on a Mac until ejected/touched */
#define USB_REPLUG_MS         300u    /* re-plug: how long the USB link stays down for the host to notice (a disconnect registers in ms) */

/* ---- USB link supervisor (usbss_device.c usb_link_poll) ---------------- */
#define USB_HS_FALLBACK_MS    1800u   /* SuperSpeed still has not left RxDetect this long after the attach: a USB 2.0-only host */
#define USB_SS_RETRY_MS       2000u   /* nothing attached: restart the SuperSpeed attempt this often so a plug-in starts from RxDetect */
#define USB_SS_TRAIN_MAX_MS   6000u   /* SuperSpeed trained but never enumerated this long after the attach: fall back to USB 2.0 */
#define USB_SS_REENUM_MS      2000u   /* SuperSpeed link bounced and the host has not re-configured us this long: fresh attempt */
#define USB_SS_LOST_MS        500u    /* enumerated SuperSpeed link dead this long: unplugged (a warm reset is ~100 ms) */
#define USB_HS_DISC_MS        800u    /* USB 2.0 bus idle (suspended) this long: check D+/D- for an unplug */

/* ---- USB identity ------------------------------------------------------ */
#define USB_VID               0x1A86u
#define USB_PID               0x55D1u
#define USB_BCD_DEVICE        0x0100u

/* SCSI INQUIRY strings (exact widths: 8 / 16 / 4) */
#define SCSI_VENDOR_ID        "WCH     "
#define SCSI_PRODUCT_ID_IMG   "CH32H417 ISO DVD"
#define SCSI_PRODUCT_ID_DISK  "CH32H417 IMG Dsk"
#define SCSI_PRODUCT_ID_SD    "CH32H417 SD Card"
#define SCSI_PRODUCT_REV      "1.00"

/* ---- Kit LCD (1.54" 240x240 ST7789 on the FPC-12P connector) ---------- */
#define LCD_ENABLE            1
#define LCD_SPI_MAX_HZ        50000000u  /* SCK = HCLK / 2^n, largest not above this (ST7789: 62.5 MHz max) */
#define LCD_ROTATION          0          /* 0..3, see lcd_init() */
#define LCD_BGR               0          /* 1 if red and blue come out swapped */
#define DISPLAY_PERIOD_MS     300u       /* status page refresh, from the idle loops */

/* ---- Board LEDs (D1 blue PC3 = activity, D2 green PC2 = image mounted) ---- */
#define LED_ACTIVE_LOW        1          /* VDDIO -> 1k -> LED -> pin: the pin sinks */
#define LED_ACT_HOLD_MS       60u        /* activity LED keeps blinking this long after the last command */
#define LED_BLINK_MS          80u        /* half period of the activity blink */
#define LCD_MARGIN_X          6          /* text inset from the panel edges (19 columns of 12 px) */

/* ---- Debug ------------------------------------------------------------- */
#define APP_LOG_SCSI          0       /* 1: print every SCSI opcode (slow)   */

#endif
