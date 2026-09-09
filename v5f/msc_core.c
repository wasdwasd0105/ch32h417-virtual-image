/*
 * USB Mass Storage Bulk-Only Transport + SCSI, three logical units with
 * FIXED device types (hosts read INQUIRY once, at enumeration):
 *   LUN 0 - a CD/DVD drive (SCSI MMC, 2048-byte blocks): holds the mounted
 *           .iso, or has no disc.  Blocks are mapped through the image's extent
 *           table and streamed straight from the card.
 *   LUN 1 - the microSD card itself (512-byte blocks), so new images can be
 *           dropped on the card without switching firmware.
 *   LUN 2 - a removable disk: holds the mounted .img (optionally writable), or
 *           has no medium.
 * Runs in the main loop; the USB transports run from interrupts and report
 * completions through the msc_evt_* callbacks.  Data phases move 16 KiB chunks
 * (one USB 3.0 burst) through 3 rotating buffers.  A chunk may span several
 * SD extents: it is always filled completely before it goes to USB, because a
 * short bulk packet in the middle of a data phase would end the transfer.
 */
#include "msc_core.h"
#include "usb_transport.h"
#include "sd_sdio.h"
#include "image_store.h"
#include "usbfs_host.h"
#include "app_config.h"
#include "debug.h"
#include "console.h"
#include "display.h"
#include "leds.h"
#include "systime.h"
#include <string.h>

/* ---- BOT structures ----------------------------------------------------- */
#define CBW_SIGNATURE 0x43425355u   /* 'USBC' */
#define CSW_SIGNATURE 0x53425355u   /* 'USBS' */
#define CBW_LEN       31u
#define CSW_LEN       13u

typedef struct __attribute__((packed))
{
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} msc_cbw_t;

typedef struct __attribute__((packed))
{
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} msc_csw_t;

#define CSW_PASSED      0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

/* ---- SCSI / MMC opcodes ------------------------------------------------- */
#define SCSI_TEST_UNIT_READY          0x00
#define SCSI_REQUEST_SENSE            0x03
#define SCSI_FORMAT_UNIT              0x04
#define SCSI_INQUIRY                  0x12
#define SCSI_MODE_SELECT_6            0x15
#define SCSI_MODE_SENSE_6             0x1A
#define SCSI_START_STOP_UNIT          0x1B
#define SCSI_PREVENT_ALLOW_REMOVAL    0x1E
#define SCSI_READ_FORMAT_CAPACITIES   0x23
#define SCSI_READ_CAPACITY_10         0x25
#define SCSI_READ_10                  0x28
#define SCSI_WRITE_10                 0x2A
#define SCSI_VERIFY_10                0x2F
#define SCSI_SYNCHRONIZE_CACHE_10     0x35
#define SCSI_READ_TOC                 0x43
#define SCSI_READ_HEADER              0x44
#define SCSI_GET_CONFIGURATION        0x46
#define SCSI_GET_EVENT_STATUS         0x4A
#define SCSI_READ_DISC_INFORMATION    0x51
#define SCSI_READ_TRACK_INFORMATION   0x52
#define SCSI_MODE_SELECT_10           0x55
#define SCSI_MODE_SENSE_10            0x5A
#define SCSI_READ_16                  0x88
#define SCSI_WRITE_16                 0x8A
#define SCSI_SERVICE_ACTION_IN_16     0x9E
#define SCSI_REPORT_LUNS              0xA0
#define SCSI_READ_12                  0xA8
#define SCSI_WRITE_12                 0xAA
#define SCSI_READ_DISC_STRUCTURE      0xAD
#define SCSI_SET_CD_SPEED             0xBB
#define SCSI_MECHANISM_STATUS         0xBD
#define SCSI_READ_CD                  0xBE

/* sense keys */
#define SK_NO_SENSE          0x00
#define SK_NOT_READY         0x02
#define SK_MEDIUM_ERROR      0x03
#define SK_ILLEGAL_REQUEST   0x05
#define SK_UNIT_ATTENTION    0x06
#define SK_DATA_PROTECT      0x07

/* ---- logical units ------------------------------------------------------ */
#define LUN_DVD   0u
#define LUN_SD    1u
#define LUN_DISK  2u
#define NUM_LUNS  3u

typedef struct
{
    uint8_t key, asc, ascq;
    uint8_t media_changed;      /* UNIT ATTENTION pending */
    uint8_t gesn_event;         /* GET EVENT STATUS (DVD LUN): 0 none, 2 new media, 3 media removed */
    uint8_t prevent;            /* PREVENT ALLOW MEDIUM REMOVAL */
    uint8_t was_present;        /* for media change detection */
    uint8_t ejected;            /* card LUN: the host ejected it -- absent until re-inserted */
    uint8_t stale;              /* image LUN: the medium changed and the host has not read the new capacity yet */
    uint32_t stale_us;          /* when that happened */
    uint32_t changed_us;        /* image LUN: last medium change told to the host */
    uint8_t  changed;           /* changed_us is valid */
    uint8_t  acked;             /* the host has looked at the LUN since (poll seen it empty / read the new capacity) */
} lun_state_t;

static lun_state_t luns[NUM_LUNS];
static uint32_t    sd_bounce_us;   /* console L: the card LUN is absent from here for SD_BOUNCE_MS */
#define SD_BOUNCE_MS 2500u

/* ---- write-behind ---------------------------------------------------------
 * A WRITE that ends in the middle of its SD stream leaves the stream (CMD25)
 * open; the next WRITE that continues exactly where it stopped, on the same
 * LUN, pushes straight into it -- no STOP, no wait for the card's programming
 * between the host's pieces.  Anything else flushes it: a non-sequential
 * write, any other command (reads, SYNCHRONIZE CACHE, eject...), a console
 * action, ~20 ms of idle, or the host going away.                            */
typedef struct
{
    bool        open;
    unsigned    lun;
    sd_stream_t st;
    uint32_t    fsec;           /* LUN sector the stream continues at (512-byte units) */
    uint32_t    seg, seg_done;  /* bytes announced / pushed in the open stream */
    uint32_t    last_us;
} wb_t;
static wb_t wb;
volatile uint32_t msc_stat_wb_flushes, msc_stat_wb_continued;
#define WB_IDLE_US 20000u

static sd_err_t wb_flush(void)
{
    sd_err_t e;
    if (!wb.open)
        return SD_OK;
    wb.open = false;
    e = sd_stream_end(&wb.st);                      /* closes the short stream, waits for programming */
    msc_stat_wb_flushes++;
    if (e != SD_OK)
        printf("[SD] write-behind flush on LUN%u failed: %s at %s\r\n", wb.lun, sd_err_str(e), sd_last_stage);
    return e;
}

static uint8_t  ejected_by_host;         /* image unmounted by START STOP UNIT (eject) */
static volatile uint8_t pending_remount; /* USB bus reset after a host eject: bring the image back */

/* ---- buffers (DMA targets: keep 16-byte aligned) ------------------------ */
__attribute__((aligned(16))) static uint8_t xfer_buf[MSC_NBUF][MSC_CHUNK_BYTES];
__attribute__((aligned(16))) static uint8_t cbw_buf[1024];
__attribute__((aligned(16))) static uint8_t resp_buf[256];
__attribute__((aligned(16))) static uint8_t csw_buf[32];

