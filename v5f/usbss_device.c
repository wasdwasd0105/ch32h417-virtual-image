/*
 * USBSS controller bring-up, link-layer handling and USB 2.0 fallback.
 *
 * Derived from WCH's ch32h417_usbss_device.c (CH372Device example).  The
 * endpoint configuration is reduced to one bulk IN/OUT pair (EP1) whose
 * chains are managed by usbss_msc.c.
 */
#include "usbss_device.h"
#include "usbhs_msc.h"
#include "app_config.h"
#include "systime.h"
#include "usbhs_msc.h"
#include "usb_transport.h"
#include "msc_core.h"
#include "debug.h"

USBSS_Dev_Info_t USBSS_Dev_Info;
volatile uint8_t  USB_Enum_Status = UNINIT;
volatile uint32_t Chip = 0;
volatile uint16_t usbss_link_state_hist[12];   /* LINK_STATE_U0..LOOPBACK change counts */
volatile uint8_t  usbss_link_progress;         /* the link got past RxDetect since the last (re)start: a SuperSpeed partner exists */
volatile uint32_t usbss_link_irqs, usbss_link_lmp_rx, usbss_link_warm_rst;

/* ------------------------------------------------------------------------ */
void USBSS_PLL_Init(FunctionalState sta)
{
    if (sta)
    {
        RCC->CTLR |= (uint32_t)RCC_USBSS_PLLON;
        while ((RCC->CTLR & (uint32_t)RCC_USBSS_PLLRDY) != (uint32_t)RCC_USBSS_PLLRDY)
            ;
    }
    else
    {
        RCC->CTLR &= ~(uint32_t)RCC_USBSS_PLLON;
    }
}

static void USBSS_RCC_Init(FunctionalState sta)
{
    if (sta)
    {
        USBSS_PLL_Init(ENABLE);
        RCC_HBPeriphClockCmd(RCC_HBPeriph_USBSS, ENABLE);
        RCC_PIPECmd(ENABLE);
        RCC_UTMIcmd(ENABLE);
        RCC_USBSS_PLLCmd(ENABLE);
    }
    else
    {
        RCC_HBPeriphClockCmd(RCC_HBPeriph_USBSS, DISABLE);
        if (!usb_hs_on)
            RCC_UTMIcmd(DISABLE);          /* the USB 2.0 PHY clock: shared with a running USBHS */
        RCC_PIPECmd(DISABLE);
        if ((RCC->PLLCFGR & RCC_SYSPLL_SEL) != RCC_SYSPLL_USBSS)
            RCC_USBSS_PLLCmd(DISABLE);
    }
}

uint32_t USBSS_PHY_Cfg(uint8_t port_num, uint8_t addr, uint16_t data)
{
    if (port_num == 0)
    {
        USBSS_PHY_CFG_CR = (1 << 23) | (addr << 16) | data;
        USBSS_PHY_CFG_DAT = 0x01;
        return USBSS_PHY_CFG_DAT;
    }
    return 0;
}

void USBSS_CFG_MOD(void)
{
    USBSS_PHY_Cfg(0, 0x03, 0x7c12);
    USBSS_PHY_Cfg(0, 0x0D, 0x79AA);
    USBSS_PHY_Cfg(0, 0x15, 0x4430);
    USBSS_PHY_Cfg(0, 0x13, 0x0010);
    (*((__IO uint32_t *)0x5003C018)) = 0xB0054000;
}

/* ------------------------------------------------------------------------ */
void USBSS_Device_Endp_Deinit(void)
{
    USBSSD->USB_CONTROL |= USBSS_USB_CLR_ALL;
    USBSSD->USB_CONTROL &= ~USBSS_USB_CLR_ALL;
}

