/*
 * USB 2.0 full-speed host on the "USB-FS" Type-C port.
 *
 * The USBFS controller is the chip's dual-role full-speed block; setting
 * USBFS_UC_HOST_MODE in BASE_CTRL turns it into a host.  Attach is detected
 * from the device's own D+/D- pull-up (MIS_ST), exactly like a classic USB-A
 * host -- the connector's CC pins are wired as a fixed sink on this board and
 * play no part.  VBUS comes from the "VBUS 5V" header next to the connector.
 *
 * What this does: enumerate one device on the root port, print what it is,
 * and for a HID boot keyboard feed the keys into the existing console, so the
 * board can be driven without a serial terminal.  Boot mice are reported too.
 * External hubs are recognised but not enumerated.
 *
 * Timing: poll() returns after a register read when nothing is attached.  A
 * device's interrupt endpoint is polled at most once per bInterval and a NAK
 * (the usual "no new keys" answer) returns immediately.  Only enumeration
 * blocks, for a few tens of ms, once per attach.
 */
#include "usbfs_host.h"
#include "console.h"
#include "systime.h"
#include "debug.h"
#include "ch32h417_usb.h"
#include <string.h>

/* ---- transfer results --------------------------------------------------- */
#define H_OK        0
#define H_NAK       1
#define H_STALL     2
#define H_XFER      3
#define H_TIMEOUT   4
#define H_DISCON    5

#define EP0_GUESS   8u          /* before the device descriptor says otherwise */
#define CFG_BUF_MAX 256u
#define SETUP_TIMEOUT_US 300000u

/* DMA buffers: the controller reads/writes these directly, so word aligned */
__attribute__((aligned(4))) static uint8_t rx_buf[64];
__attribute__((aligned(4))) static uint8_t tx_buf[64];
#define SETUP  ((PUSB_SETUP_REQ)tx_buf)

volatile uint32_t usbfs_host_keys, usbfs_host_attaches, usbfs_host_errors;

/* ---- attached device ---------------------------------------------------- */
enum { DEV_NONE = 0, DEV_ENUMERATING, DEV_KEYBOARD, DEV_MOUSE, DEV_OTHER, DEV_FAILED };

static struct {
    uint8_t  state;
    uint8_t  speed;             /* 0 = low speed, 1 = full speed */
    uint8_t  ep0;               /* endpoint 0 max packet size    */
    uint8_t  addr;
    uint8_t  itf;               /* HID interface number          */
    uint8_t  ep_in;             /* interrupt IN endpoint number  */
    uint8_t  ep_in_size;
    uint8_t  tog;               /* data toggle for that endpoint */
    uint8_t  interval_ms;
    uint8_t  leds;
    uint16_t vid, pid;
    uint8_t  dev_class;
    uint8_t  prev[8];           /* previous keyboard report      */
    uint32_t last_poll_us;
} dev;

static volatile bool attach_event;      /* set by poll(), cleared once serviced */
static bool port_enabled;
static bool had_fail;                   /* last enumeration failed: back off before retrying */
static uint32_t fail_us;
#define RETRY_AFTER_FAIL_US 3000000u

/* ---- low level ---------------------------------------------------------- */
static void set_addr(uint8_t addr)
{
    USBFSH->DEV_ADDR = (USBFSH->DEV_ADDR & USBFS_UDA_GP_BIT) | (addr & USBFS_USB_ADDR_MASK);
}

static void set_speed(uint8_t full_speed)
{
    if (full_speed)
    {
        USBFSH->BASE_CTRL  &= ~USBFS_UC_LOW_SPEED;
        USBFSH->HOST_CTRL  &= ~USBFS_UH_LOW_SPEED;
        USBFSH->HOST_SETUP &= ~USBFS_UH_PRE_PID_EN;
    }
    else
    {
        /* low speed through a full-speed host: every token needs a PRE PID */
        USBFSH->BASE_CTRL  |= USBFS_UC_LOW_SPEED;
        USBFSH->HOST_CTRL  |= USBFS_UH_LOW_SPEED;
        USBFSH->HOST_SETUP |= USBFS_UH_PRE_PID_EN;
    }
}

static bool device_attached(void)
{
    return (USBFSH->MIS_ST & USBFS_UMS_DEV_ATTACH) != 0;
}

/* One USB transaction.  nak_us = how long to keep retrying a NAK; 0 returns at
 * the first NAK, which is what an idle interrupt endpoint answers. */
