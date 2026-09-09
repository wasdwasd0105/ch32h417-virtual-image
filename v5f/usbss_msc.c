/*
 * USBSS (USB 3.0) interrupt handling: control endpoint (standard + mass
 * storage class requests) and the bulk EP1 IN/OUT "chain" transport used by
 * msc_core.c.
 *
 * Chain model of the CH32H417 USBSS controller (see RM 27.2.3 / 27.2.4):
 *   RX: write UEP_RX_DMA + UEP_RX_CHAIN_MAX_NUMP -> endpoint accepts up to
 *       MAX_NUMP packets of DMA_OFS bytes into consecutive buffer slots.  The
 *       chain completes on MAX_NUMP packets, on a short packet, or on PP=0;
 *       UEP_RX_CHAIN_NUMP / UEP_RX_CHAIN_LEN give count and last-packet size.
 *   TX: write UEP_TX_DMA, UEP_TX_CHAIN_LEN (last packet length) and
 *       UEP_TX_CHAIN_EXP_NUMP (packet count) -> hardware sends the burst and
 *       interrupts when all packets are acknowledged.
 * One chain is kept in hardware at a time; a second buffer waits in a small
 * software queue and is armed from the completion interrupt.
 */
#include "usbss_device.h"
#include "usb_transport.h"
#include "msc_core.h"
#include "app_config.h"
#include "debug.h"
#include "systime.h"
#include <string.h>

volatile ss_setup_trace_t ss_setup_trace[SS_SETUP_TRACE_N];
volatile uint8_t          ss_setup_trace_n;
volatile uint8_t          usbss_configured_evt;   /* a host completed SET_CONFIGURATION on SuperSpeed (sticky) */

#if MSC_CHUNK_BYTES > (SS_BULK_BURST * 1024)
#error "MSC_CHUNK_BYTES must not exceed one SuperSpeed burst (16 KiB)"
#endif

__attribute__((aligned(16))) uint8_t USBSS_EP0_Buf[DEF_USBSSD_UEP0_SIZE];

static const uint8_t *pUSBSS_Descr;
static volatile uint8_t  USBSS_SetupReqType;
static volatile uint8_t  USBSS_SetupReqCode;
static volatile uint16_t USBSS_SetupReqValue;
static volatile uint16_t USBSS_SetupReqIndex;
static volatile uint16_t USBSS_SetupReqLen;

volatile uint8_t USBSS_DevConfig;
volatile uint8_t USBSS_DevSleepStatus;
volatile uint8_t USBSS_DevEnumStatus;

/* ---- bulk transport queues --------------------------------------------- */
typedef struct { uint8_t *buf; uint32_t len; } ss_q_t;
static ss_q_t          rxq[2];
static volatile uint8_t rxq_w, rxq_r, rx_armed;
static ss_q_t          txq[2];
static volatile uint8_t txq_w, txq_r, tx_armed;

static void ss_arm_rx_hw(uint8_t *buf, uint32_t maxlen)
{
    uint32_t nump = (maxlen + 1023u) / 1024u;
    if (nump == 0)
        nump = 1;
    if (nump > SS_BULK_BURST)
        nump = SS_BULK_BURST;
    USBSSD->EP1_RX.UEP_RX_DMA = (uint32_t)buf;
    USBSSD->EP1_RX.UEP_RX_DMA_OFS = 1024;
    USBSSD->EP1_RX.UEP_RX_CHAIN_MAX_NUMP = (uint8_t)nump;
}

static void ss_arm_tx_hw(const uint8_t *buf, uint32_t len)
{
    uint32_t nump = (len + 1023u) / 1024u;
    uint32_t last;
    if (nump == 0)
        nump = 1;                            /* zero length packet */
    last = len - (nump - 1u) * 1024u;
    USBSSD->EP1_TX.UEP_TX_DMA = (uint32_t)buf;
    USBSSD->EP1_TX.UEP_TX_DMA_OFS = 1024;
    USBSSD->EP1_TX.UEP_TX_CHAIN_LEN = (uint16_t)last;
    USBSSD->EP1_TX.UEP_TX_CHAIN_EXP_NUMP = (uint8_t)nump;
}