void USBSS_Device_Endp_Init(void)
{
    USBSS_Device_Endp_Deinit();

    USBSSD->UEP_TX_EN = USBSS_EP1_TX_EN;
    USBSSD->UEP_RX_EN = USBSS_EP1_RX_EN;

    USBSSD->UEP0_TX_CTRL = 0;
    USBSSD->UEP0_RX_CTRL = 0;
    USBSSD->UEP0_TX_DMA = (uint32_t)USBSS_EP0_Buf;
    USBSSD->UEP0_RX_DMA = (uint32_t)USBSS_EP0_Buf;

    /* EP1 bulk: 1024-byte packets, ERDY NumP = burst size */
    USBSSD->EP1_TX.UEP_TX_DMA_OFS = 1024;
    USBSSD->EP1_RX.UEP_RX_DMA_OFS = 1024;
    USBSSD->EP1_TX.UEP_TX_CR = SS_BULK_BURST;
    USBSSD->EP1_RX.UEP_RX_CR = SS_BULK_BURST;

    ss_xport_reset();
    usb_configured = 0;
    USBSS_DevEnumStatus = 0;
    USBSS_DevConfig = 0;
    msc_evt_reset();
}

void USBSS_Device_Init(FunctionalState sta)
{
    if (sta)
    {
        USBSS_RCC_Init(ENABLE);

        USBSSD->LINK_CFG = LINK_RX_EQ_EN | LINK_TX_DEEMPH_MASK | LINK_PHY_RESET;
        USBSSD->LINK_CTRL = LINK_P2_MODE | LINK_GO_DISABLED;
        USBSSD->LINK_CFG = LINK_RX_EQ_EN | LINK_TX_DEEMPH_MASK | LINK_LTSSM_MODE | LINK_TOUT_MODE;
        USBSSD->LINK_LPM_CR |= LINK_LPM_EN;
        USBSSD->LINK_CFG |= LINK_RX_TERM_EN;
        USBSSD->LINK_INT_CTRL = LINK_IE_TX_LMP | LINK_IE_RX_LMP | LINK_IE_RX_LMP_TOUT | LINK_IE_STATE_CHG |
                                LINK_IE_WARM_RST | LINK_IE_TERM_PRES;
        if (Chip >= 3)
            USBSSD->LINK_INT_CTRL |= LINK_IE_RX_SET_FC;

        USBSSD->LINK_CTRL = LINK_P2_MODE;
        USBSSD->LINK_U1_WKUP_TMR = 120;
        USBSSD->LINK_U1_WKUP_FILTER = 50;
        USBSSD->LINK_U2_WKUP_FILTER = 0;
        USBSSD->LINK_U3_WKUP_FILTER = 0;
        USBSSD->USB_CONTROL |= USBSS_FORCE_RST;
        USBSSD->USB_STATUS = USBSS_UIF_TRANSFER;
        USBSSD->USB_CONTROL = USBSS_UIE_TRANSFER | USBSS_UDIE_SETUP | USBSS_UDIE_STATUS | USBSS_DMA_EN | USBSS_SETUP_FLOW;

        USBSS_CFG_MOD();
        USBSS_Device_Endp_Init();
        NVIC_EnableIRQ(USBSS_IRQn);
        NVIC_EnableIRQ(USBSS_LINK_IRQn);
    }
    else
    {
        NVIC_DisableIRQ(USBSS_LINK_IRQn);
        NVIC_DisableIRQ(USBSS_IRQn);
        USBSSD->USB_CONTROL = USBSS_FORCE_RST;
        USBSSD->LINK_CFG |= LINK_PHY_RESET | U3_LINK_RESET;
        Delay_Us(100);
        USBSSD->USB_CONTROL &= ~USBSS_FORCE_RST;
        USBSSD->LINK_CFG &= ~(LINK_PHY_RESET | U3_LINK_RESET);
        USBSS_RCC_Init(DISABLE);
        if (usb_active_xport == USB_XPORT_SS)
        {
            usb_active_xport = USB_XPORT_NONE;
            usb_configured = 0;
            msc_evt_reset();
        }
    }
}

void USBSS_Reset_Init(FunctionalState sta)
{
    if (sta)
    {
        USBSSD->USB_CONTROL |= USBSS_FORCE_RST;
        USBSSD->USB_STATUS = USBSS_UIF_TRANSFER;
        USBSSD->USB_CONTROL = USBSS_UIE_TRANSFER | USBSS_UDIE_SETUP | USBSS_UDIE_STATUS | USBSS_DMA_EN | USBSS_SETUP_FLOW;
        USBSS_Device_Endp_Init();
    }
    else
    {
        USBSSD->USB_CONTROL = USBSS_FORCE_RST;
        USBSSD->LINK_CFG |= LINK_PHY_RESET | U3_LINK_RESET;
        Delay_Us(100);
        USBSSD->USB_CONTROL &= ~USBSS_FORCE_RST;
        USBSSD->LINK_CFG &= ~(LINK_PHY_RESET | U3_LINK_RESET);
        USBSS_RCC_Init(DISABLE);
    }
}