/* ---- state shared with interrupt context -------------------------------- */
typedef struct { uint8_t *buf; uint32_t len; } rx_done_t;
static volatile rx_done_t rx_done[4];
static volatile uint8_t   rx_done_w, rx_done_r;
static volatile uint32_t  tx_outstanding;
static volatile uint32_t  reset_seq;
static volatile uint8_t   in_halted, out_halted;

volatile uint32_t msc_stat_read_sectors;
volatile uint32_t msc_stat_write_sectors;
volatile uint32_t msc_stat_commands;
volatile uint32_t msc_last_cmd_us;

static uint32_t sd_retry_deadline;

/* ======================================================================== */
/* callbacks from the USB transports (interrupt context)                    */
/* ======================================================================== */
void msc_evt_rx_complete(uint8_t *buf, uint32_t len)
{
    uint8_t w = rx_done_w;
    rx_done[w & 3].buf = buf;
    rx_done[w & 3].len = len;
    rx_done_w = w + 1;
}

void msc_evt_tx_complete(void)
{
    if (tx_outstanding)
        tx_outstanding--;
}

static bool lun_present(unsigned l);

/* An image LUN's medium changed and the host has not read the new capacity for
 * a while: it still shows the old volume (macOS never polls an optical LUN that
 * has a disc).  The panel tells the user to eject it on the host. */
int msc_image_unacknowledged(void)
{
    if (IMG_SWAP_ACK_MS != 0)
        return 0;                                   /* the swap watchdog re-plugs instead of asking the user */
    for (unsigned l = 0; l < NUM_LUNS; l++)
        if (l != LUN_SD && luns[l].changed && !luns[l].acked && usb_configured &&
            (int32_t)(systime_us() - luns[l].changed_us) >= 3000000)
            return l == LUN_DVD ? 1 : 2;             /* swapped OR removed: the host still shows the old volume */
    return 0;
}

bool msc_sd_ejected(void)
{
    return sd_card.present && luns[LUN_SD].ejected;
}

/* console L: take the card LUN away for SD_BOUNCE_MS and bring it back.  A host
 * that ejected the card volume (macOS sends no command for that on a device it
 * could not lock) waits for exactly this: medium gone, then a new medium. */
void msc_sd_reinsert(void)
{
    luns[LUN_SD].ejected = 1;
    sd_bounce_us = systime_us() | 1u;
}

void msc_evt_reset(void)
{
    luns[LUN_SD].ejected = 0;                       /* a fresh enumeration sees the card again */
    reset_seq++;
    in_halted = 0;
    out_halted = 0;
    tx_outstanding = 0;
    rx_done_r = rx_done_w;
    if (ejected_by_host)
        pending_remount = 1;
}

void msc_evt_halt_cleared(bool in_endpoint)
{
    if (in_endpoint)
        in_halted = 0;
    else
        out_halted = 0;
}

/* ======================================================================== */
/* helpers                                                                   */
/* ======================================================================== */
static inline bool aborted(uint32_t seq)
{
    return reset_seq != seq || !usb_configured;
}

static void set_sense(unsigned l, uint8_t key, uint8_t asc, uint8_t ascq)
{
    luns[l].key = key;
    luns[l].asc = asc;
    luns[l].ascq = ascq;
}

static bool lun_present(unsigned l)
{
    if (!sd_card.present)
        return false;
    switch (l)
    {
        case LUN_SD:   return !luns[LUN_SD].ejected;
        case LUN_DVD:  return img_cur.mounted && img_cur.kind == IMG_KIND_ISO;
        case LUN_DISK: return img_cur.mounted && img_cur.kind == IMG_KIND_DISK;
        default:       return false;
    }
}

static bool lun_cdrom(unsigned l)
{
    return l == LUN_DVD;
}

static uint32_t lun_bs(unsigned l)
{
    return l == LUN_DVD ? 2048u : 512u;
}

static uint32_t lun_blocks(unsigned l)
{
    if (l == LUN_SD)
        return sd_card.present ? sd_card.block_count : 0u;
    return lun_present(l) ? img_cur.blocks : 0u;
}

static bool lun_writable(unsigned l)
{
    if (l == LUN_SD)
        return true;
    return l == LUN_DISK && lun_present(l) && img_cur.writable;
}

/* contiguous SD run for LUN sectors starting at fsec (512-byte units) */
static uint32_t lun_map(unsigned l, uint32_t fsec, uint32_t count, uint32_t *sd_sector)
{
    if (l == LUN_SD)
    {
        if (fsec >= sd_card.block_count)
            return 0;
        if (fsec + count > sd_card.block_count)
            count = sd_card.block_count - fsec;
        *sd_sector = fsec;
        return count;
    }
    return lun_present(l) ? img_map(fsec, count, sd_sector) : 0u;
}

static uint8_t replug_wanted;

/* An image LUN's medium changed.  The acknowledgement clock starts at the first
 * change the host has not looked at and is NOT restarted by a follow-up change
 * (the mount that ends a swap gap): a host that ignored the empty LUN will
 * ignore the new medium too, so the re-plug comes IMG_SWAP_ACK_MS after the
 * key press, not after the mount. */
static void lun_changed(unsigned l)
{
    if (!(luns[l].changed && !luns[l].acked))
        luns[l].changed_us = systime_us();
    luns[l].changed = 1;
    luns[l].acked = 0;
}

/* The host was told about a medium change on an image LUN (LUN empty, or a new
 * medium with UNIT ATTENTION and a media event).  Windows and Linux poll
 * removable and optical LUNs every 1-2 s and act on it; macOS does on a disk
 * LUN but never polls an optical LUN that has a disc.  A host that has not
 * looked at the LUN IMG_SWAP_ACK_MS after the change gets the one signal every
 * OS honours: a USB re-plug (everything re-enumerates, card volume included).
 * Cooperative hosts never see one. */
static void swap_watchdog(void)
{
    if (IMG_SWAP_ACK_MS == 0 || !usb_configured || replug_wanted)
        return;
    for (unsigned l = 0; l < NUM_LUNS; l++)
    {
        if (l == LUN_SD || luns[l].acked || !luns[l].changed)
            continue;
        /* signed: a check in the same microsecond as the change must read as 0, not as a wrap */
        if ((int32_t)(systime_us() - luns[l].changed_us) < (int32_t)((uint32_t)IMG_SWAP_ACK_MS * 1000u))
            continue;
        printf("[img] the host has not looked at LUN %u %u ms after its medium changed: USB re-plug\r\n",
               l, (unsigned)IMG_SWAP_ACK_MS);
        for (unsigned k = 0; k < NUM_LUNS; k++)
            luns[k].acked = 1;
        replug_wanted = 1;
        return;
    }
}

/* Turn image mount/unmount and card insertion/removal into UNIT ATTENTION and
 * media events.  Called between commands. */
