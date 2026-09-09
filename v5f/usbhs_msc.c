/*
 * USBHS device: control endpoint, mass-storage class requests and the bulk
 * EP2 IN / EP3 OUT packet transport.  Adapted from WCH's ch32h417_usbhs_device.c
 * (USBSS/DEVICE/CH372Device and USBHS/DEVICE/MSC_U-Disk examples); keeps the
 * "retry SuperSpeed after a USB 2.0 bus reset" logic of the CH372 example.
 */
#include "usbhs_msc.h"
#include "usbss_device.h"
#include "usb_transport.h"
#include "usb_desc.h"
#include "msc_core.h"
#include "debug.h"
#include <string.h>

#define pUSBHS_SetupReqPak   ((PUSB_SETUP_REQ)USBHS_EP0_Buf)
#define USBHS_SPEED_FULL     0
#define USBHS_SPEED_HIGH     1

__attribute__((aligned(16))) static uint8_t USBHS_EP0_Buf[DEF_USBD_UEP0_SIZE];

static const uint8_t *pUSBHS_Descr;
static volatile uint8_t  USBHS_SetupReqCode;
static volatile uint8_t  USBHS_SetupReqType;
static volatile uint16_t USBHS_SetupReqValue;
static volatile uint16_t USBHS_SetupReqIndex;
static volatile uint16_t USBHS_SetupReqLen;

static volatile uint8_t  USBHS_DevConfig;
static volatile uint8_t  USBHS_DevAddr;
static volatile uint16_t USBHS_DevMaxPackLen = DEF_USBD_HS_PACK_SIZE;
static volatile uint8_t  USBHS_DevSpeed;
static volatile uint8_t  USBHS_DevSleepStatus;
volatile uint8_t  usb_hs_on;            /* USBHS controller enabled (owned by usb_link_poll) */
volatile uint8_t  usb_hs_bus_rst_evt;   /* a bus reset arrived since usb_link_poll last looked */
volatile uint32_t usb_hs_bus_resets;
volatile uint32_t usb_hs_setups, usb_hs_set_addr, usb_hs_set_cfg, usb_hs_suspends;

bool usb_hs_suspended(void)
{
    return usb_hs_on && (USBHS_DevSleepStatus & 0x02);
}

volatile uint8_t         USBHS_DevEnumStatus;

/* ---- bulk transport queues --------------------------------------------- */
typedef struct { uint8_t *buf; uint32_t len; uint32_t done; } hs_q_t;
static hs_q_t           rxq[2];
static volatile uint8_t rxq_w, rxq_r;
static hs_q_t           txq[2];
static volatile uint8_t txq_w, txq_r;
static volatile uint32_t tx_cur_pkt;