/* ------------------------------------------------------------------------ */
void SET_Device_Address(uint32_t address, USBSSH_TypeDef *USBSSx)
{
    USBSSx->USB_CONTROL &= 0x00ffffff;
    USBSSx->USB_CONTROL |= (address << 24);
}

void USBSS_LINK_Handle(USBSSH_TypeDef *USBSSHx)
{
    static uint8_t link_low_power = 0;
    uint32_t link_state = USBSSHx->LINK_STATUS & LINK_STATE_MASK;
    uint32_t link_int = USBSSHx->LINK_INT_FLAG;
    uint32_t lmp0;

    if (Chip >= 3)
    {
        if (link_int & LINK_IF_RX_SET_FC)
        {
            USBSSHx->LINK_INT_FLAG = LINK_IF_RX_SET_FC;
            if (USBSSHx->LINK_LMP_PORT_CAP & FORCE_PM)
                USBSSHx->LINK_CFG |= LINK_U1_ALLOW | LINK_U2_ALLOW;
            else
                USBSSHx->LINK_CFG &= ~(LINK_U1_ALLOW | LINK_U2_ALLOW);
        }
    }

    usbss_link_irqs++;
    if (link_int & LINK_IF_STATE_CHG)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_STATE_CHG;
        usbss_link_state_hist[(link_state >> 8) & 0xF]++;
        usbss_link_trace[usbss_link_trace_n++ % SS_LINK_TRACE_N] = (systime_us() & ~0xFFu) | ((link_state >> 8) & 0xFu);
        if (link_state != LINK_STATE_RXDET && link_state != LINK_STATE_DISABLE)
            usbss_link_progress = 1;

        if (link_state == LINK_STATE_RXDET)
        {
            USBSS_Dev_Info.u1_enable = DISABLE;
            USBSS_Dev_Info.u2_enable = DISABLE;
        }
        else if (link_state == LINK_STATE_RECOVERY)
        {
            if (link_low_power)
            {
                link_low_power = 0;
                Delay_Us(100);
                USBSS_PHY_Cfg(0, 0x12, 0x67c8);
            }
        }
        else if (link_state == LINK_STATE_DISABLE)
        {
            /* let the LTSSM go back to RxDetect; usb_link_poll() decides what
             * to do about a link that stays down */
            USBSSHx->LINK_CTRL &= ~LINK_GO_DISABLED;
        }
        else if (link_state == LINK_STATE_U3 || link_state == LINK_STATE_U1 || link_state == LINK_STATE_U2)
        {
            link_low_power = 0x01;
            USBSS_PHY_Cfg(0, 0x12, 0x67c8 & (~(1 << 9)));
        }
        else if (link_state == LINK_STATE_HOTRST)
        {
            USBSS_Dev_Info.u1_enable = DISABLE;
            USBSS_Dev_Info.u2_enable = DISABLE;
            USBSS_Reset_Init(ENABLE);
            USBSS_DevEnumStatus = 0;
            USBSSHx->LINK_CTRL &= ~LINK_HOT_RESET;
        }
    }
    else if (link_int & LINK_IF_TERM_PRES)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_TERM_PRES;
        USBSS_DevEnumStatus = 0;
    }
    else if (link_int & LINK_IF_RX_LMP_TOUT)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_RX_LMP_TOUT;
        USBSSHx->LINK_CTRL |= LINK_GO_DISABLED;
        USBSSHx->LINK_CTRL |= LINK_GO_RX_DET;
    }
    else if (link_int & LINK_IF_TX_LMP)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_TX_LMP;
        USBSSHx->LINK_LMP_TX_DATA0 = LMP_LINK_SPEED | LMP_PORT_CAP | LMP_HP;
        USBSSHx->LINK_LMP_TX_DATA1 = (USBSSHx->LINK_CFG & LINK_DOWN_MODE) ? (DOWN_STREAM | NUM_HP_BUF) : (UP_STREAM | NUM_HP_BUF);
        USBSSHx->LINK_LMP_TX_DATA2 = 0x0;
    }
    else if (link_int & LINK_IF_RX_LMP)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_RX_LMP;
        usbss_link_lmp_rx++;
        lmp0 = USBSSHx->LINK_LMP_RX_DATA0;

        if ((lmp0 & LMP_SUBTYPE_MASK) == LMP_PORT_CFG)
        {
            USBSSHx->LINK_LMP_TX_DATA0 = LMP_LINK_SPEED | LMP_PORT_CFG_RES | LMP_HP;
            USBSSHx->LINK_LMP_TX_DATA1 = 0x0;
            USBSSHx->LINK_LMP_TX_DATA2 = 0x0;
            USBSSHx->LINK_LMP_PORT_CAP |= LINK_LMP_TX_CAP_VLD;
            USB_Enum_Status = U2U3_SUCC;
            if (usb_hs_on)                 /* SuperSpeed won: drop the USB 2.0 side */
                USBHS_Device_Init(DISABLE);
        }
        else if ((lmp0 & LMP_SUBTYPE_MASK) == LMP_U2_INACT_TOUT)
        {
            USBSSHx->LINK_U2_INACT_TIMER = (lmp0 >> 9) & 0xff;
        }
        else if ((lmp0 & LMP_SUBTYPE_MASK) == LMP_SET_LINK_FUNC)
        {
            if (Chip < 3)
            {
                if (USBSSHx->LINK_LMP_RX_DATA0 & (0x02 << 9))
                {
                    USBSSD->LINK_CFG |= LINK_U1_ALLOW | LINK_U2_ALLOW;
                    USBSS_Dev_Info.u1_enable = ENABLE;
                    USBSS_Dev_Info.u2_enable = ENABLE;
                }
                else
                {
                    USBSSD->LINK_CFG &= ~(LINK_U1_ALLOW | LINK_U2_ALLOW);
                    USBSS_Dev_Info.u1_enable = DISABLE;
                    USBSS_Dev_Info.u2_enable = DISABLE;
                }
            }
        }
    }
    else if (link_int & LINK_IF_WARM_RST)
    {
        USBSSHx->LINK_INT_FLAG = LINK_IF_WARM_RST;
        usbss_link_warm_rst++;
        if (USBSSHx->LINK_STATUS & LINK_RX_WARM_RST)
        {
            USBSS_DevEnumStatus = 0;
            USBSS_Reset_Init(ENABLE);
            USBSSHx->LINK_CTRL |= LINK_GO_DISABLED;
            __NOP(); __NOP(); __NOP(); __NOP();
            USBSSHx->LINK_CTRL &= ~LINK_GO_DISABLED;
        }
    }
}