void ss_xport_reset(void)
{
    rxq_w = rxq_r = 0;
    txq_w = txq_r = 0;
    rx_armed = tx_armed = 0;
}

void ss_xport_flush(void)
{
    NVIC_DisableIRQ(USBSS_IRQn);
    if (rx_armed)
        USBSSD->EP1_RX.UEP_RX_CR = USBSS_EP_RX_CHAIN_CLR | SS_BULK_BURST;
    if (tx_armed)
        USBSSD->EP1_TX.UEP_TX_CR = USBSS_EP_TX_CHAIN_CLR | SS_BULK_BURST;
    ss_xport_reset();
    NVIC_EnableIRQ(USBSS_IRQn);
}

void ss_xport_rx_arm(uint8_t *buf, uint32_t maxlen)
{
    NVIC_DisableIRQ(USBSS_IRQn);
    if ((uint8_t)(rxq_w - rxq_r) < 2)
    {
        rxq[rxq_w & 1].buf = buf;
        rxq[rxq_w & 1].len = maxlen;
        rxq_w++;
        if (!rx_armed)
        {
            rx_armed = 1;
            ss_arm_rx_hw(buf, maxlen);
        }
    }
    NVIC_EnableIRQ(USBSS_IRQn);
}

void ss_xport_tx(const uint8_t *buf, uint32_t len)
{
    NVIC_DisableIRQ(USBSS_IRQn);
    if ((uint8_t)(txq_w - txq_r) < 2)
    {
        txq[txq_w & 1].buf = (uint8_t *)buf;
        txq[txq_w & 1].len = len;
        txq_w++;
        if (!tx_armed)
        {
            tx_armed = 1;
            ss_arm_tx_hw(buf, len);
        }
    }
    NVIC_EnableIRQ(USBSS_IRQn);
}

void ss_xport_stall_in(void)
{
    NVIC_DisableIRQ(USBSS_IRQn);
    txq_w = txq_r = 0;
    tx_armed = 0;
    USBSSD->EP1_TX.UEP_TX_CR = USBSS_EP_TX_HALT | USBSS_EP_TX_CHAIN_CLR | SS_BULK_BURST;
    /* USB 3 flow control: if the host's IN already got an NRDY (no chain was armed
     * when it asked), it will not retry until the device sends an ERDY -- and only
     * the retry is answered with STALL.  Without this the host sits in its 30 s
     * command timeout on every failed data-IN command. */
    USBSSD->EP1_TX.UEP_TX_ST = USBSS_EP_TX_ERDY_REQ;
    NVIC_EnableIRQ(USBSS_IRQn);
}

void ss_xport_stall_out(void)
{
    NVIC_DisableIRQ(USBSS_IRQn);
    rxq_w = rxq_r = 0;
    rx_armed = 0;
    USBSSD->EP1_RX.UEP_RX_CR = USBSS_EP_RX_HALT | USBSS_EP_RX_CHAIN_CLR | SS_BULK_BURST;
    USBSSD->EP1_RX.UEP_RX_ST = USBSS_EP_RX_ERDY_REQ;   /* same as the IN side: wake a host waiting after NRDY */
    NVIC_EnableIRQ(USBSS_IRQn);
}

/* ---- endpoint feature helpers (called from the EP0 handler) ------------- */
static uint8_t ss_endp_clear_halt(uint8_t dir_endp)
{
    if ((dir_endp & DEF_UEP_MASK) != SS_EP_BULK)
        return 0xFF;
    if (dir_endp & DEF_UEP_IN)
    {
        USBSSD->EP1_TX.UEP_TX_CR = USBSS_EP_TX_CLR | USBSS_EP_TX_CHAIN_CLR | SS_BULK_BURST;
        USBSSD->EP1_TX.UEP_TX_DMA_OFS = 1024;
        tx_armed = 0;
        if (txq_r != txq_w)            /* something still queued: restart it */
        {
            tx_armed = 1;
            ss_arm_tx_hw(txq[txq_r & 1].buf, txq[txq_r & 1].len);
        }
        msc_evt_halt_cleared(true);
    }
    else
    {
        USBSSD->EP1_RX.UEP_RX_CR = USBSS_EP_RX_CLR | USBSS_EP_RX_CHAIN_CLR | SS_BULK_BURST;
        USBSSD->EP1_RX.UEP_RX_DMA_OFS = 1024;
        rx_armed = 0;
        if (rxq_r != rxq_w)
        {
            rx_armed = 1;
            ss_arm_rx_hw(rxq[rxq_r & 1].buf, rxq[rxq_r & 1].len);
        }
        msc_evt_halt_cleared(false);
    }
    return 0;
}