static uint8_t transact(uint8_t pid_ep, uint8_t tog, uint32_t nak_us)
{
    uint32_t t0 = systime_us();
    uint32_t cap = nak_us > 100000u ? nak_us : 100000u;
    uint8_t  tries = 0;

    USBFSH->HOST_TX_CTRL = USBFSH->HOST_RX_CTRL = tog;
    for (;;)
    {
        uint16_t i;
        uint8_t  r;

        USBFSH->HOST_EP_PID = pid_ep;
        USBFSH->INT_FG = USBFS_UIF_TRANSFER;
        for (i = 3000; i && !(USBFSH->INT_FG & USBFS_UIF_TRANSFER); i--)
            Delay_Us(1);
        USBFSH->HOST_EP_PID = 0;

        if (!(USBFSH->INT_FG & USBFS_UIF_TRANSFER))
            return H_TIMEOUT;
        if (USBFSH->INT_ST & USBFS_UIS_TOG_OK)
            return H_OK;

        r = USBFSH->INT_ST & USBFS_UIS_H_RES_MASK;
        if (r == USB_PID_STALL)
            return H_STALL;
        if (r == USB_PID_NAK)
        {
            if (nak_us == 0 || systime_elapsed_us(t0) > nak_us)
                return H_NAK;
        }
        else switch (pid_ep >> 4)
        {
            case USB_PID_SETUP:
            case USB_PID_OUT:
                if (r)
                    return H_XFER;
                break;                      /* toggle mismatch: retry */
            case USB_PID_IN:
                if (r && r != USB_PID_DATA0 && r != USB_PID_DATA1)
                    return H_XFER;
                break;
            default:
                return H_XFER;
        }

        Delay_Us(15);
        if (USBFSH->INT_FG & USBFS_UIF_DETECT)
        {
            Delay_Us(200);
            if (!(USBFSH->HOST_CTRL & USBFS_UH_PORT_EN))
                return H_DISCON;
            USBFSH->INT_FG = USBFS_UIF_DETECT;
        }
        if (++tries >= 16 || systime_elapsed_us(t0) > cap)
            return H_TIMEOUT;
    }
}

/* Control transfer.  The request is already in tx_buf via SETUP. */
static uint8_t ctrl_transfer(uint8_t *buf, uint16_t *plen)
{
    uint8_t  s;
    uint16_t rem, n, k;
    uint8_t  ep0 = dev.ep0 ? dev.ep0 : EP0_GUESS;

    if (plen)
        *plen = 0;
    USBFSH->HOST_TX_LEN = sizeof(USB_SETUP_REQ);
    s = transact((USB_PID_SETUP << 4) | 0, 0, SETUP_TIMEOUT_US);
    if (s != H_OK)
        return s;

    USBFSH->HOST_TX_CTRL = USBFSH->HOST_RX_CTRL = USBFS_UH_T_TOG | USBFS_UH_R_TOG;
    rem = SETUP->wLength;
    if (rem && buf)
    {
        if (SETUP->bRequestType & USB_REQ_TYP_IN)
        {
            while (rem)
            {
                s = transact((USB_PID_IN << 4) | 0, USBFSH->HOST_RX_CTRL, SETUP_TIMEOUT_US);
                if (s != H_OK)
                    return s;
                USBFSH->HOST_RX_CTRL ^= USBFS_UH_R_TOG;
                n = USBFSH->RX_LEN < rem ? USBFSH->RX_LEN : rem;
                for (k = 0; k < n; k++)
                    buf[k] = rx_buf[k];
                buf += n;
                rem -= n;
                if (plen)
                    *plen += n;
                if (USBFSH->RX_LEN == 0 || (USBFSH->RX_LEN % ep0) != 0)
                    break;                  /* short packet ends the stage */
            }
            USBFSH->HOST_TX_LEN = 0;        /* status stage is OUT */
        }
        else
        {
            while (rem)
            {
                n = rem >= ep0 ? ep0 : rem;
                for (k = 0; k < n; k++)
                    tx_buf[k] = buf[k];
                USBFSH->HOST_TX_LEN = n;
                s = transact((USB_PID_OUT << 4) | 0, USBFSH->HOST_TX_CTRL, SETUP_TIMEOUT_US);
                if (s != H_OK)
                    return s;
                USBFSH->HOST_TX_CTRL ^= USBFS_UH_T_TOG;
                buf += n;
                rem -= n;
                if (plen)
                    *plen += n;
            }
        }
    }
    /* status stage: opposite direction of the data stage */
    return transact(USBFSH->HOST_TX_LEN ? ((USB_PID_IN << 4) | 0) : ((USB_PID_OUT << 4) | 0),
                    USBFS_UH_R_TOG | USBFS_UH_T_TOG, SETUP_TIMEOUT_US);
}