/* ------------------------------------------------------------------------
 * Link supervisor (main-loop, one controller at a time).
 *
 * Both controllers share the one USB 3 connector and the one USB 2.0 PHY
 * (PB8/PB9 = USB2DP/USB2DM, clocked by UTMI).  The known-good states are the
 * ones a fresh power-up reaches, and the two controllers do NOT coexist
 * cleanly on the shared PHY.  So this supervisor never runs both at once, and
 * on any disconnect it hard-resets both USB blocks (RCC HBRSTR) and starts
 * over from the SuperSpeed attempt -- exactly the boot sequence -- rather than
 * trying to un-wedge a half-torn-down controller.
 *
 *   WAIT     USBSS on, USBHS off (the boot state).  A USB 3 host trains the
 *            SuperSpeed link -> SS_ACTIVE.  A USB 2.0-only host holds D+/D-
 *            low (its 15 k pull-downs, read on PB8/PB9) but never trains SS:
 *            after USB_HS_FALLBACK_MS -> HS_ACTIVE.  Nobody attached: hard
 *            reset + restart SuperSpeed every USB_SS_RETRY_MS so a later
 *            plug-in always meets a fresh RxDetect.
 *   SS_ACTIVE  USBSS only.  Link dead (RxDetect/Disabled/Inactive) for
 *            USB_SS_LOST_MS -> unplugged -> hard reset -> WAIT.
 *   HS_ACTIVE  USBHS only.  Bus idle (suspended) for USB_HS_DISC_MS: turn
 *            USBHS off and read D+/D- -- floating -> unplugged -> hard reset
 *            -> WAIT; still pulled low -> a host is there, USBHS back on.  A
 *            fresh USB 2.0 bus reset before we are configured may be a USB 3
 *            host's 2.0 side, so peek for SuperSpeed once -> hard reset -> WAIT.
 * ---------------------------------------------------------------------- */