static void media_poll(void)
{
    if (!sd_card.present)
    {
        wb.open = false;                            /* nothing left to flush to */
        luns[LUN_SD].ejected = 0;                   /* a card that comes back is a new medium */
    }
    if (luns[LUN_SD].ejected && sd_bounce_us && systime_elapsed_us(sd_bounce_us) >= SD_BOUNCE_MS * 1000u)
    {
        sd_bounce_us = 0;
        luns[LUN_SD].ejected = 0;                   /* end of the console-L bounce */
    }
    if (!sd_card.present && img_cur.mounted)
        img_unmount();                              /* card gone: image gone */
    /* an image swapped for another within one idle pass keeps the LUN "present":
     * the mount generation still changed, and the host must be told */
    {
        static uint32_t last_gen;
        if (img_cur.generation != last_gen)
        {
            last_gen = img_cur.generation;
            for (unsigned l = 0; l < NUM_LUNS; l++)
                if (l != LUN_SD && lun_present(l) && luns[l].was_present)
                {
                    luns[l].media_changed = 1;
                    luns[l].stale = 1; luns[l].stale_us = systime_us(); lun_changed(l);
                    if (l == LUN_DVD)
                        luns[l].gesn_event = 2u;
                }
        }
    }
    for (unsigned l = 0; l < NUM_LUNS; l++)
    {
        uint8_t now = lun_present(l);
        if (now == luns[l].was_present)
            continue;
        luns[l].was_present = now;
        luns[l].media_changed = 1;
        if (l != LUN_SD)
            lun_changed(l);
        if (l == LUN_DVD)
            luns[l].gesn_event = now ? 2u : 3u;
        if (now && l != LUN_SD)
        {
            ejected_by_host = 0;
            luns[l].stale = 1; luns[l].stale_us = systime_us(); lun_changed(l);
        }
    }
}

/* (Re)initialise the card when it is not present, then pick an image. */
static void sd_try_init(void)
{
    sd_err_t e = sd_init();
    if (e == SD_OK)
    {
        printf("[SD] card ready: %s S/N %08lX, %lu MiB, %lu.%lu MHz\r\n",
               sd_card.product, (unsigned long)sd_card.serial,
               (unsigned long)(sd_card.block_count / 2048u),
               (unsigned long)(sd_card.clk_hz / 1000000u),
               (unsigned long)((sd_card.clk_hz / 100000u) % 10u));
        img_auto_mount();
    }
}

static void sd_housekeeping(void)
{
    static uint32_t tick;
    tick++;
    if (!sd_card.present && tick >= sd_retry_deadline)
    {
        sd_retry_deadline = tick + 200000u;
        sd_try_init();
    }
    if (pending_remount)
    {
        pending_remount = 0;
        if (img_remount_last())
            ejected_by_host = 0;
    }
    media_poll();
    swap_watchdog();
    if (replug_wanted)
    {
        replug_wanted = 0;
        if (wb.open)
            wb_flush();                             /* nothing of the host's may be left in flight */
        usb_device_replug();
    }
}

/* Wait for one RX completion.  Returns false when the USB link was reset. */
static bool wait_rx(uint32_t seq, uint8_t **buf, uint32_t *len)
{
    while (rx_done_r == rx_done_w)
    {
        console_poll();
        if (aborted(seq))
            return false;
    }
    {
        uint8_t r = rx_done_r;
        *buf = rx_done[r & 3].buf;
        *len = rx_done[r & 3].len;
        rx_done_r = r + 1;
    }
    return true;
}

/* Same, while idle between commands: console actions that use the card and
 * card/image housekeeping may run here. */
static bool wait_rx_idle(uint32_t seq, uint8_t **buf, uint32_t *len)
{
    while (rx_done_r == rx_done_w)
    {
        usb_link_poll();
        console_poll();
#if LCD_ENABLE
        display_poll();
#endif
        if (wb.open && (console_has_pending() || usbfs_host_pending() ||
                        systime_elapsed_us(wb.last_us) > WB_IDLE_US))
            wb_flush();                             /* an attach takes a long slot */
        console_run_pending();
        usbfs_host_poll();
        sd_housekeeping();
        if (aborted(seq))
            return false;
    }
    {
        uint8_t r = rx_done_r;
        *buf = rx_done[r & 3].buf;
        *len = rx_done[r & 3].len;
        rx_done_r = r + 1;
    }
    return true;
}

static bool wait_tx_outstanding_le(uint32_t seq, uint32_t n)
{
    while (tx_outstanding > n)
    {
        console_poll();
        if (aborted(seq))
            return false;
    }
    return true;
}

static void queue_tx(const uint8_t *buf, uint32_t len)
{
    tx_outstanding++;
    usb_xport_tx(buf, len);
}

/* A data-IN phase the device cannot (fully) serve: BOT allows sending fewer
 * bytes than requested, so end it with a zero-length packet and let the CSW
 * carry the failure and residue.  STALL + CLEAR_FEATURE is the textbook way,
 * but a host that misses the STALL sits in a 30 s timeout on every rejected
 * command (seen with macOS on USB 3 for MODE SENSE page 31h / READ TRACK
 * INFORMATION during enumeration). */
static bool end_in_phase_short(uint32_t seq)
{
    queue_tx(resp_buf, 0);
    return wait_tx_outstanding_le(seq, 0);
}

static bool stall_in_and_wait(uint32_t seq)
{
    in_halted = 1;
    tx_outstanding = 0;
    usb_xport_stall_in();
    while (in_halted)
    {
        console_poll();
        if (aborted(seq))
            return false;
    }
    return true;
}

static bool stall_out_and_wait(uint32_t seq)
{
    out_halted = 1;
    rx_done_r = rx_done_w;
    usb_xport_stall_out();
    while (out_halted)
    {
        console_poll();
        if (aborted(seq))
            return false;
    }
    return true;
}

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ======================================================================== */
/* SCSI commands                                                             */
/* ======================================================================== */
typedef struct
{
    uint8_t  status;
    uint32_t transferred;
    bool     stall_in;
    bool     stall_out;
} cmd_result_t;

static bool medium_ready(unsigned l, cmd_result_t *res)
{
    if (!lun_present(l))
    {
        set_sense(l, SK_NOT_READY, 0x3A, 0x00);        /* medium not present */
        res->status = CSW_FAILED;
        return false;
    }
    if (luns[l].media_changed)
    {
        luns[l].media_changed = 0;
        set_sense(l, SK_UNIT_ATTENTION, 0x28, 0x00);   /* not ready to ready change */
        res->status = CSW_FAILED;
        return false;
    }
    return true;
}

static void fail_illegal(unsigned l, uint8_t asc, const msc_cbw_t *cbw, cmd_result_t *res)
{
    bool data_in = (cbw->bmCBWFlags & 0x80) != 0;
    set_sense(l, SK_ILLEGAL_REQUEST, asc, 0x00);
    res->status = CSW_FAILED;
    res->stall_in = data_in && cbw->dCBWDataTransferLength != 0;
    res->stall_out = !data_in && cbw->dCBWDataTransferLength != 0;
}

static void send_response(uint32_t seq, const msc_cbw_t *cbw, const uint8_t *data, uint32_t len, cmd_result_t *res)
{
    uint32_t want = cbw->dCBWDataTransferLength;
    if (len > want)
        len = want;
    if (len)
    {
        memcpy(resp_buf, data, len);
        queue_tx(resp_buf, len);
        if (!wait_tx_outstanding_le(seq, 0))
            return;
    }
    res->transferred = len;
    if (len < want && (len % usb_xport_max_packet()) == 0)
        res->stall_in = true;
}