static inline void hs_rx_set_res(uint8_t res)
{
    USBHSD->UEP3_RX_CTRL = (USBHSD->UEP3_RX_CTRL & ~USBHS_UEP_R_RES_MASK) | res;
}
static inline void hs_tx_set_res(uint8_t res)
{
    USBHSD->UEP2_TX_CTRL = (USBHSD->UEP2_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | res;
}

static void hs_tx_send_piece(hs_q_t *e)
{
    uint32_t pkt = e->len - e->done;
    if (pkt > USBHS_DevMaxPackLen)
        pkt = USBHS_DevMaxPackLen;
    tx_cur_pkt = pkt;
    USBHSD->UEP2_TX_DMA = (uint32_t)(e->buf + e->done);
    USBHSD->UEP2_TX_LEN = (uint16_t)pkt;
    hs_tx_set_res(USBHS_UEP_T_RES_ACK);
}

uint32_t hs_xport_max_packet(void)
{
    return USBHS_DevMaxPackLen;
}

void hs_xport_reset(void)
{
    rxq_w = rxq_r = 0;
    txq_w = txq_r = 0;
    tx_cur_pkt = 0;
}

void hs_xport_flush(void)
{
    NVIC_DisableIRQ(USBHS_IRQn);
    hs_xport_reset();
    hs_rx_set_res(USBHS_UEP_R_RES_NAK);
    hs_tx_set_res(USBHS_UEP_T_RES_NAK);
    NVIC_EnableIRQ(USBHS_IRQn);
}

void hs_xport_rx_arm(uint8_t *buf, uint32_t maxlen)
{
    NVIC_DisableIRQ(USBHS_IRQn);
    if ((uint8_t)(rxq_w - rxq_r) < 2)
    {
        hs_q_t *e = &rxq[rxq_w & 1];
        e->buf = buf;
        e->len = maxlen;
        e->done = 0;
        rxq_w++;
        if ((uint8_t)(rxq_w - rxq_r) == 1)
        {
            USBHSD->UEP3_RX_DMA = (uint32_t)buf;
            hs_rx_set_res(USBHS_UEP_R_RES_ACK);
        }
    }
    NVIC_EnableIRQ(USBHS_IRQn);
}

void hs_xport_tx(const uint8_t *buf, uint32_t len)
{
    NVIC_DisableIRQ(USBHS_IRQn);
    if ((uint8_t)(txq_w - txq_r) < 2)
    {
        hs_q_t *e = &txq[txq_w & 1];
        e->buf = (uint8_t *)buf;
        e->len = len;
        e->done = 0;
        txq_w++;
        if ((uint8_t)(txq_w - txq_r) == 1)
            hs_tx_send_piece(e);
    }
    NVIC_EnableIRQ(USBHS_IRQn);
}

void hs_xport_stall_in(void)
{
    NVIC_DisableIRQ(USBHS_IRQn);
    txq_w = txq_r = 0;
    hs_tx_set_res(USBHS_UEP_T_RES_STALL);
    NVIC_EnableIRQ(USBHS_IRQn);
}

void hs_xport_stall_out(void)
{
    NVIC_DisableIRQ(USBHS_IRQn);
    rxq_w = rxq_r = 0;
    hs_rx_set_res(USBHS_UEP_R_RES_STALL);
    NVIC_EnableIRQ(USBHS_IRQn);
}

/* ---- controller setup ---------------------------------------------------- */
void USBHS_Device_Endp_Init(void)
{
    USBHSD->UEP_TX_EN = USBHS_UEP0_T_EN | USBHS_UEP2_T_EN;
    USBHSD->UEP_RX_EN = USBHS_UEP0_R_EN | USBHS_UEP3_R_EN;

    USBHSD->UEP_TX_TOG_AUTO = USBHS_UEP2_T_EN;
    USBHSD->UEP_RX_TOG_AUTO = USBHS_UEP3_R_EN;

    USBHSD->UEP0_MAX_LEN = DEF_USBD_UEP0_SIZE;
    USBHSD->UEP2_MAX_LEN = DEF_USBD_HS_PACK_SIZE;
    USBHSD->UEP3_MAX_LEN = DEF_USBD_HS_PACK_SIZE;

    USBHSD->UEP0_DMA = (uint32_t)USBHS_EP0_Buf;
    USBHSD->UEP2_TX_DMA = (uint32_t)USBHS_EP0_Buf;
    USBHSD->UEP3_RX_DMA = (uint32_t)USBHS_EP0_Buf;

    USBHSD->UEP0_TX_LEN = 0;
    USBHSD->UEP0_TX_CTRL = USBHS_UEP_T_RES_NAK;
    USBHSD->UEP0_RX_CTRL = USBHS_UEP_R_RES_ACK;

    USBHSD->UEP2_TX_LEN = 0;
    USBHSD->UEP2_TX_CTRL = USBHS_UEP_T_RES_NAK;
    USBHSD->UEP3_RX_CTRL = USBHS_UEP_R_RES_NAK;

    hs_xport_reset();
    usb_configured = 0;
    USBHS_DevEnumStatus = 0;
    USBHS_DevConfig = 0;
    msc_evt_reset();
}

static void USBHS_RCC_Init(FunctionalState sta)
{
    if (sta)
    {
        if ((RCC->PLLCFGR & RCC_SYSPLL_SEL) != RCC_SYSPLL_USBHS)
        {
            RCC_USBHS_PLLCmd(DISABLE);
            RCC_USBHSPLLCLKConfig(RCC_USBHSPLLSource_HSE);
            RCC_USBHSPLLReferConfig(RCC_USBHSPLLRefer_25M);
            RCC_USBHSPLLClockSourceDivConfig(RCC_USBHSPLL_IN_Div1);
            RCC_USBHS_PLLCmd(ENABLE);
            while (!(RCC->CTLR & RCC_USBHS_PLLRDY))
                ;
        }
        RCC_UTMIcmd(ENABLE);
        RCC_HBPeriphClockCmd(RCC_HBPeriph_USBHS, ENABLE);
    }
    else
    {
        RCC_HBPeriphClockCmd(RCC_HBPeriph_USBHS, DISABLE);
        RCC_UTMIcmd(DISABLE);              /* the SuperSpeed controller does not need the USB 2.0 PHY clock */
        if ((RCC->PLLCFGR & RCC_SYSPLL_SEL) != RCC_SYSPLL_USBHS)
            RCC_USBHS_PLLCmd(DISABLE);
    }
}

void USBHS_Device_Init(FunctionalState sta)
{
    if (sta)
    {
        USBHS_RCC_Init(ENABLE);
        USBHSD->CONTROL = USBHS_UD_RST_LINK | USBHS_UD_PHY_SUSPENDM;
        USBHSD->INT_EN = USBHS_UDIE_BUS_RST | USBHS_UDIE_SUSPEND | USBHS_UDIE_BUS_SLEEP | USBHS_UDIE_LPM_ACT |
                         USBHS_UDIE_TRANSFER | USBHS_UDIE_LINK_RDY;
        USBHS_Device_Endp_Init();
        USBHSD->DEV_AD = 0;                /* a previous session's address must not linger */
        USBHSD->BASE_MODE = USBHS_UD_SPEED_HIGH;
        USBHSD->CONTROL = USBHS_UD_DEV_EN | USBHS_UD_DMA_EN | USBHS_UD_LPM_EN | USBHS_UD_PHY_SUSPENDM;
        NVIC_EnableIRQ(USBHS_IRQn);
        usb_hs_on = 1;
    }
    else
    {
        if (!usb_hs_on)
            return;
        USBHSD->CONTROL = USBHS_UD_RST_SIE | USBHS_UD_RST_LINK;
        NVIC_DisableIRQ(USBHS_IRQn);
        USBHS_RCC_Init(DISABLE);
        usb_hs_on = 0;
        if (usb_active_xport == USB_XPORT_HS)
        {
            usb_active_xport = USB_XPORT_NONE;
            usb_configured = 0;
            msc_evt_reset();
        }
    }
}

/* ======================================================================== */
void USBHS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBHS_IRQHandler(void)
{
    uint8_t intflag, intst, errflag;
    uint16_t len;
    uint8_t endp_num;

    intflag = USBHSD->INT_FG;
    intst = USBHSD->INT_ST;

    if (intflag & USBHS_UDIF_TRANSFER)
    {
        endp_num = intst & USBHS_UDIS_EP_ID_MASK;
        if (!(intst & USBHS_UDIS_EP_DIR))
        {
            /* ---------------- SETUP / OUT ---------------- */
            switch (endp_num)
            {
            case 0:
                USBHSD->UEP0_RX_CTRL &= ~USBHS_UEP_R_DONE;
                if ((USBHSD->UEP0_RX_CTRL & USBHS_UEP_R_SETUP_IS) && !(USBHSD->UEP0_RX_CTRL & USBHS_UEP_R_DONE))
                {
                    usb_hs_setups++;
                    USBHS_SetupReqType  = pUSBHS_SetupReqPak->bRequestType;
                    USBHS_SetupReqCode  = pUSBHS_SetupReqPak->bRequest;
                    USBHS_SetupReqLen   = pUSBHS_SetupReqPak->wLength;
                    USBHS_SetupReqValue = pUSBHS_SetupReqPak->wValue;
                    USBHS_SetupReqIndex = pUSBHS_SetupReqPak->wIndex;

                    len = 0;
                    errflag = 0;
                    if ((USBHS_SetupReqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS)
                    {
                        if (USBHS_SetupReqCode == MSC_REQ_GET_MAX_LUN && (USBHS_SetupReqType & USB_REQ_TYP_IN))
                        {
                            USBHS_EP0_Buf[0] = 2;               /* three LUNs: 0 = DVD, 1 = SD card, 2 = disk image */
                            pUSBHS_Descr = USBHS_EP0_Buf;
                            len = 1;
                            if (USBHS_SetupReqLen > len)
                                USBHS_SetupReqLen = len;
                        }
                        else if (USBHS_SetupReqCode == MSC_REQ_BULK_ONLY_RESET && !(USBHS_SetupReqType & USB_REQ_TYP_IN))
                        {
                            hs_xport_reset();
                            hs_rx_set_res(USBHS_UEP_R_RES_NAK);
                            hs_tx_set_res(USBHS_UEP_T_RES_NAK);
                            msc_evt_reset();
                            USBHS_SetupReqLen = 0;
                        }
                        else
                        {
                            errflag = 0xFF;
                        }
                    }
                    else if ((USBHS_SetupReqType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
                    {
                        errflag = 0xFF;
                    }
                    else
                    {
                        switch (USBHS_SetupReqCode)
                        {
                        case USB_GET_DESCRIPTOR:
                            switch ((uint8_t)(USBHS_SetupReqValue >> 8))
                            {
                            case USB_DESCR_TYP_DEVICE:
                                pUSBHS_Descr = HS_DeviceDescriptor;
                                len = HS_DEVICE_DESC_LEN;
                                break;
                            case USB_DESCR_TYP_CONFIG:
                                if (USBHSD->MIS_ST & USBHS_UDMS_HS_MOD)
                                {
                                    USBHS_DevSpeed = USBHS_SPEED_HIGH;
                                    USBHS_DevMaxPackLen = DEF_USBD_HS_PACK_SIZE;
                                    pUSBHS_Descr = HS_ConfigDescriptor;
                                    len = HS_CONFIG_DESC_LEN;
                                }
                                else
                                {
                                    USBHS_DevSpeed = USBHS_SPEED_FULL;
                                    USBHS_DevMaxPackLen = DEF_USBD_FS_PACK_SIZE;
                                    pUSBHS_Descr = FS_ConfigDescriptor;
                                    len = FS_CONFIG_DESC_LEN;
                                }
                                break;
                            case USB_DESCR_TYP_STRING:
                                switch ((uint8_t)(USBHS_SetupReqValue & 0xFF))
                                {
                                case DEF_STRING_DESC_LANG: pUSBHS_Descr = StringLangID;       len = StringLangID[0];       break;
                                case DEF_STRING_DESC_MANU: pUSBHS_Descr = StringManufacturer; len = StringManufacturer[0]; break;
                                case DEF_STRING_DESC_PROD: pUSBHS_Descr = StringProduct;      len = StringProduct[0];      break;
                                case DEF_STRING_DESC_SERN: pUSBHS_Descr = StringSerial;       len = StringSerial[0];       break;
                                default: errflag = 0xFF; break;
                                }
                                break;
                            case USB_DESCR_TYP_QUALIF:
                                pUSBHS_Descr = HS_QualifierDescriptor;
                                len = HS_QUALIFIER_DESC_LEN;
                                break;
                            case USB_DESCR_TYP_SPEED:
                                if (USBHS_DevSpeed == USBHS_SPEED_HIGH)
                                {
                                    pUSBHS_Descr = HS_OtherSpeedDescriptor;
                                    len = FS_CONFIG_DESC_LEN;
                                }
                                else
                                {
                                    pUSBHS_Descr = FS_OtherSpeedDescriptor;
                                    len = HS_CONFIG_DESC_LEN;
                                }
                                break;
                            case USB_DESCR_TYP_BOS:
                            default:
                                errflag = 0xFF;
                                break;
                            }
                            if (USBHS_SetupReqLen > len)
                                USBHS_SetupReqLen = len;
                            len = (USBHS_SetupReqLen >= DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                            if (!errflag)
                            {
                                memcpy(USBHS_EP0_Buf, pUSBHS_Descr, len);
                                pUSBHS_Descr += len;
                            }
                            break;

                        case USB_SET_ADDRESS:
                            usb_hs_set_addr++;
                            USBHS_DevAddr = (uint8_t)(USBHS_SetupReqValue & 0xFF);
                            break;

                        case USB_GET_CONFIGURATION:
                            USBHS_EP0_Buf[0] = USBHS_DevConfig;
                            if (USBHS_SetupReqLen > 1)
                                USBHS_SetupReqLen = 1;
                            break;

                        case USB_SET_CONFIGURATION:
                            usb_hs_set_cfg++;
                            USBHS_DevConfig = (uint8_t)(USBHS_SetupReqValue & 0xFF);
                            USBHS_DevEnumStatus = USBHS_DevConfig ? 1 : 0;
                            USB_Enum_Status = U2U3_SUCC;
                            USBHS_DevMaxPackLen = (USBHSD->MIS_ST & USBHS_UDMS_HS_MOD) ? DEF_USBD_HS_PACK_SIZE : DEF_USBD_FS_PACK_SIZE;
                            hs_xport_reset();
                            USBHSD->UEP2_TX_CTRL = USBHS_UEP_T_RES_NAK;   /* DATA0 */
                            USBHSD->UEP3_RX_CTRL = USBHS_UEP_R_RES_NAK;
                            usb_active_xport = USB_XPORT_HS;
                            usb_configured = USBHS_DevEnumStatus;
                            msc_evt_reset();
                            break;

                        case USB_CLEAR_FEATURE:
                            if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                            {
                                if ((uint8_t)(USBHS_SetupReqValue & 0xFF) == 0x01)
                                    USBHS_DevSleepStatus &= ~0x01;
                                else
                                    errflag = 0xFF;
                            }
                            else if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                            {
                                if ((uint8_t)(USBHS_SetupReqValue & 0xFF) == USB_REQ_FEAT_ENDP_HALT)
                                {
                                    switch ((uint8_t)(USBHS_SetupReqIndex & 0xFF))
                                    {
                                    case (HS_EP_IN | 0x80):
                                        USBHSD->UEP2_TX_CTRL = USBHS_UEP_T_RES_NAK;    /* clears toggle */
                                        if (txq_r != txq_w)
                                        {
                                            txq[txq_r & 1].done = 0;
                                            hs_tx_send_piece(&txq[txq_r & 1]);
                                        }
                                        msc_evt_halt_cleared(true);
                                        break;
                                    case HS_EP_OUT:
                                        USBHSD->UEP3_RX_CTRL = USBHS_UEP_R_RES_NAK;
                                        if (rxq_r != rxq_w)
                                        {
                                            rxq[rxq_r & 1].done = 0;
                                            USBHSD->UEP3_RX_DMA = (uint32_t)rxq[rxq_r & 1].buf;
                                            hs_rx_set_res(USBHS_UEP_R_RES_ACK);
                                        }
                                        msc_evt_halt_cleared(false);
                                        break;
                                    default:
                                        errflag = 0xFF;
                                        break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            break;

                        case USB_SET_FEATURE:
                            if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                            {
                                if ((uint8_t)(USBHS_SetupReqValue & 0xFF) == USB_REQ_FEAT_REMOTE_WAKEUP)
                                    errflag = 0xFF;             /* remote wakeup not supported */
                                else if ((uint8_t)(USBHS_SetupReqValue & 0xFF) == 0x02)
                                    ;                           /* test mode: accept, not implemented */
                                else
                                    errflag = 0xFF;
                            }
                            else if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                            {
                                if ((uint8_t)(USBHS_SetupReqValue & 0xFF) == USB_REQ_FEAT_ENDP_HALT)
                                {
                                    switch ((uint8_t)(USBHS_SetupReqIndex & 0xFF))
                                    {
                                    case (HS_EP_IN | 0x80):
                                        hs_tx_set_res(USBHS_UEP_T_RES_STALL);
                                        break;
                                    case HS_EP_OUT:
                                        hs_rx_set_res(USBHS_UEP_R_RES_STALL);
                                        break;
                                    default:
                                        errflag = 0xFF;
                                        break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            break;

                        case USB_GET_INTERFACE:
                            USBHS_EP0_Buf[0] = 0x00;
                            if (USBHS_SetupReqLen > 1)
                                USBHS_SetupReqLen = 1;
                            break;

                        case USB_SET_INTERFACE:
                            break;

                        case USB_GET_STATUS:
                            USBHS_EP0_Buf[0] = 0x00;
                            USBHS_EP0_Buf[1] = 0x00;
                            if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
                            {
                                switch ((uint8_t)(USBHS_SetupReqIndex & 0xFF))
                                {
                                case (HS_EP_IN | 0x80):
                                    if ((USBHSD->UEP2_TX_CTRL & USBHS_UEP_T_RES_MASK) == USBHS_UEP_T_RES_STALL)
                                        USBHS_EP0_Buf[0] = 0x01;
                                    break;
                                case HS_EP_OUT:
                                    if ((USBHSD->UEP3_RX_CTRL & USBHS_UEP_R_RES_MASK) == USBHS_UEP_R_RES_STALL)
                                        USBHS_EP0_Buf[0] = 0x01;
                                    break;
                                default:
                                    errflag = 0xFF;
                                    break;
                                }
                            }
                            else if ((USBHS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
                            {
                                if (USBHS_DevSleepStatus & 0x01)
                                    USBHS_EP0_Buf[0] = 0x02;
                            }
                            if (USBHS_SetupReqLen > 2)
                                USBHS_SetupReqLen = 2;
                            break;

                        default:
                            errflag = 0xFF;
                            break;
                        }
                    }

                    if (errflag == 0xFF)
                    {
                        USBHSD->UEP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_STALL;
                        USBHSD->UEP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_STALL;
                    }
                    else
                    {
                        if (USBHS_SetupReqType & USB_REQ_TYP_IN)
                        {
                            len = (USBHS_SetupReqLen > DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                            USBHS_SetupReqLen -= len;
                            USBHSD->UEP0_TX_LEN = len;
                            USBHSD->UEP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
                        }
                        else
                        {
                            if (USBHS_SetupReqLen == 0)
                            {
                                USBHSD->UEP0_TX_LEN = 0;
                                USBHSD->UEP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
                            }
                            else
                            {
                                USBHSD->UEP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
                            }
                        }
                    }
                }
                else
                {
                    /* EP0 OUT data */
                    USBHSD->UEP0_RX_CTRL = USBHS_UEP_R_RES_NAK;
                    len = USBHSD->UEP0_RX_LEN;
                    (void)len;
                    if ((USBHS_SetupReqType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
                        USBHS_SetupReqLen = 0;
                    if (USBHS_SetupReqLen == 0)
                    {
                        USBHSD->UEP0_TX_LEN = 0;
                        USBHSD->UEP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
                    }
                }
                break;

            case HS_EP_OUT:
                USBHSD->UEP3_RX_CTRL &= ~USBHS_UEP_R_DONE;
                if (USBHSD->UEP3_RX_CTRL & USBHS_UEP_R_TOG_MATCH)
                {
                    len = USBHSD->UEP3_RX_LEN;
                    hs_rx_set_res(USBHS_UEP_R_RES_NAK);
                    if (rxq_r != rxq_w)
                    {
                        hs_q_t *e = &rxq[rxq_r & 1];
                        e->done += len;
                        if (e->done >= e->len || len < USBHS_DevMaxPackLen)
                        {
                            uint8_t *buf = e->buf;
                            uint32_t got = e->done;
                            rxq_r++;
                            if (rxq_r != rxq_w)
                            {
                                USBHSD->UEP3_RX_DMA = (uint32_t)rxq[rxq_r & 1].buf;
                                hs_rx_set_res(USBHS_UEP_R_RES_ACK);
                            }
                            msc_evt_rx_complete(buf, got);
                        }
                        else
                        {
                            USBHSD->UEP3_RX_DMA = (uint32_t)(e->buf + e->done);
                            hs_rx_set_res(USBHS_UEP_R_RES_ACK);
                        }
                    }
                }
                else
                {
                    hs_rx_set_res(USBHS_UEP_R_RES_ACK);   /* toggle mismatch: retransmission */
                }
                break;

            default:
                break;
            }
        }
        else
        {
            /* ---------------- IN ---------------- */
            switch (endp_num)
            {
            case 0:
                USBHSD->UEP0_TX_CTRL &= ~USBHS_UEP_T_DONE;
                if (USBHS_SetupReqLen == 0)
                    USBHSD->UEP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
                if ((USBHS_SetupReqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD)
                {
                    switch (USBHS_SetupReqCode)
                    {
                    case USB_GET_DESCRIPTOR:
                        len = USBHS_SetupReqLen >= DEF_USBD_UEP0_SIZE ? DEF_USBD_UEP0_SIZE : USBHS_SetupReqLen;
                        memcpy(USBHS_EP0_Buf, pUSBHS_Descr, len);
                        USBHS_SetupReqLen -= len;
                        pUSBHS_Descr += len;
                        USBHSD->UEP0_TX_LEN = len;
                        USBHSD->UEP0_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                        USBHSD->UEP0_TX_CTRL = (USBHSD->UEP0_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
                        break;
                    case USB_SET_ADDRESS:
                        USBHSD->DEV_AD = USBHS_DevAddr;
                        break;
                    default:
                        USBHSD->UEP0_TX_LEN = 0;
                        break;
                    }
                }
                else
                {
                    USBHSD->UEP0_TX_LEN = 0;
                }
                break;

            case HS_EP_IN:
                USBHSD->UEP2_TX_CTRL &= ~USBHS_UEP_T_DONE;
                hs_tx_set_res(USBHS_UEP_T_RES_NAK);
                if (txq_r != txq_w)
                {
                    hs_q_t *e = &txq[txq_r & 1];
                    e->done += tx_cur_pkt;
                    if (e->done < e->len)
                    {
                        hs_tx_send_piece(e);
                    }
                    else
                    {
                        txq_r++;
                        if (txq_r != txq_w)
                            hs_tx_send_piece(&txq[txq_r & 1]);
                        msc_evt_tx_complete();
                    }
                }
                break;

            default:
                break;
            }
        }
    }
    else if (intflag & USBHS_UDIF_LINK_RDY)
    {
        USBHSD->INT_FG = USBHS_UDIF_LINK_RDY;
    }
    else if (intflag & USBHS_UDIF_SUSPEND)
    {
        USBHSD->INT_FG = USBHS_UDIF_SUSPEND;
        usb_hs_suspends++;
        if (USBHSD->MIS_ST & USBHS_UDMS_SUSPEND)
            USBHS_DevSleepStatus |= 0x02;
        else
            USBHS_DevSleepStatus &= ~0x02;
    }
    else if (intflag & USBHS_UDIF_BUS_RST)
    {
        USBHS_DevConfig = 0;
        USBHS_DevAddr = 0;
        USBHS_DevSleepStatus = 0;
        USBHS_DevEnumStatus = 0;
        USBHSD->DEV_AD = 0;
        if (usb_active_xport != USB_XPORT_SS)       /* a lab-mode USB 2.0 reset must not unconfigure a live SuperSpeed session */
            USBHS_Device_Endp_Init();
        else
            hs_xport_reset();
        USBHSD->INT_FG = USBHS_UDIF_BUS_RST;
        if (usb_active_xport == USB_XPORT_HS)
            usb_active_xport = USB_XPORT_NONE;
        /* a USB 2.0 reset may come from a USB 3.0 host: usb_link_poll() probes for SuperSpeed */
        usb_hs_bus_rst_evt = 1;
        usb_hs_bus_resets++;
    }
    else
    {
        USBHSD->INT_FG = intflag;
    }
}