typedef enum { USB_PH_WAIT, USB_PH_SS_ACTIVE, USB_PH_HS_ACTIVE } usb_phase_t;
static volatile usb_phase_t usb_phase;
static uint32_t phase_us;          /* when the current phase began */
static uint32_t sub_us;            /* generic sub-timer (SS retry / suspend onset) */
static uint32_t attach_us;         /* WAIT: when a host was first seen on D+/D- */
static bool     host_present;      /* WAIT: a host is on the connector */
static bool     ss_dead_seen, hs_idle_seen;
static bool     ss_bounced;        /* SS_ACTIVE: the link left U0-family states since the host last configured us */
static bool     ss_host_known;     /* the host on this cable has enumerated us at SuperSpeed: never fall to USB 2.0 for it */
static bool     ss_keep_said;
uint32_t        usb_stat_ss_keep;  /* SuperSpeed attempts kept alive for a known SuperSpeed host (host rebooting) */
static uint32_t bounce_us;
static bool     sup_paused;        /* console 'Z': supervisor keeps hands off */
volatile uint8_t usb_ss_on;
uint32_t usb_stat_ss_lost, usb_stat_hs_active, usb_stat_waits, usb_stat_ss_starts, usb_stat_ss_wins, usb_stat_resets;

static const char *const phase_name[3] = { "wait", "SS-active", "HS-active" };
static const char *const link_name[12] = { "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive",
                                           "Polling", "Recovery", "HotReset", "Compliance", "Loopback" };

static void ss_stop(void)
{
    if (!usb_ss_on)
        return;
    NVIC_DisableIRQ(USBSS_IRQn);
    NVIC_DisableIRQ(USBSS_LINK_IRQn);
    USBSS_Device_Init(DISABLE);
    usb_ss_on = 0;
}

static void ss_start(void)
{
    USB_Enum_Status = UNINIT;
    usbss_link_progress = 0;
    usbss_configured_evt = 0;
    USBSS_Device_Init(ENABLE);
    usb_ss_on = 1;
    usb_stat_ss_starts++;
}

static void hs_start(void)
{
    if (!usb_hs_on)
        USBHS_Device_Init(ENABLE);
    usb_hs_bus_rst_evt = 0;
}

static void hs_stop(void)
{
    if (usb_hs_on)
        USBHS_Device_Init(DISABLE);
}

/* Hard-reset both USB controllers back to power-up: the only reliable way to
 * recover the shared USB 2.0 PHY between attaches of different link types. */
static void usb_reset_all(void)
{
    ss_stop();
    hs_stop();
    usb_configured = 0;
    usb_active_xport = USB_XPORT_NONE;
    msc_evt_reset();
    RCC_HBPeriphResetCmd(RCC_HBPeriph_USBHS | RCC_HBPeriph_USBSS, ENABLE);
    Delay_Us(60);
    RCC_HBPeriphResetCmd(RCC_HBPeriph_USBHS | RCC_HBPeriph_USBSS, DISABLE);
    usb_stat_resets++;
}

/* Diagnostics + fallback probe: a host holds D+/D- low through its 15 k
 * pull-downs, so with USBHS off the pins PB8/PB9 read low under the chip's
 * internal pull-ups; unplugged they float high.  Valid only while USBHS off. */
static uint32_t probe_us;
static bool     probe_val, probe_done;