static void make_setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    SETUP->bRequestType = type;
    SETUP->bRequest     = req;
    SETUP->wValue       = value;
    SETUP->wIndex       = index;
    SETUP->wLength      = len;
}

static uint8_t get_descriptor(uint8_t type, uint8_t idx, uint8_t *buf, uint16_t len, uint16_t *got)
{
    make_setup(USB_REQ_TYP_IN, USB_GET_DESCRIPTOR, (uint16_t)(type << 8) | idx, 0, len);
    return ctrl_transfer(buf, got);
}

/* ---- HID keycode mapping ------------------------------------------------ */
/* HID usage IDs 0x04..0x38, unshifted then shifted; 0 = not a character key */
static const char kb_lower[] =
    "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./";
static const char kb_upper[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?";

_Static_assert(sizeof(kb_lower) == 0x38 - 0x04 + 2, "unshifted keymap must cover HID usages 0x04..0x38");
_Static_assert(sizeof(kb_upper) == sizeof(kb_lower), "both keymaps must be the same length");

static char keycode_to_char(uint8_t code, bool shift)
{
    if (code < 0x04 || code > 0x38)
        return 0;
    return shift ? kb_upper[code - 0x04] : kb_lower[code - 0x04];
}

static uint8_t set_leds(void)
{
    uint16_t len = 1;
    uint8_t  report = dev.leds;
    make_setup(USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF, 0x09 /* SET_REPORT */,
               0x0200 /* output report 0 */, dev.itf, 1);
    return ctrl_transfer(&report, &len);
}

static void handle_keyboard_report(const uint8_t *rep, uint16_t len)
{
    bool shift;
    int  i, j;

    if (len < 3)
        return;
    shift = (rep[0] & 0x22) != 0;               /* either shift modifier */

    for (i = 2; i < 8 && i < (int)len; i++)
    {
        uint8_t code = rep[i];
        char    c;
        bool    was_down = false;

        if (code == 0 || code == 1 /* rollover */)
            continue;
        for (j = 2; j < 8; j++)
            if (dev.prev[j] == code)
                was_down = true;
        if (was_down)
            continue;                            /* held, not a new press */

        if (code == 0x39)                        /* caps lock: toggle its LED */
        {
            dev.leds ^= 0x02;
            set_leds();
            continue;
        }
        if (code == 0x53) { dev.leds ^= 0x01; set_leds(); continue; }   /* num lock  */
        if (code == 0x47) { dev.leds ^= 0x04; set_leds(); continue; }   /* scroll    */

        c = keycode_to_char(code, shift);
        if (c)
        {
            usbfs_host_keys++;
            console_feed(c);
        }
    }
    memcpy(dev.prev, rep, len < 8 ? len : 8);
}

/* ---- enumeration -------------------------------------------------------- */
static void parse_config(const uint8_t *cfg, uint16_t len)
{
    uint16_t p = 0;
    uint8_t  cur_itf = 0xFF, cur_class = 0, cur_proto = 0;
    bool     chosen = false;

    dev.ep_in = 0;
    while (p + 2 <= len && cfg[p] >= 2)
    {
        uint8_t dlen = cfg[p], dtype = cfg[p + 1];

        if (dtype == 0x04 && p + 9 <= len)              /* interface */
        {
            cur_itf   = cfg[p + 2];
            cur_class = cfg[p + 5];
            cur_proto = cfg[p + 7];
            printf("[FSH]   interface %u: class %02X subclass %02X protocol %02X\r\n",
                   cur_itf, cur_class, cfg[p + 6], cur_proto);
        }
        else if (dtype == 0x05 && p + 7 <= len)         /* endpoint */
        {
            uint8_t  ea = cfg[p + 2], attr = cfg[p + 3];
            uint16_t mps = (uint16_t)cfg[p + 4] | ((uint16_t)cfg[p + 5] << 8);

            if ((ea & 0x80) && (attr & 0x03) == 0x03 && cur_class == 0x03 && !chosen)
            {
                dev.itf         = cur_itf;
                dev.ep_in       = ea & 0x0F;
                dev.ep_in_size  = mps > sizeof(rx_buf) ? sizeof(rx_buf) : (uint8_t)mps;
                dev.interval_ms = cfg[p + 6] ? cfg[p + 6] : 8;
                dev.state       = (cur_proto == 0x02) ? DEV_MOUSE : DEV_KEYBOARD;
                /* protocol 1 = keyboard, 2 = mouse; anything else: treat as keyboard */
                chosen = (cur_proto == 0x01 || cur_proto == 0x02);
            }
        }
        p += dlen;
    }
}

static bool enumerate(void)
{
    static uint8_t cfg[CFG_BUF_MAX];
    uint8_t  s;
    uint16_t got, total;

    memset(&dev, 0, sizeof(dev));
    dev.ep0 = EP0_GUESS;

    /* bus reset, then the device answers at address 0 */
    set_addr(0);
    set_speed(1);
    USBFSH->HOST_CTRL |= USBFS_UH_BUS_RESET;
    Delay_Ms(15);
    USBFSH->HOST_CTRL &= ~USBFS_UH_BUS_RESET;
    Delay_Ms(2);
    USBFSH->INT_FG = USBFS_UIF_DETECT;
    Delay_Ms(2);

    if (!device_attached())
        return false;

    dev.speed = (USBFSH->MIS_ST & USBFS_UMS_DM_LEVEL) ? 0 : 1;
    set_speed(dev.speed);
    USBFSH->HOST_CTRL  |= USBFS_UH_PORT_EN;
    USBFSH->HOST_SETUP |= USBFS_UH_SOF_EN;
    port_enabled = true;
    Delay_Ms(100);                                   /* let it settle after reset */

    s = get_descriptor(USB_DESCR_TYP_DEVICE, 0, cfg, 18, &got);
    if (s != H_OK || got < 8)
    {
        printf("[FSH] device descriptor failed (%u)\r\n", s);
        return false;
    }
    dev.ep0 = cfg[7] ? cfg[7] : EP0_GUESS;
    if (got >= 12)
    {
        dev.vid = (uint16_t)cfg[8]  | ((uint16_t)cfg[9]  << 8);
        dev.pid = (uint16_t)cfg[10] | ((uint16_t)cfg[11] << 8);
    }
    dev.dev_class = cfg[4];

    /* a second reset is the reliable way to start addressing from a known state */
    USBFSH->HOST_CTRL |= USBFS_UH_BUS_RESET;
    Delay_Ms(15);
    USBFSH->HOST_CTRL &= ~USBFS_UH_BUS_RESET;
    Delay_Ms(2);
    USBFSH->INT_FG = USBFS_UIF_DETECT;
    set_speed(dev.speed);
    USBFSH->HOST_CTRL  |= USBFS_UH_PORT_EN;
    USBFSH->HOST_SETUP |= USBFS_UH_SOF_EN;
    Delay_Ms(10);

    make_setup(0x00, USB_SET_ADDRESS, 2, 0, 0);
    s = ctrl_transfer(NULL, NULL);
    if (s != H_OK)
    {
        printf("[FSH] set address failed (%u)\r\n", s);
        return false;
    }
    Delay_Ms(5);
    dev.addr = 2;
    set_addr(2);

    s = get_descriptor(USB_DESCR_TYP_CONFIG, 0, cfg, 9, &got);
    if (s != H_OK || got < 9)
    {
        printf("[FSH] config descriptor header failed (%u)\r\n", s);
        return false;
    }
    total = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
    if (total > CFG_BUF_MAX)
        total = CFG_BUF_MAX;
    s = get_descriptor(USB_DESCR_TYP_CONFIG, 0, cfg, total, &got);
    if (s != H_OK || got < 9)
    {
        printf("[FSH] config descriptor failed (%u)\r\n", s);
        return false;
    }

    printf("[FSH] device %04X:%04X, %s speed, ep0 %u bytes, class %02X\r\n",
           dev.vid, dev.pid, dev.speed ? "full" : "low", dev.ep0, dev.dev_class);

    dev.state = DEV_OTHER;
    parse_config(cfg, got);

    make_setup(0x00, USB_SET_CONFIGURATION, cfg[5], 0, 0);
    s = ctrl_transfer(NULL, NULL);
    if (s != H_OK)
    {
        printf("[FSH] set configuration failed (%u)\r\n", s);
        return false;
    }

    if (dev.dev_class == 0x09)
    {
        printf("[FSH] this is a hub: hubs are not enumerated, plug the device in directly\r\n");
        dev.state = DEV_OTHER;
        return true;
    }
    if (!dev.ep_in)
    {
        printf("[FSH] no HID interrupt endpoint: device is powered but not driven\r\n");
        dev.state = DEV_OTHER;
        return true;
    }

    /* HID boot protocol: fixed 8-byte keyboard / 3-byte mouse reports */
    make_setup(USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF, 0x0B /* SET_PROTOCOL */, 0, dev.itf, 0);
    if (ctrl_transfer(NULL, NULL) != H_OK)
        printf("[FSH] set boot protocol refused, trying report protocol anyway\r\n");
    make_setup(USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF, 0x0A /* SET_IDLE */, 0, dev.itf, 0);
    (void)ctrl_transfer(NULL, NULL);

    printf("[FSH] %s ready on interface %u, endpoint %u, %u bytes every %u ms\r\n",
           dev.state == DEV_MOUSE ? "mouse" : "keyboard",
           dev.itf, dev.ep_in, dev.ep_in_size, dev.interval_ms);
    if (dev.state == DEV_KEYBOARD)
        printf("[FSH] keys are fed to the console: press h for the command list\r\n");
    return true;
}

static void detach(void)
{
    if (dev.state != DEV_NONE)
        printf("[FSH] device removed\r\n");
    memset(&dev, 0, sizeof(dev));
    had_fail = false;                       /* a real unplug clears the back-off */
    port_enabled = false;
    USBFSH->HOST_CTRL  &= ~USBFS_UH_PORT_EN;
    USBFSH->HOST_SETUP &= ~USBFS_UH_SOF_EN;
    set_addr(0);
    set_speed(1);
}

/* ---- public ------------------------------------------------------------- */
void usbfs_host_init(void)
{
    /* 48 MHz for the USBFS block, tapped off the USBHS PLL (480 MHz / 10).
     * usbhs_msc.c configures that PLL identically, so either order is fine. */
    if ((RCC->PLLCFGR & RCC_SYSPLL_SEL) != RCC_SYSPLL_USBHS)
    {
        RCC_USBHS_PLLCmd(DISABLE);
        RCC_USBHSPLLCLKConfig((RCC->CTLR & RCC_HSERDY) ? RCC_USBHSPLLSource_HSE : RCC_USBHSPLLSource_HSI);
        RCC_USBHSPLLReferConfig(RCC_USBHSPLLRefer_25M);
        RCC_USBHSPLLClockSourceDivConfig(RCC_USBHSPLL_IN_Div1);
        RCC_USBHS_PLLCmd(ENABLE);
        while (!(RCC->CTLR & RCC_USBHS_PLLRDY))
            ;
    }
    RCC_USBFSCLKConfig(RCC_USBFSCLKSource_USBHSPLL);
    RCC_USBFS48ClockSourceDivConfig(RCC_USBFS_Div10);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_OTG_FS, ENABLE);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOA, ENABLE);
    /* PA11/PA12 are switched to the USB transceiver by the controller itself */

    USBFSH->BASE_CTRL = USBFS_UC_HOST_MODE;
    while (!(USBFSH->BASE_CTRL & USBFS_UC_HOST_MODE))
        ;
    USBFSH->HOST_CTRL   = 0;
    USBFSH->DEV_ADDR    = 0;
    USBFSH->HOST_EP_MOD = USBFS_UH_EP_TX_EN | USBFS_UH_EP_RX_EN;
    USBFSH->HOST_RX_DMA = (uint32_t)rx_buf;
    USBFSH->HOST_TX_DMA = (uint32_t)tx_buf;
    USBFSH->HOST_RX_CTRL = 0;
    USBFSH->HOST_TX_CTRL = 0;
    USBFSH->BASE_CTRL   = USBFS_UC_HOST_MODE | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;
    USBFSH->INT_FG      = 0xFF;
    USBFSH->INT_EN      = 0;                 /* polled, not interrupt driven */

    memset(&dev, 0, sizeof(dev));
    printf("[FSH] full-speed host started on the USB-FS port\r\n");
    printf("[FSH] close the \"VBUS 5V\" header next to that connector or devices get no power\r\n");
}