static uint8_t ss_endp_set_halt(uint8_t dir_endp)
{
    if ((dir_endp & DEF_UEP_MASK) != SS_EP_BULK)
        return 0xFF;
    if (dir_endp & DEF_UEP_IN)
        USBSSD->EP1_TX.UEP_TX_CR |= USBSS_EP_TX_HALT;
    else
        USBSSD->EP1_RX.UEP_RX_CR |= USBSS_EP_RX_HALT;
    return 0;
}

static uint8_t ss_endp_get_status(uint8_t dir_endp)
{
    if ((dir_endp & DEF_UEP_MASK) == DEF_UEP0)
        return 0;
    if ((dir_endp & DEF_UEP_MASK) != SS_EP_BULK)
        return 0xFF;
    if (dir_endp & DEF_UEP_IN)
    {
        if (USBSSD->EP1_TX.UEP_TX_CR & USBSS_EP_TX_HALT)
            USBSS_EP0_Buf[0] = 0x01;
    }
    else
    {
        if (USBSSD->EP1_RX.UEP_RX_CR & USBSS_EP_RX_HALT)
            USBSS_EP0_Buf[0] = 0x01;
    }
    return 0;
}

/* ======================================================================== */
void USBSS_LINK_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBSS_LINK_IRQHandler(void)
{
    USBSS_LINK_Handle(USBSSH);
}