static bool usb2_host_attached(void)
{
    GPIO_InitTypeDef g = {0};
    bool attached;
    if (probe_done && systime_elapsed_us(probe_us) < 250000u)
        return probe_val;
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOB, ENABLE);
    g.GPIO_Pin  = GPIO_Pin_8 | GPIO_Pin_9;
    g.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &g);
    Delay_Us(100);
    attached = GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_8) == 0 && GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_9) == 0;
    g.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &g);
    probe_us = systime_us();
    probe_val = attached;
    probe_done = true;
    return attached;
}

bool usb_host_attached_probe(bool *valid)
{
    *valid = !usb_hs_on;
    return *valid ? usb2_host_attached() : false;
}

static uint32_t ss_link_state(void)
{
    return usb_ss_on ? (USBSSD->LINK_STATUS & LINK_STATE_MASK) : LINK_STATE_DISABLE;
}

static bool ss_link_dead(void)
{
    uint32_t st = ss_link_state();
    return st == LINK_STATE_RXDET || st == LINK_STATE_DISABLE || st == LINK_STATE_INACTIVE ||
           st == LINK_STATE_COMPLIANCE || st == LINK_STATE_LOOPBACK;
}

static bool hs_bus_idle(void)
{
    return usb_hs_on && (USBHSD->MIS_ST & USBHS_UDMS_SUSPEND);
}

static void enter(usb_phase_t p)
{
    usb_phase = p;
    phase_us = sub_us = systime_us();
    ss_dead_seen = hs_idle_seen = false;
    ss_bounced = false;
    host_present = false;
    probe_done = false;                /* the next probe really reads the pins */
}

/* boot state: fresh USB blocks, USBSS listening for SuperSpeed */
static void go_wait(void)
{
    usb_stat_waits++;
    usb_reset_all();
    ss_start();
    enter(USB_PH_WAIT);
}

static uint32_t hs_resets_at_entry;      /* USB 2.0 bus resets seen when HS_ACTIVE began */

static void go_hs(void)
{
    usb_stat_hs_active++;
    usb_reset_all();
    hs_start();
    hs_resets_at_entry = usb_hs_bus_resets;
    enter(USB_PH_HS_ACTIVE);
}

void usb_device_start(void)
{
    usb_desc_init();
    Chip = ((DBGMCU_GetCHIPID() >> 4) & 0x0F);
    go_wait();
}