static void cmd_inquiry(uint32_t seq, unsigned l, bool valid, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t inq[36];
    if (cbw->CBWCB[1] & 0x01)                       /* EVPD */
    {
        if (cbw->CBWCB[2] == 0x00)
        {
            uint8_t vpd[6] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x80};
            vpd[0] = lun_cdrom(l) ? 0x05 : 0x00;
            send_response(seq, cbw, vpd, sizeof(vpd), res);
            return;
        }
        if (cbw->CBWCB[2] == 0x80)                  /* unit serial number */
        {
            uint8_t sn[4 + 8];
            sn[0] = lun_cdrom(l) ? 0x05 : 0x00; sn[1] = 0x80; sn[2] = 0x00; sn[3] = 8;
            for (int i = 0; i < 8; i++)
                sn[4 + i] = "0123456789ABCDEF"[((sd_card.serial ^ (0x494D47A5u * (uint32_t)l)) >> (28 - 4 * i)) & 0xF];
            send_response(seq, cbw, sn, sizeof(sn), res);
            return;
        }
        fail_illegal(valid ? l : 0, 0x24, cbw, res);
        return;
    }
    memset(inq, 0, sizeof(inq));
    if (!valid)
        inq[0] = 0x7F;                              /* no device at this LUN */
    else
        inq[0] = lun_cdrom(l) ? 0x05 : 0x00;
    inq[1] = 0x80;                                  /* removable */
    inq[2] = lun_cdrom(l) ? 0x02 : 0x06;
    inq[3] = 0x02;
    inq[4] = 36 - 5;
    memcpy(&inq[8], SCSI_VENDOR_ID, 8);
    memcpy(&inq[16], l == LUN_SD ? SCSI_PRODUCT_ID_SD : lun_cdrom(l) ? SCSI_PRODUCT_ID_IMG : SCSI_PRODUCT_ID_DISK, 16);
    memcpy(&inq[32], SCSI_PRODUCT_REV, 4);
    send_response(seq, cbw, inq, sizeof(inq), res);
}

static void cmd_request_sense(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t s[18];
    memset(s, 0, sizeof(s));
    s[0]  = 0x70;
    s[2]  = luns[l].key;
    s[7]  = 10;
    s[12] = luns[l].asc;
    s[13] = luns[l].ascq;
    send_response(seq, cbw, s, sizeof(s), res);
    set_sense(l, SK_NO_SENSE, 0, 0);
}

static void cmd_read_capacity_10(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[8];
    uint32_t blocks;
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    blocks = lun_blocks(l);
    put_be32(&d[0], blocks ? blocks - 1u : 0u);
    put_be32(&d[4], lun_bs(l));
    send_response(seq, cbw, d, sizeof(d), res);
}

static void cmd_read_capacity_16(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[32];
    uint32_t blocks;
    if ((cbw->CBWCB[1] & 0x1F) != 0x10) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    blocks = lun_blocks(l);
    memset(d, 0, sizeof(d));
    put_be32(&d[4], blocks ? blocks - 1u : 0u);
    put_be32(&d[8], lun_bs(l));
    send_response(seq, cbw, d, sizeof(d), res);
}

static void cmd_read_format_capacities(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[12];
    uint32_t bs = lun_bs(l);
    memset(d, 0, sizeof(d));
    d[3] = 8;
    if (lun_present(l))
    {
        put_be32(&d[4], lun_blocks(l));
        d[8] = 0x02;                                /* formatted media */
    }
    else
        d[8] = 0x03;                                /* no media present */
    d[9] = (uint8_t)(bs >> 16); d[10] = (uint8_t)(bs >> 8); d[11] = (uint8_t)bs;
    send_response(seq, cbw, d, sizeof(d), res);
}

/* MODE SENSE(6/10): header (+ CD/DVD capabilities page 2Ah for the CD LUN) */
static void cmd_mode_sense(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res, bool ten)
{
    uint8_t d[8 + 22];
    uint8_t page = cbw->CBWCB[2] & 0x3F;
    uint32_t hdr = ten ? 8u : 4u, n = hdr;
    uint8_t wp = lun_writable(l) ? 0x00 : 0x80;

    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    memset(d, 0, sizeof(d));
    if (lun_cdrom(l) && (page == 0x2A || page == 0x3F))
    {
        uint8_t *p = &d[hdr];
        p[0] = 0x2A; p[1] = 20;
        p[2] = 0x0B;            /* reads CD-R, CD-RW, DVD-ROM */
        p[3] = 0x00;            /* writes nothing */
        p[4] = 0x71;            /* audio play, composite, mode 2 form 1/2, multi-session */
        p[5] = 0x23;
        p[6] = 0x29;            /* lock, eject, tray loading mechanism */
        p[7] = 0x00;
        put_be16(&p[8], 7056);  /* max read speed kB/s */
        put_be16(&p[10], 256);  /* volume levels */
        put_be16(&p[12], 64);   /* buffer size KiB */
        put_be16(&p[14], 7056); /* current read speed */
        n += 22;
    }
    else if (!(page == 0x3F || page == 0x08 || page == 0x1C || page == 0x1A || page == 0x00))
    {
        fail_illegal(l, 0x24, cbw, res);
        return;
    }
    if (ten)
    {
        put_be16(&d[0], (uint16_t)(n - 2));
        d[2] = 0x00;
        d[3] = wp;
    }
    else
    {
        d[0] = (uint8_t)(n - 1);
        d[1] = 0x00;
        d[2] = wp;
    }
    send_response(seq, cbw, d, n, res);
}

/* ---- MMC (CD/DVD) commands -------------------------------------------- */
static void store_cd_address(uint8_t *dst, bool msf, uint32_t lba)
{
    if (msf)
    {
        uint32_t a = lba + 150u;                    /* 2 s lead-in */
        dst[0] = 0;
        dst[1] = (uint8_t)(a / (60u * 75u));
        dst[2] = (uint8_t)((a / 75u) % 60u);
        dst[3] = (uint8_t)(a % 75u);
    }
    else
        put_be32(dst, lba);
}

static void cmd_read_toc(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    const uint8_t *cb = cbw->CBWCB;
    bool msf = (cb[1] & 0x02) != 0;
    uint8_t format = cb[2] & 0x0F;
    uint8_t start_track = cb[6];
    uint8_t d[4 + 3 * 11];
    uint32_t len;

    if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    if (format == 0)
        format = (cb[9] >> 6) & 0x03;               /* old SFF-8020i style (macOS) */
    if ((cb[1] & ~0x02) != 0 || (start_track > 1 && format != 1))
    {
        fail_illegal(l, 0x24, cbw, res);
        return;
    }
    memset(d, 0, sizeof(d));
    switch (format)
    {
        case 0:                                     /* formatted TOC */
        case 1:                                     /* multi-session info */
            len = 4 + 2 * 8;
            d[1] = (uint8_t)(len - 2);
            d[2] = 1; d[3] = 1;                     /* first / last track */
            d[5] = 0x16; d[6] = 0x01;               /* data track, copy permitted, track 1 */
            store_cd_address(&d[8], msf, 0);
            d[13] = 0x16; d[14] = 0xAA;             /* lead-out */
            store_cd_address(&d[16], msf, img_cur.blocks);
            break;
        case 2:                                     /* raw TOC: session 1, points A0 A1 A2 */
        {
            uint8_t *p = &d[4];
            len = 4 + 3 * 11;
            d[1] = (uint8_t)(len - 2);
            d[2] = 1; d[3] = 1;
            for (int i = 0; i < 3; i++, p += 11)
            {
                p[0] = 1; p[1] = 0x16; p[3] = (uint8_t)(0xA0 + i); p[8] = 1;
            }
            store_cd_address(&d[4 + 2 * 11 + 7], msf, img_cur.blocks);   /* A2: lead-out */
            break;
        }
        default:
            fail_illegal(l, 0x24, cbw, res);
            return;
    }
    send_response(seq, cbw, d, len, res);
}