bool usbfs_host_pending(void)
{
    if (attach_event)
        return true;
    /* a change between "attached" and what we think is attached needs service */
    return device_attached() != (dev.state != DEV_NONE);
}

void usbfs_host_poll(void)
{
    bool attached = device_attached();

    if (USBFSH->INT_FG & USBFS_UIF_DETECT)
    {
        USBFSH->INT_FG = USBFS_UIF_DETECT;
        attach_event = true;
    }

    if (!attached)
    {
        attach_event = false;
        if (dev.state != DEV_NONE)
            detach();
        return;
    }

    if (dev.state == DEV_NONE)
    {
        attach_event = false;
        /* a device that will not enumerate must not cost 250 ms of the idle
         * loop over and over, so back off between attempts */
        if (had_fail && systime_elapsed_us(fail_us) < RETRY_AFTER_FAIL_US)
            return;
        Delay_Ms(100);                       /* contact bounce and device power-up */
        if (!device_attached())
            return;
        usbfs_host_attaches++;
        if (enumerate())
        {
            had_fail = false;
        }
        else
        {
            dev.state = DEV_FAILED;
            had_fail  = true;
            fail_us   = systime_us();
            usbfs_host_errors++;
            printf("[FSH] enumeration failed; retrying in a few seconds, or unplug and plug back in\r\n");
        }
        USBFSH->INT_FG = USBFS_UIF_DETECT;   /* our own resets set this; not a new attach */
        return;
    }
    attach_event = false;

    /* the port drops its enable bit by itself when the device goes away */
    if (dev.state != DEV_FAILED && port_enabled && !(USBFSH->HOST_CTRL & USBFS_UH_PORT_EN))
    {
        detach();
        return;
    }
    if (dev.state == DEV_FAILED)
    {
        if (systime_elapsed_us(fail_us) >= RETRY_AFTER_FAIL_US)
            dev.state = DEV_NONE;            /* try the device once more */
        return;
    }

    if (dev.state != DEV_KEYBOARD && dev.state != DEV_MOUSE)
        return;
    if (systime_elapsed_us(dev.last_poll_us) < (uint32_t)dev.interval_ms * 1000u)
        return;
    dev.last_poll_us = systime_us();

    {
        uint8_t  s = transact((USB_PID_IN << 4) | dev.ep_in, dev.tog, 0);
        uint16_t len;

        if (s == H_NAK)
            return;                          /* nothing new: the common answer */
        if (s == H_DISCON)
        {
            detach();
            return;
        }
        if (s == H_STALL)
        {
            printf("[FSH] endpoint stalled, dropping the device\r\n");
            usbfs_host_errors++;
            detach();
            return;
        }
        if (s != H_OK)
        {
            usbfs_host_errors++;
            return;
        }
        dev.tog ^= USBFS_UH_R_TOG;
        len = USBFSH->RX_LEN;
        if (len > sizeof(rx_buf))
            len = sizeof(rx_buf);
        if (dev.state == DEV_KEYBOARD)
            handle_keyboard_report(rx_buf, len);
        else if (len >= 3 && rx_buf[0] != dev.prev[0])
        {
            /* movement arrives every few ms; printing all of it would stall the
             * MSC loop on the UART, so only button changes are reported */
            printf("[FSH] mouse buttons %02X\r\n", rx_buf[0]);
            dev.prev[0] = rx_buf[0];
        }
    }
}

void usbfs_host_status(void)
{
    const char *what = "nothing attached";

    switch (dev.state)
    {
        case DEV_KEYBOARD: what = "keyboard"; break;
        case DEV_MOUSE:    what = "mouse"; break;
        case DEV_OTHER:    what = "device (not a HID keyboard or mouse)"; break;
        case DEV_FAILED:   what = "device that failed to enumerate"; break;
        default:           break;
    }
    printf("[FSH] USB-FS host: %s", what);
    if (dev.state == DEV_KEYBOARD || dev.state == DEV_MOUSE || dev.state == DEV_OTHER)
        printf(", %04X:%04X, %s speed", dev.vid, dev.pid, dev.speed ? "full" : "low");
    printf(" | attaches %lu, keys %lu, errors %lu\r\n",
           (unsigned long)usbfs_host_attaches, (unsigned long)usbfs_host_keys,
           (unsigned long)usbfs_host_errors);
    if (dev.state == DEV_NONE)
        printf("[FSH] if a device is plugged in and nothing appears, the \"VBUS 5V\" header is still open\r\n");
}