void usb_link_poll(void)
{
    uint32_t t;
    if (sup_paused)
        return;

    switch (usb_phase)
    {
    case USB_PH_WAIT:
        if (usb_active_xport == USB_XPORT_SS || usbss_configured_evt)
        {
            usb_stat_ss_wins++;
            ss_host_known = true;              /* this cable leads to a SuperSpeed host */
            ss_keep_said = false;
            printf("[usb] SuperSpeed enumerated\r\n");
            enter(USB_PH_SS_ACTIVE);
            break;
        }
        /* A USB 3 host pulls D+/D- low exactly like a USB 2.0 one, so the pins
         * only say "something is attached".  Which kind it is has to be
         * decided by giving SuperSpeed a full training window that starts at
         * the attach, not at the start of the phase. */
        if (!host_present)
        {
            if (!usb2_host_attached())
            {   /* nobody home: keep the SuperSpeed attempt fresh for a later plug-in */
                ss_host_known = false;         /* the cable is out: whatever comes next is a new host */
                if (systime_elapsed_us(sub_us) >= (uint32_t)USB_SS_RETRY_MS * 1000u)
                {
                    usb_reset_all();
                    ss_start();
                    sub_us = systime_us();
                }
                break;
            }
            /* attach: restart SuperSpeed so training begins now, and stop
             * probing (the probe momentarily drives the USB 2.0 PHY pins) */
            host_present = true;
            attach_us = systime_us();
            usb_reset_all();
            ss_start();
            break;
        }
        t = systime_elapsed_us(attach_us);
        if ((!usbss_link_progress && t >= (uint32_t)USB_HS_FALLBACK_MS * 1000u) ||
            (usbss_link_progress && t >= (uint32_t)USB_SS_TRAIN_MAX_MS * 1000u))
        {
            if (ss_host_known)
            {
                /* This host already spoke SuperSpeed to us and the cable never left
                 * (its pull-downs stayed): it is rebooting or resetting its USB
                 * controller.  A real USB 3 drive stays SuperSpeed through that,
                 * so do the same -- keep RxDetect fresh, never drop to USB 2.0. */
                if (!ss_keep_said)
                {
                    ss_keep_said = true;
                    printf("[usb] SuperSpeed host quiet (rebooting?): keeping the SuperSpeed attempt, no USB 2.0 fallback\r\n");
                }
                usb_stat_ss_keep++;
                usb_reset_all();
                ss_start();
                attach_us = systime_us();
            }
            else if (!usbss_link_progress)
            {
                printf("[usb] no SuperSpeed partner %lu ms after attach: USB 2.0\r\n", (unsigned long)(t / 1000u));
                go_hs();
            }
            else
            {
                printf("[usb] SuperSpeed trained but never enumerated: USB 2.0\r\n");
                go_hs();
            }
        }
        break;

    case USB_PH_SS_ACTIVE:
        if (ss_link_dead())
        {
            if (!ss_bounced)
            {
                ss_bounced = true;                 /* the host must re-configure us after this */
                bounce_us = systime_us();
            }
            if (!ss_dead_seen)
            {
                ss_dead_seen = true;
                sub_us = systime_us();
            }
            else if (systime_elapsed_us(sub_us) >= (uint32_t)USB_SS_LOST_MS * 1000u)
            {
                usb_stat_ss_lost++;
                printf("[usb] SuperSpeed link lost: waiting for a host\r\n");
                go_wait();
            }
        }
        else
        {
            ss_dead_seen = false;
            if (ss_bounced)
            {
                if (usb_configured)
                    ss_bounced = false;            /* re-enumerated: all good */
                else if (systime_elapsed_us(bounce_us) >= (uint32_t)USB_SS_REENUM_MS * 1000u)
                {
                    /* the link came back but the host never re-configured us: give it a
                     * clean connect (fresh RxDetect) instead of a silent, half-dead device */
                    printf("[usb] SuperSpeed link bounced, host did not re-configure us: fresh attempt\r\n");
                    go_wait();
                }
            }
        }
        break;

    case USB_PH_HS_ACTIVE:
        /* Bus resets are how a USB 2.0 enumeration STARTS -- never tear the
         * controller down here or the host's first SETUP is lost.  A USB 3
         * host plugged in later is caught through the unplug path below,
         * which returns to WAIT with SuperSpeed listening. */
        usb_hs_bus_rst_evt = 0;
        if (hs_bus_idle())
        {
            if (!hs_idle_seen)
            {
                hs_idle_seen = true;
                sub_us = systime_us();
            }
            else if (systime_elapsed_us(sub_us) >= (uint32_t)USB_HS_DISC_MS * 1000u)
            {
                hs_stop();
                if (usb2_host_attached())
                {
                    if (usb_hs_bus_resets == hs_resets_at_entry)
                    {
                        /* attached, yet the host never reset us: it has written this port off
                         * (Windows does after a failed SuperSpeed device).  A real disconnect
                         * is the only thing that makes it look again. */
                        printf("[usb] USB 2.0 host attached but never reset us: re-plugging the USB 2.0 side\r\n");
                        Delay_Ms(USB_REPLUG_MS);
                    }
                    else
                        printf("[usb] USB 2.0 bus idle but host still attached: staying\r\n");
                    hs_start();
                    hs_idle_seen = false;
                    sub_us = systime_us();
                }
                else
                {
                    printf("[usb] USB 2.0 unplugged: waiting for a host\r\n");
                    go_wait();
                }
            }
        }
        else
            hs_idle_seen = false;
        break;
    }
}

/* Drop off the bus for USB_REPLUG_MS and come back so the host re-enumerates
 * (used after an image swap). */
void usb_device_replug(void)
{
    ss_stop();
    hs_stop();
    usb_configured = 0;
    usb_active_xport = USB_XPORT_NONE;
    msc_evt_reset();
    Delay_Ms(USB_REPLUG_MS);
    go_wait();
}