static void cmd_read_header(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[8];
    uint32_t lba = be32(&cbw->CBWCB[2]);
    if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    if (lba >= img_cur.blocks) { fail_illegal(l, 0x21, cbw, res); return; }
    memset(d, 0, sizeof(d));
    d[0] = 0x01;                                    /* mode 1 data */
    store_cd_address(&d[4], (cbw->CBWCB[1] & 0x02) != 0, lba);
    send_response(seq, cbw, d, sizeof(d), res);
}

static uint16_t current_profile(void)
{
    if (!lun_present(LUN_DVD))
        return 0;
    return img_cur.size > (700ull << 20) ? 0x0010 : 0x0008;   /* DVD-ROM above CD size */
}

static void cmd_get_configuration(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    const uint8_t *cb = cbw->CBWCB;
    uint8_t rt = cb[1] & 0x03;
    uint16_t start = be16(&cb[2]);
    uint16_t prof = current_profile();
    uint8_t d[8 + 12 + 12 + 8 + 12 + 8 + 8];
    uint8_t *p = &d[8];
    uint32_t n;

    if (l != LUN_DVD) { fail_illegal(l, 0x20, cbw, res); return; }
    memset(d, 0, sizeof(d));
    put_be16(&d[6], prof);
#define FEATURE(code, ver, len, ...) \
    do { if ((code) >= start && (rt != 2 || (code) == start)) { \
             const uint8_t body_[] = { __VA_ARGS__ }; \
             put_be16(p, (code)); p[2] = (uint8_t)(0x03 | ((ver) << 2)); p[3] = (len); \
             memcpy(&p[4], body_, (len)); p += 4 + (len); } } while (0)
    FEATURE(0x0000, 0, 8, 0x00, 0x10, (uint8_t)(prof == 0x0010), 0x00, 0x00, 0x08, (uint8_t)(prof == 0x0008), 0x00); /* profile list */
    FEATURE(0x0001, 2, 8, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00);   /* core: USB interface, DBE */
    FEATURE(0x0003, 1, 4, 0x28, 0x00, 0x00, 0x00);                           /* removable medium: tray, eject, no lock */
    FEATURE(0x0010, 0, 8, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00);   /* random readable: 2048-byte blocks */
    FEATURE(0x001E, 2, 4, 0x00, 0x00, 0x00, 0x00);                           /* CD read */
    FEATURE(0x001F, 0, 4, 0x00, 0x00, 0x00, 0x00);                           /* DVD read */
#undef FEATURE
    n = (uint32_t)(p - d);
    put_be32(&d[0], n - 4);
    send_response(seq, cbw, d, n, res);
}

static void cmd_get_event_status(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    const uint8_t *cb = cbw->CBWCB;
    uint8_t d[8];
    uint32_t n;

    if (l != LUN_DVD) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!(cb[1] & 0x01)) { fail_illegal(l, 0x24, cbw, res); return; }   /* only polled mode */
    memset(d, 0, sizeof(d));
    if (cb[4] & 0x10)                               /* media class requested */
    {
        d[1] = 6; d[2] = 0x04; d[3] = 0x10;
        d[4] = luns[LUN_DVD].gesn_event;            /* 0 no change, 2 new media, 3 media removed */
        d[5] = lun_present(LUN_DVD) ? 0x02 : 0x00;  /* media present */
        if (!lun_present(LUN_DVD))
            luns[LUN_DVD].acked = 1;                /* the host's event poll sees the drive empty */
        luns[LUN_DVD].gesn_event = 0;
        n = 8;
    }
    else
    {
        d[1] = 2; d[2] = 0x80; d[3] = 0x10;         /* no event available; we support the media class */
        n = 4;
    }
    send_response(seq, cbw, d, n, res);
}

static void cmd_read_disc_information(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[34];
    if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    memset(d, 0, sizeof(d));
    d[1] = 32;
    d[2] = 0x0E;                                    /* finalised disc, last session complete */
    d[3] = 1; d[4] = 1; d[5] = 1; d[6] = 1;
    d[7] = 0x20;                                    /* unrestricted use */
    send_response(seq, cbw, d, sizeof(d), res);
}

static void cmd_read_track_information(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[36];
    if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    memset(d, 0, sizeof(d));
    d[1] = 34;                                      /* data length */
    d[2] = 1; d[3] = 1;                             /* track 1, session 1 */
    d[5] = 0x04;                                    /* data track, uninterrupted */
    d[6] = 0x01;                                    /* mode 1 data */
    put_be32(&d[24], img_cur.blocks);               /* track size */
    put_be32(&d[28], img_cur.blocks ? img_cur.blocks - 1u : 0u);   /* last recorded address */
    send_response(seq, cbw, d, sizeof(d), res);
}

static void cmd_read_disc_structure(uint32_t seq, unsigned l, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[20];
    uint8_t format = cbw->CBWCB[7];
    if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
    if (!medium_ready(l, res)) { res->stall_in = true; return; }
    memset(d, 0, sizeof(d));
    if (format == 0x00)                             /* physical format information */
    {
        d[1] = 18;
        d[4] = 0x01;                                /* DVD-ROM, part version 1 */
        d[5] = 0x0F;                                /* 120 mm, max transfer rate not specified */
        d[6] = 0x01;                                /* one layer, PTP, embossed */
        put_be32(&d[8], 0x00030000u);               /* start PSN of data area */
        put_be32(&d[12], 0x00030000u + img_cur.blocks - 1u);
        send_response(seq, cbw, d, 20, res);
    }
    else if (format == 0x01)                        /* copyright: none */
    {
        d[1] = 6;
        send_response(seq, cbw, d, 8, res);
    }
    else
        fail_illegal(l, 0x24, cbw, res);
}

static void cmd_report_luns(uint32_t seq, const msc_cbw_t *cbw, cmd_result_t *res)
{
    uint8_t d[8 + 8 * NUM_LUNS];
    memset(d, 0, sizeof(d));
    put_be32(&d[0], 8 * NUM_LUNS);
    for (unsigned i = 1; i < NUM_LUNS; i++)
        d[8 + 8 * i + 1] = (uint8_t)i;
    send_response(seq, cbw, d, sizeof(d), res);
}

/* ---- data transfers ----------------------------------------------------- */
static unsigned buf_index(const uint8_t *b)
{
    return (unsigned)((b - xfer_buf[0]) / MSC_CHUNK_BYTES);
}

static void report_sd_error(unsigned l, const char *what, uint32_t fsec, sd_err_t e, bool write, cmd_result_t *res)
{
    printf("[SD] %s LUN%u sector %lu failed: %s at %s (STA %08lX DCOUNT %lu)\r\n", what, l, (unsigned long)fsec,
           sd_err_str(e), sd_last_stage, (unsigned long)sd_last_sta, (unsigned long)sd_last_dcount);
    set_sense(l, sd_card.present ? SK_MEDIUM_ERROR : SK_NOT_READY, sd_card.present ? (write ? 0x0C : 0x11) : 0x3A, 0x00);
    res->status = CSW_FAILED;
}