void USBSS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBSS_IRQHandler(void)
{
    uint8_t  endp_num, errflag;
    uint16_t len;
    uint32_t status = USBSSD->USB_STATUS;

    if ((status & USBSS_UDIF_SETUP) && !(status & USBSS_UDIF_STATUS))
    {
        USBSS_Dev_Info.set_devaddr = 0;
        USBSS_Dev_Info.set_isoch_delay = 0;

        USBSS_SetupReqType  = pUSBSS_SetupReqPak->bRequestType;
        USBSS_SetupReqCode  = pUSBSS_SetupReqPak->bRequest;
        USBSS_SetupReqLen   = pUSBSS_SetupReqPak->wLength;
        USBSS_SetupReqValue = pUSBSS_SetupReqPak->wValue;
        USBSS_SetupReqIndex = pUSBSS_SetupReqPak->wIndex;
        {
            volatile ss_setup_trace_t *tr = &ss_setup_trace[ss_setup_trace_n++ % SS_SETUP_TRACE_N];
            tr->us = systime_us(); tr->type = USBSS_SetupReqType; tr->req = USBSS_SetupReqCode;
            tr->val = USBSS_SetupReqValue; tr->idx = USBSS_SetupReqIndex; tr->len = USBSS_SetupReqLen; tr->stalled = 0;
        }

        len = 0;
        errflag = 0;
        if ((USBSS_SetupReqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS)
        {
            /* mass storage class requests */
            if (USBSS_SetupReqCode == MSC_REQ_GET_MAX_LUN && (USBSS_SetupReqType & USB_REQ_TYP_IN))
            {
                USBSS_EP0_Buf[0] = 2;               /* three LUNs: 0 = DVD, 1 = SD card, 2 = disk image */
                if (USBSS_SetupReqLen > 1)
                    USBSS_SetupReqLen = 1;
            }
            else if (USBSS_SetupReqCode == MSC_REQ_BULK_ONLY_RESET && !(USBSS_SetupReqType & USB_REQ_TYP_IN))
            {
                ss_xport_reset();
                msc_evt_reset();
                USBSS_SetupReqLen = 0;
            }
            else
            {
                errflag = 0xFF;
            }
        }
        else if ((USBSS_SetupReqType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
        {
            errflag = 0xFF;
        }
        else
        {
            switch (USBSS_SetupReqCode)
            {
                case USB_GET_DESCRIPTOR:
                    switch ((uint8_t)(USBSS_SetupReqValue >> 8))
                    {
                        case USB_DESCR_TYP_DEVICE:
                            pUSBSS_Descr = SS_DeviceDescriptor;
                            len = SS_DEVICE_DESC_LEN;
                            break;
                        case USB_DESCR_TYP_CONFIG:
                            pUSBSS_Descr = SS_ConfigDescriptor;
                            len = SS_CONFIG_DESC_LEN;
                            break;
                        case USB_DESCR_TYP_STRING:
                            switch ((uint8_t)(USBSS_SetupReqValue & 0xFF))
                            {
                                case DEF_STRING_DESC_LANG: pUSBSS_Descr = StringLangID;       len = StringLangID[0];       break;
                                case DEF_STRING_DESC_MANU: pUSBSS_Descr = StringManufacturer; len = StringManufacturer[0]; break;
                                case DEF_STRING_DESC_PROD: pUSBSS_Descr = StringProduct;      len = StringProduct[0];      break;
                                case DEF_STRING_DESC_SERN: pUSBSS_Descr = StringSerial;       len = StringSerial[0];       break;
                                default: errflag = 0xFF; break;
                            }
                            break;
                        case USB_DESCR_TYP_QUALIF:
                            pUSBSS_Descr = HS_QualifierDescriptor;
                            len = HS_QUALIFIER_DESC_LEN;
                            break;
                        case USB_DESCR_TYP_BOS:
                            pUSBSS_Descr = SS_BOSDescriptor;
                            len = SS_BOS_DESC_LEN;
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                    }
                    if (USBSS_SetupReqLen > len)
                        USBSS_SetupReqLen = len;
                    len = (USBSS_SetupReqLen >= DEF_USBSSD_UEP0_SIZE) ? DEF_USBSSD_UEP0_SIZE : USBSS_SetupReqLen;
                    if (!errflag)
                    {
                        memcpy(USBSS_EP0_Buf, pUSBSS_Descr, len);
                        pUSBSS_Descr += len;
                    }
                    break;

                case USB_SET_ADDRESS:
                    USBSS_Dev_Info.set_devaddr = 1;
                    USBSS_Dev_Info.devaddr = (uint8_t)(USBSS_SetupReqValue & 0xFF);
                    break;

                case USB_GET_CONFIGURATION:
                    USBSS_EP0_Buf[0] = USBSS_DevConfig;
                    if (USBSS_SetupReqLen > 1)
                        USBSS_SetupReqLen = 1;
                    break;

                case USB_SET_ISOCH_DLY:
                    USBSS_Dev_Info.set_isoch_delay = 1;
                    USBSS_Dev_Info.set_isoch_value = USBSS_SetupReqValue;
                    break;

                case USB_SET_SEL:
                    break;

                case USB_SET_CONFIGURATION:
                    USBSS_DevConfig = (uint8_t)(USBSS_SetupReqValue & 0xFF);
                    if (USBSS_DevConfig && (USBSS_DevConfig != SS_ConfigDescriptor[5]))
                    {
                        USBSS_DevConfig = 0;
                        errflag = 0xFF;
                    }
                    USBSS_DevEnumStatus = USBSS_DevConfig ? 1 : 0;
                    USBSS_Dev_Info.u1_enable = DISABLE;
                    USBSS_Dev_Info.u2_enable = DISABLE;
                    /* (re)start the mass-storage protocol on this transport */
                    ss_xport_reset();
                    USBSSD->EP1_TX.UEP_TX_CR = USBSS_EP_TX_CLR | USBSS_EP_TX_CHAIN_CLR | SS_BULK_BURST;
                    USBSSD->EP1_RX.UEP_RX_CR = USBSS_EP_RX_CLR | USBSS_EP_RX_CHAIN_CLR | SS_BULK_BURST;
                    USBSSD->EP1_TX.UEP_TX_DMA_OFS = 1024;
                    USBSSD->EP1_RX.UEP_RX_DMA_OFS = 1024;
                    usb_active_xport = USB_XPORT_SS;
                    usb_configured = USBSS_DevEnumStatus;
                    if (USBSS_DevEnumStatus)
                        usbss_configured_evt = 1;   /* sticky: the supervisor commits to SuperSpeed even if a
                                                     * following reset clears usb_active_xport before it looks */
                    msc_evt_reset();
                    break;

                case USB_CLEAR_FEATURE:
                    if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                    {
                        uint8_t f = (uint8_t)(USBSS_SetupReqValue & 0xFF);
                        if (f == 0x01)
                            USBSS_DevSleepStatus &= ~0x01;
                        else if (f == USB_U1_ENABLE || f == USB_U2_ENABLE)
                            ;   /* U1/U2 stay disabled (DEF_UP_U1_EN not set) */
                        else
                            errflag = 0xFF;
                    }
                    else if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                    {
                        if ((uint8_t)(USBSS_SetupReqValue & 0xFF) == USB_REQ_FEAT_ENDP_HALT)
                            errflag = ss_endp_clear_halt((uint8_t)(USBSS_SetupReqIndex & 0xFF));
                        else
                            errflag = 0xFF;
                    }
                    else
                    {
                        errflag = 0xFF;
                    }
                    break;

                case USB_SET_FEATURE:
                    if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                    {
                        uint8_t f = (uint8_t)(USBSS_SetupReqValue & 0xFF);
                        if (f == USB_U1_ENABLE || f == USB_U2_ENABLE)
                        {
                            if (!USBSS_DevEnumStatus)
                                errflag = 0xFF;
                        }
                        else
                        {
                            errflag = 0xFF;
                        }
                    }
                    else if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                    {
                        if ((uint8_t)(USBSS_SetupReqValue & 0xFF) == USB_REQ_FEAT_ENDP_HALT)
                            errflag = ss_endp_set_halt((uint8_t)(USBSS_SetupReqIndex & 0xFF));
                        else
                            errflag = 0xFF;
                    }
                    break;

                case USB_SET_INTERFACE:
                    break;

                case USB_GET_INTERFACE:
                    USBSS_EP0_Buf[0] = 0x00;
                    if (USBSS_SetupReqLen > 1)
                        USBSS_SetupReqLen = 1;
                    break;

                case USB_GET_STATUS:
                    USBSS_EP0_Buf[0] = 0x00;
                    USBSS_EP0_Buf[1] = 0x00;
                    if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                    {
                        errflag = ss_endp_get_status((uint8_t)(USBSS_SetupReqIndex & 0xFF));
                    }
                    else if ((USBSS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                    {
                        if (USBSS_DevSleepStatus & 0x01)
                            USBSS_EP0_Buf[0] = 0x02;
                        USBSS_EP0_Buf[0] |= (USBSS_Dev_Info.u2_enable << 3) | (USBSS_Dev_Info.u1_enable << 2);
                    }
                    if (USBSS_SetupReqLen > 2)
                        USBSS_SetupReqLen = 2;
                    break;

                case USB_SET_ENDPOINT:
                    break;

                default:
                    errflag = 0xFF;
                    break;
            }
        }

        if (errflag == 0xFF)
        {
            ss_setup_trace[(uint8_t)(ss_setup_trace_n - 1u) % SS_SETUP_TRACE_N].stalled = 1;
            USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_STALL;
            USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_STALL;
        }
        else
        {
            if (USBSS_SetupReqType & DEF_UEP_IN)
            {
                if (USBSS_SetupReqLen == 0)
                {
                    USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_ACK;
                }
                else
                {
                    len = (USBSS_SetupReqLen > DEF_USBSSD_UEP0_SIZE) ? DEF_USBSSD_UEP0_SIZE : USBSS_SetupReqLen;
                    USBSS_SetupReqLen -= len;
                    USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_DPH | len;
                    USBSSD->UEP0_TX_CTRL |= USBSS_EP0_TX_ERDY;
                }
            }
            else
            {
                if (USBSS_SetupReqLen == 0)
                {
                    USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_DPH;
                    USBSSD->UEP0_TX_CTRL |= USBSS_EP0_TX_ERDY;
                    USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_ACK;
                }
                else
                {
                    USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_ACK;
                }
            }
        }
        USBSSD->USB_STATUS = USBSS_UDIF_SETUP;
    }
    else if (status & USBSS_UDIF_STATUS)
    {
        USBSSD->USB_STATUS = USBSS_UDIF_STATUS;
        if (USBSS_Dev_Info.set_devaddr)
        {
            SET_Device_Address(USBSS_Dev_Info.devaddr, USBSSH);
            USBSS_Dev_Info.set_devaddr = 0;
        }
        else if (USBSS_Dev_Info.set_isoch_delay)
        {
            USBSSD->LINK_ISO_DLY = USBSS_Dev_Info.set_isoch_value;
            USBSS_Dev_Info.set_isoch_delay = 0;
        }
        USBSSD->UEP0_TX_CTRL = 0;
        USBSSD->UEP0_RX_CTRL = 0;
    }
    else if (status & USBSS_UIF_TRANSFER)
    {
        endp_num = (status & USBSS_EP_ID_MASK) >> 8;
        if (status & USBSS_EP_DIR_MASK)
        {
            /* ---- IN completion ---- */
            switch (endp_num)
            {
                case DEF_UEP0:
                    if (USBSS_SetupReqLen == 0)
                    {
                        USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_DPH;
                        USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_ACK;
                    }
                    else
                    {
                        switch (USBSS_SetupReqCode)
                        {
                            case USB_GET_DESCRIPTOR:
                                len = USBSS_SetupReqLen >= DEF_USBSSD_UEP0_SIZE ? DEF_USBSSD_UEP0_SIZE : USBSS_SetupReqLen;
                                memcpy(USBSS_EP0_Buf, pUSBSS_Descr, len);
                                USBSS_SetupReqLen -= len;
                                pUSBSS_Descr += len;
                                USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_DPH | len | ((((USBSSD->UEP0_TX_CTRL >> 16) & 0x1F) + 1) << 16);
                                USBSSD->UEP0_TX_CTRL |= USBSS_EP0_TX_ERDY;
                                break;
                            default:
                                USBSSD->UEP0_TX_CTRL = USBSS_EP0_TX_DPH;
                                break;
                        }
                    }
                    break;

                case SS_EP_BULK:
                    USBSSD->EP1_TX.UEP_TX_CHAIN_ST |= USBSS_EP_TX_CHAIN_IF;
                    if (tx_armed && txq_r != txq_w)
                    {
                        txq_r++;
                        tx_armed = 0;
                        if (txq_r != txq_w)
                        {
                            tx_armed = 1;
                            ss_arm_tx_hw(txq[txq_r & 1].buf, txq[txq_r & 1].len);
                        }
                        msc_evt_tx_complete();
                    }
                    break;

                default:
                    break;
            }
        }
        else
        {
            /* ---- OUT completion ---- */
            switch (endp_num)
            {
                case DEF_UEP0:
                    USBSSD->UEP0_RX_CTRL = USBSS_EP0_RX_ERDY | USBSS_EP0_RX_ACK;
                    break;

                case SS_EP_BULK:
                {
                    uint32_t nump = USBSSD->EP1_RX.UEP_RX_CHAIN_NUMP;
                    uint32_t last = USBSSD->EP1_RX.UEP_RX_CHAIN_LEN;
                    uint32_t total = nump ? (nump - 1u) * 1024u + last : 0u;
                    USBSSD->EP1_RX.UEP_RX_CHAIN_ST |= USBSS_EP_RX_CHAIN_IF;
                    if (rx_armed && rxq_r != rxq_w)
                    {
                        uint8_t *buf = rxq[rxq_r & 1].buf;
                        rxq_r++;
                        rx_armed = 0;
                        if (rxq_r != rxq_w)
                        {
                            rx_armed = 1;
                            ss_arm_rx_hw(rxq[rxq_r & 1].buf, rxq[rxq_r & 1].len);
                        }
                        msc_evt_rx_complete(buf, total);
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }
    else
    {
        /* unexpected flags (FIFO overflow etc.): clear */
        USBSSD->USB_STATUS = status & 0xFF;
    }
}