void usb_sup_manual(char c)
{
    switch (c)
    {
    case 'Z': sup_paused = !sup_paused; printf("[usb] supervisor %s\r\n", sup_paused ? "PAUSED" : "running"); break;
    case 'j': hs_stop();  printf("[usb] USBHS off\r\n"); break;
    case 'J': hs_start(); printf("[usb] USBHS on\r\n"); break;
    case 'q': ss_stop();  printf("[usb] USBSS off\r\n"); break;
    case 'Q': ss_start(); printf("[usb] USBSS on\r\n"); break;
    case 'W': go_wait();  printf("[usb] reset -> wait (USBSS)\r\n"); break;
    case 'R': usb_reset_all(); printf("[usb] hard reset both USB blocks\r\n"); break;
    }
}

volatile uint32_t usbss_link_trace[SS_LINK_TRACE_N];
volatile uint8_t  usbss_link_trace_n;

static void usb_trace_dump(void)
{
    static const char *const ln[16] = { "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
                                        "Recovery", "HotReset", "Compliance", "Loopback", "?", "?", "?", "?" };
    uint32_t now = systime_us();
    unsigned n = usbss_link_trace_n < SS_LINK_TRACE_N ? usbss_link_trace_n : SS_LINK_TRACE_N;
    printf("[usb] SS link trace (age ms -> state):");
    for (unsigned i = 0; i < n; i++)
    {
        uint32_t e = usbss_link_trace[(uint8_t)(usbss_link_trace_n - n + i) % SS_LINK_TRACE_N];
        printf(" %lu:%s", (unsigned long)((now - (e & ~0xFFu)) / 1000u), ln[e & 0xFu]);
    }
    printf("\r\n");
    n = ss_setup_trace_n < SS_SETUP_TRACE_N ? ss_setup_trace_n : SS_SETUP_TRACE_N;
    printf("[usb] SS control requests (age ms: type req val idx len%s):", n ? "" : " none");
    for (unsigned i = 0; i < n; i++)
    {
        volatile ss_setup_trace_t *t = &ss_setup_trace[(uint8_t)(ss_setup_trace_n - n + i) % SS_SETUP_TRACE_N];
        printf(" %lu:%02X %02X %04X %04X %u%s", (unsigned long)((now - t->us) / 1000u), t->type, t->req, t->val, t->idx, t->len,
               t->stalled ? "!" : "");
    }
    printf("\r\n");
}

void usb_link_dump(void)
{
    usb_trace_dump();
    printf("[usb] SuperSpeed host known: %s, attempts kept for it: %lu\r\n", ss_host_known ? "yes" : "no", (unsigned long)usb_stat_ss_keep);
    printf("[usb] MSC commands %lu, last %lu ms ago\r\n", (unsigned long)msc_stat_commands,
           (unsigned long)(msc_stat_commands ? systime_elapsed_us(msc_last_cmd_us) / 1000u : 0u));
    uint32_t st = ss_link_state();
    printf("[usb] phase %s for %lu ms, USBSS %s (link %s, progress %d), USBHS %s%s\r\n",
           phase_name[usb_phase], (unsigned long)(systime_elapsed_us(phase_us) / 1000u),
           usb_ss_on ? "on" : "off", usb_ss_on ? link_name[(st >> 8) & 0xF] : "-", usbss_link_progress,
           usb_hs_on ? "on" : "off", usb_hs_suspended() ? " (bus idle/suspended)" : "");
    printf("[usb] events: waits %lu, resets %lu, SS starts %lu, SS wins %lu, SS lost %lu, HS-only %lu%s\r\n",
           (unsigned long)usb_stat_waits, (unsigned long)usb_stat_resets, (unsigned long)usb_stat_ss_starts,
           (unsigned long)usb_stat_ss_wins, (unsigned long)usb_stat_ss_lost, (unsigned long)usb_stat_hs_active,
           sup_paused ? " (supervisor PAUSED)" : "");
    printf("[usb] USBHS: bus resets %lu, setups %lu, set-address %lu, set-config %lu, suspends %lu, UTMI %s\r\n",
           (unsigned long)usb_hs_bus_resets, (unsigned long)usb_hs_setups, (unsigned long)usb_hs_set_addr,
           (unsigned long)usb_hs_set_cfg, (unsigned long)usb_hs_suspends, (RCC->CFGR0 & RCC_UTMION) ? "on" : "off");
}