/* READ: fill each 16 KiB chunk from as many SD extents as it spans, then hand
 * it to USB while the next one is read. */
static void cmd_read(uint32_t seq, unsigned l, const msc_cbw_t *cbw, uint32_t lba, uint32_t nblk, cmd_result_t *res)
{
    uint32_t want = cbw->dCBWDataTransferLength, bs = lun_bs(l);
    uint32_t bytes, sent = 0, fsec, seg = 0, seg_done = 0;
    unsigned idx = 0;
    bool open = false;
    sd_stream_t st;
    sd_err_t e = SD_OK;

    if (!medium_ready(l, res)) { res->stall_in = want != 0; return; }
    if (nblk == 0)
        return;
    if (lba + nblk > lun_blocks(l) || lba + nblk < lba)
    {
        fail_illegal(l, 0x21, cbw, res);            /* LBA out of range */
        return;
    }
    bytes = nblk * bs;
    if (bytes > want)
        bytes = want - (want % bs);
    fsec = lba * (bs / 512u);
    leds_activity();

    while (sent < bytes)
    {
        uint8_t *buf = xfer_buf[idx];
        uint32_t chunk = bytes - sent, filled = 0;
        if (chunk > MSC_CHUNK_BYTES)
            chunk = MSC_CHUNK_BYTES;
        /* this buffer went to USB MSC_NBUF chunks ago: free once at most one transfer is queued */
        if (!wait_tx_outstanding_le(seq, 1)) { if (open) sd_stream_abort(&st); return; }
        while (filled < chunk)
        {
            uint32_t piece;
            if (!open)
            {
                uint32_t sd, run = lun_map(l, fsec, (bytes - sent - filled) / 512u, &sd);
                if (run == 0) { e = SD_ERR_PARAM; goto fail; }
                if (sd == IMG_ZERO_RUN)
                {
                    /* unallocated region of a sparse image: reads as zeros, no card access */
                    uint32_t z = run * 512u;
                    if (z > chunk - filled)
                        z = chunk - filled;
                    memset(buf + filled, 0, z);
                    filled += z; fsec += z / 512u;
                    continue;
                }
                if (run > SD_STREAM_MAX_SECTORS)
                    run = SD_STREAM_MAX_SECTORS;
                e = sd_stream_begin(&st, sd, run, false);
                if (e != SD_OK)
                    goto fail;
                open = true; seg = run * 512u; seg_done = 0;
            }
            piece = chunk - filled;
            if (piece > seg - seg_done)
                piece = seg - seg_done;
            e = sd_stream_push(&st, buf + filled, piece / 512u);
            if (e != SD_OK) { open = false; goto fail; }
            filled += piece; seg_done += piece; fsec += piece / 512u;
            if (seg_done == seg)
            {
                e = sd_stream_end(&st);
                open = false;
                if (e != SD_OK)
                    goto fail;
            }
        }
        if (!wait_tx_outstanding_le(seq, 1)) { if (open) sd_stream_abort(&st); return; }
        queue_tx(buf, filled);
        msc_stat_read_sectors += filled / 512u;
        sent += filled;
        idx = (idx + 1u) % MSC_NBUF;
    }
    if (!wait_tx_outstanding_le(seq, 0))
        return;
    res->transferred = sent;
    if (sent < want && (sent % usb_xport_max_packet()) == 0)
        res->stall_in = true;
    return;

fail:
    if (open)
        sd_stream_abort(&st);
    report_sd_error(l, "read", fsec, e, false, res);
    res->stall_in = true;
    if (!wait_tx_outstanding_le(seq, 0))
        return;
    res->transferred = sent;
}

/* WRITE: two buffers stay armed in the USB RX queue, the third holds the chunk
 * the card is still acknowledging.  A chunk may span extents: its pieces go to
 * consecutive streams.  Sequential writes continue the previous command's
 * stream (write-behind, see wb_t). */
static void cmd_write(uint32_t seq, unsigned l, const msc_cbw_t *cbw, uint32_t lba, uint32_t nblk, cmd_result_t *res)
{
    uint32_t want = cbw->dCBWDataTransferLength, bs = lun_bs(l);
    uint32_t bytes, armed = 0, received = 0, fsec, seg = 0, seg_done = 0, len = 0;
    uint8_t free_mask = (uint8_t)((1u << MSC_NBUF) - 1u), in_flight = 0;
    uint8_t *hold = NULL;
    bool open = false;
    sd_stream_t st;
    sd_err_t e = SD_OK;

    if (!medium_ready(l, res)) { wb_flush(); res->stall_out = want != 0; return; }
    if (!lun_writable(l))
    {
        wb_flush();
        set_sense(l, SK_DATA_PROTECT, 0x27, 0x00);  /* write protected */
        res->status = CSW_FAILED;
        res->stall_out = want != 0;
        return;
    }
    if (nblk == 0)
        return;
    if (lba + nblk > lun_blocks(l) || lba + nblk < lba)
    {
        wb_flush();
        fail_illegal(l, 0x21, cbw, res);
        return;
    }
    bytes = nblk * bs;
    if (bytes > want)
        bytes = want - (want % bs);
    fsec = lba * (bs / 512u);
    leds_activity();

    /* continue the pending stream if this write follows it exactly, else flush it */
    if (wb.open)
    {
        if (wb.lun == l && wb.fsec == fsec && wb.seg_done < wb.seg)
        {
            st = wb.st; open = true; seg = wb.seg; seg_done = wb.seg_done;
            wb.open = false;                        /* owned by this command now */
            msc_stat_wb_continued++;
            sd_stream_run(&st, bytes < seg - seg_done ? bytes : seg - seg_done);
        }
        else
            wb_flush();
    }

#define ARM_MORE()                                                          \
    do {                                                                    \
        while (armed < bytes && in_flight < 2u && free_mask)                \
        {                                                                   \
            unsigned i = (unsigned)__builtin_ctz(free_mask);                \
            uint32_t n = bytes - armed;                                     \
            if (n > MSC_CHUNK_BYTES)                                        \
                n = MSC_CHUNK_BYTES;                                        \
            free_mask &= (uint8_t)~(1u << i);                               \
            usb_xport_rx_arm(xfer_buf[i], n);                               \
            armed += n;                                                     \
            in_flight++;                                                    \
        }                                                                   \
    } while (0)

    ARM_MORE();
    while (received < bytes)
    {
        uint8_t *buf;
        uint32_t off = 0;
        bool buf_free = false;

        if (!wait_rx(seq, &buf, &len)) { if (open) sd_stream_abort(&st); return; }
        in_flight--;
        if (len == 0 || (len & 511u) || received + len > bytes)
        {
            set_sense(l, SK_ILLEGAL_REQUEST, 0x24, 0x00);
            res->status = CSW_PHASE_ERROR;
            res->stall_out = true;
            if (open) sd_stream_abort(&st);
            break;
        }
        while (off < len)
        {
            uint32_t piece;
            if (!open)
            {
                /* announce the whole remaining contiguous run (not just this command's
                 * bytes) so the next sequential write can continue the same stream */
                uint32_t sd, run = lun_map(l, fsec, SD_STREAM_MAX_SECTORS, &sd);
                if (run == 0 || sd == IMG_ZERO_RUN) { e = SD_ERR_PARAM; goto fail; }   /* sparse images are read-only */
                e = sd_stream_begin(&st, sd, run, true);
                if (e != SD_OK)
                    goto fail;
                open = true; seg = run * 512u; seg_done = 0;
                sd_stream_run(&st, bytes - (received + off) < seg ? bytes - (received + off) : seg);
            }
            piece = len - off;
            if (piece > seg - seg_done)
                piece = seg - seg_done;
            e = sd_stream_push(&st, buf + off, piece / 512u);
            if (e != SD_OK) { open = false; goto fail; }
            off += piece; seg_done += piece; fsec += piece / 512u;
            if (seg_done == seg)
            {
                e = sd_stream_end(&st);
                open = false;
                if (e != SD_OK)
                    goto fail;
                if (hold) { free_mask |= (uint8_t)(1u << buf_index(hold)); hold = NULL; }
                if (off == len)
                    buf_free = true;                /* entirely acknowledged */
            }
        }
        msc_stat_write_sectors += len / 512u;
        received += len;
        if (buf_free)
            free_mask |= (uint8_t)(1u << buf_index(buf));
        else
        {
            if (hold)
                free_mask |= (uint8_t)(1u << buf_index(hold));
            hold = buf;
        }
        ARM_MORE();
    }
#undef ARM_MORE
    if (open)
    {
        /* Every block of this command is acknowledged by the card before the
         * CSW says so; only CMD12 and the programming wait are deferred. */
        e = sd_stream_settle(&st);
        if (e != SD_OK)
        {
            sd_stream_abort(&st);
            open = false;
            report_sd_error(l, "write", fsec, e, true, res);
        }
    }
    if (open)
    {
        /* leave the stream open for the next sequential write */
        wb.open = true; wb.lun = l; wb.st = st; wb.fsec = fsec;
        wb.seg = seg; wb.seg_done = seg_done; wb.last_us = systime_us();
    }
    res->transferred = received;
    if (res->status == CSW_PASSED && received < want)
        res->stall_out = true;
    return;

fail:
    if (open)
        sd_stream_abort(&st);
    report_sd_error(l, "write", fsec, e, true, res);
    received += len;
    res->transferred = received;
    res->stall_out = received < want;
}

/* ---- dispatcher --------------------------------------------------------- */
static void execute(uint32_t seq, const msc_cbw_t *cbw, cmd_result_t *res)
{
    const uint8_t *cb = cbw->CBWCB;
    unsigned l = cbw->bCBWLUN & 0x0F;
    bool valid = l < NUM_LUNS;
    uint32_t want = cbw->dCBWDataTransferLength;

    res->status = CSW_PASSED;
    res->transferred = 0;
    res->stall_in = false;
    res->stall_out = false;

#if APP_LOG_SCSI
    printf("[SCSI] lun %u op %02X len %lu cdb", l, cb[0], (unsigned long)want);
    for (int i = 0; i < 12; i++)
        printf(" %02X", cb[i]);
    printf("\r\n");
#endif
    media_poll();
    if (wb.open && cb[0] != SCSI_WRITE_10 && cb[0] != SCSI_WRITE_12 && cb[0] != SCSI_WRITE_16)
        wb_flush();
    if (!valid)
    {
        if (cb[0] == SCSI_INQUIRY) { cmd_inquiry(seq, 0, false, cbw, res); return; }
        if (cb[0] == SCSI_REQUEST_SENSE)
        {
            uint8_t s[18];
            memset(s, 0, sizeof(s));
            s[0] = 0x70; s[2] = SK_ILLEGAL_REQUEST; s[7] = 10; s[12] = 0x25;   /* LUN not supported */
            send_response(seq, cbw, s, sizeof(s), res);
            return;
        }
        set_sense(0, SK_ILLEGAL_REQUEST, 0x25, 0x00);
        res->status = CSW_FAILED;
        res->stall_in = (cbw->bmCBWFlags & 0x80) && want != 0;
        res->stall_out = !(cbw->bmCBWFlags & 0x80) && want != 0;
        return;
    }

    switch (cb[0])
    {
        case SCSI_TEST_UNIT_READY:
            if (!sd_card.present)
            {
                sd_try_init();
                media_poll();
            }
            if (!lun_present(l))
                luns[l].acked = 1;                  /* the host's poll sees the LUN empty */
            if (!medium_ready(l, res))
                return;
            set_sense(l, SK_NO_SENSE, 0, 0);
            return;

        case SCSI_REQUEST_SENSE:          cmd_request_sense(seq, l, cbw, res); return;
        case SCSI_INQUIRY:                cmd_inquiry(seq, l, true, cbw, res); return;
        case SCSI_READ_CAPACITY_10:
            cmd_read_capacity_10(seq, l, cbw, res);
            if (res->status == CSW_PASSED)
                luns[l].stale = 0, luns[l].acked = 1;                  /* the host has taken the new medium on board */
            return;
        case SCSI_SERVICE_ACTION_IN_16:
            cmd_read_capacity_16(seq, l, cbw, res);
            if (res->status == CSW_PASSED)
                luns[l].stale = 0, luns[l].acked = 1;
            return;
        case SCSI_READ_FORMAT_CAPACITIES: cmd_read_format_capacities(seq, l, cbw, res); return;
        case SCSI_MODE_SENSE_6:           cmd_mode_sense(seq, l, cbw, res, false); return;
        case SCSI_MODE_SENSE_10:          cmd_mode_sense(seq, l, cbw, res, true); return;
        case SCSI_REPORT_LUNS:            cmd_report_luns(seq, cbw, res); return;

        case SCSI_READ_10:  cmd_read(seq, l, cbw, be32(&cb[2]), be16(&cb[7]), res); return;
        case SCSI_READ_12:  cmd_read(seq, l, cbw, be32(&cb[2]), be32(&cb[6]), res); return;
        case SCSI_READ_16:
            if (be32(&cb[2]) != 0) { fail_illegal(l, 0x21, cbw, res); return; }
            cmd_read(seq, l, cbw, be32(&cb[6]), be32(&cb[10]), res);
            return;
        case SCSI_READ_CD:                  /* user data of mode-1 sectors only */
            if (!lun_cdrom(l)) { fail_illegal(l, 0x20, cbw, res); return; }
            if ((cb[9] & 0xF8) != 0x10 || (cb[10] & 0x07) != 0) { fail_illegal(l, 0x24, cbw, res); return; }
            cmd_read(seq, l, cbw, be32(&cb[2]), be24(&cb[6]), res);
            return;

        case SCSI_WRITE_10: cmd_write(seq, l, cbw, be32(&cb[2]), be16(&cb[7]), res); return;
        case SCSI_WRITE_12: cmd_write(seq, l, cbw, be32(&cb[2]), be32(&cb[6]), res); return;
        case SCSI_WRITE_16:
            if (be32(&cb[2]) != 0) { fail_illegal(l, 0x21, cbw, res); return; }
            cmd_write(seq, l, cbw, be32(&cb[6]), be32(&cb[10]), res);
            return;

        case SCSI_READ_TOC:               cmd_read_toc(seq, l, cbw, res); return;
        case SCSI_READ_HEADER:            cmd_read_header(seq, l, cbw, res); return;
        case SCSI_GET_CONFIGURATION:      cmd_get_configuration(seq, l, cbw, res); return;
        case SCSI_GET_EVENT_STATUS:       cmd_get_event_status(seq, l, cbw, res); return;
        case SCSI_READ_DISC_INFORMATION:  cmd_read_disc_information(seq, l, cbw, res); return;
        case SCSI_READ_TRACK_INFORMATION: cmd_read_track_information(seq, l, cbw, res); return;
        case SCSI_READ_DISC_STRUCTURE:    cmd_read_disc_structure(seq, l, cbw, res); return;

        case SCSI_MECHANISM_STATUS:
        {
            uint8_t d[8];
            memset(d, 0, sizeof(d));
            d[5] = 1;                               /* one slot */
            send_response(seq, cbw, d, sizeof(d), res);
            return;
        }

        case SCSI_START_STOP_UNIT:
            if (l == LUN_SD && (cb[4] & 0x02))      /* LoEj on the card: behave like a card reader */
            {
                if (!(cb[4] & 0x01))                /* eject: out of the host's view until the card is re-inserted, USB re-plugged or console L */
                {
                    if (!luns[l].ejected)
                        printf("[MSC] host ejected the card volume: it stays out until the card is re-inserted, USB re-plugged or console L\r\n");
                    luns[l].ejected = 1;
                }
                else
                    luns[l].ejected = 0;            /* load */
                media_poll();
                luns[l].media_changed = 0;          /* the host asked for it: no attention */
                set_sense(l, SK_NO_SENSE, 0, 0);
                return;
            }
            if ((l == LUN_DVD || l == LUN_DISK) && (cb[4] & 0x02))   /* LoEj */
            {
                if (!(cb[4] & 0x01))                /* eject */
                {
                    if (luns[l].prevent)
                    {
                        set_sense(l, SK_ILLEGAL_REQUEST, 0x53, 0x02);   /* medium removal prevented */
                        res->status = CSW_FAILED;
                        return;
                    }
                    if (luns[l].stale)
                    {
                        /* The host is ejecting a medium it has not noticed leaving (an
                         * image swapped by the key while its volume was still mounted):
                         * the current image stays; the host's polling finds it. */
                        printf("[img] host ejected the previous medium: the current image stays\r\n");
                        set_sense(l, SK_NO_SENSE, 0, 0);
                        return;
                    }
                    if (lun_present(l))
                    {
                        printf("[img] host ejected the image\r\n");
                        img_unmount();
                        ejected_by_host = 1;
                        media_poll();
                        luns[l].media_changed = 0;  /* the host asked for it: no attention */
                    }
                }
                else                                /* load */
                {
                    if (img_remount_last())
                        ejected_by_host = 0;
                    media_poll();
                }
                set_sense(l, SK_NO_SENSE, 0, 0);
                return;
            }
            /* fall through: plain start/stop is a no-op */
        case SCSI_VERIFY_10:
        case SCSI_SYNCHRONIZE_CACHE_10:
        case SCSI_SET_CD_SPEED:
            if (!medium_ready(l, res))
            {
                res->stall_in = (cbw->bmCBWFlags & 0x80) && want != 0;
                res->stall_out = !(cbw->bmCBWFlags & 0x80) && want != 0;
                return;
            }
            set_sense(l, SK_NO_SENSE, 0, 0);
            return;

        case SCSI_PREVENT_ALLOW_REMOVAL:
            /* Refused on purpose, like a card reader.  A host that has locked
             * a medium stops polling it (macOS then never notices an image
             * swap on that LUN); one it cannot lock is polled once a second
             * (TEST UNIT READY / GET EVENT STATUS), so a swap is seen as
             * medium removed -> new medium on this LUN alone, and the card's
             * volume on the other LUN stays mounted -- no USB re-plug. */
            luns[l].prevent = 0;
            fail_illegal(l, 0x20, cbw, res);
            return;

        case SCSI_MODE_SELECT_6:
        case SCSI_MODE_SELECT_10:
        case SCSI_FORMAT_UNIT:
        default:
            fail_illegal(l, 0x20, cbw, res);        /* invalid command opcode */
            return;
    }
}

bool msc_image_locked(void)
{
    return (lun_present(LUN_DVD) && luns[LUN_DVD].prevent) || (lun_present(LUN_DISK) && luns[LUN_DISK].prevent);
}

/* ======================================================================== */
void msc_init(void)
{
    for (unsigned i = 0; i < NUM_LUNS; i++)
    {
        set_sense(i, SK_NO_SENSE, 0, 0);
        luns[i].media_changed = 0;
        luns[i].gesn_event = 0;
        luns[i].prevent = 0;
        luns[i].ejected = 0;
        luns[i].was_present = lun_present(i);
    }
    sd_retry_deadline = 0;
}

void msc_task(void)
{
    for (;;)
    {
        uint32_t seq;
        uint8_t *buf;
        uint32_t len;
        msc_cbw_t cbw;
        cmd_result_t res;
        msc_csw_t *csw = (msc_csw_t *)csw_buf;

        if (!usb_configured)
        {
            if (wb.open)
                wb_flush();
            usb_link_poll();
            console_poll();
#if LCD_ENABLE
            display_poll();
#endif
            console_run_pending();
            usbfs_host_poll();
            sd_housekeeping();
            continue;
        }
        seq = reset_seq;

        /* ---- command phase: wait for a CBW ------------------------------ */
        usb_xport_rx_arm(cbw_buf, sizeof(cbw_buf));
        if (!wait_rx_idle(seq, &buf, &len))
            continue;

        memcpy(&cbw, buf, CBW_LEN);
        if (len != CBW_LEN || cbw.dCBWSignature != CBW_SIGNATURE)
        {
            printf("[MSC] bad CBW (len %lu)\r\n", (unsigned long)len);
            usb_xport_stall_in();
            usb_xport_stall_out();
            in_halted = out_halted = 1;
            while (!aborted(seq))
                ;
            continue;
        }
        msc_stat_commands++;
        msc_last_cmd_us = systime_us();

        /* ---- data phase ------------------------------------------------- */
        execute(seq, &cbw, &res);
        if (aborted(seq))
            continue;
        if (res.stall_in && !end_in_phase_short(seq))
            continue;
        if (res.stall_out && !stall_out_and_wait(seq))
            continue;

        /* ---- status phase ----------------------------------------------- */
        csw->dCSWSignature   = CSW_SIGNATURE;
        csw->dCSWTag         = cbw.dCBWTag;
        csw->dCSWDataResidue = cbw.dCBWDataTransferLength - res.transferred;
        csw->bCSWStatus      = res.status;
#if APP_LOG_SCSI
        if (res.status != CSW_PASSED)
            printf("[SCSI]   -> status %u, sense %02X/%02X/%02X\r\n", res.status,
                   luns[cbw.bCBWLUN & 0x0F].key, luns[cbw.bCBWLUN & 0x0F].asc, luns[cbw.bCBWLUN & 0x0F].ascq);
#endif
        queue_tx(csw_buf, CSW_LEN);
        if (!wait_tx_outstanding_le(seq, 0))
            continue;
    }
}
