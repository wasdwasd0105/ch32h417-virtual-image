/*
 * USBSS (USB 3.0 SuperSpeed) device controller: PLL/PHY/link bring-up and the
 * USB 2.0 fallback state machine.  Adapted from the WCH CH32H417 EVT
 * "USBSS/DEVICE/CH372Device" example.
 */
#ifndef USBSS_DEVICE_H
#define USBSS_DEVICE_H

#include "ch32h417.h"
#include "ch32h417_usb.h"
#include "usb_desc.h"

#define DEF_UEP_IN     0x80
#define DEF_UEP_OUT    0x00
#define DEF_UEP_MASK   0x7F
#define DEF_UEP0       0x00
#define DEF_UEP1       0x01
#define DEF_UEP15      0x0F

#define pUSBSS_SetupReqPak  ((PUSB_SETUP_REQ)USBSS_EP0_Buf)

#define USBSS_PHY_CFG_CR    (*((__IO uint32_t *)0x400341f8))
#define USBSS_PHY_CFG_DAT   (*((__IO uint32_t *)0x400341fc))

#define LMP_HP              0
#define LMP_SUBTYPE_MASK    (0xf << 5)
#define LMP_SET_LINK_FUNC   (0x1 << 5)
#define LMP_U2_INACT_TOUT   (0x2 << 5)
#define LMP_VENDOR_TEST     (0x3 << 5)
#define LMP_PORT_CAP        (0x4 << 5)
#define LMP_PORT_CFG        (0x5 << 5)
#define LMP_PORT_CFG_RES    (0x6 << 5)
#define LMP_LINK_SPEED      (1 << 9)
#define NUM_HP_BUF          (4 << 0)
#define DOWN_STREAM         (1 << 16)
#define UP_STREAM           (2 << 16)

typedef enum
{
    UNINIT = 0,
    U3_INI_FRIST = 1,
    U3_INIT_SECOND = 2,
    U2_INIT = 3,
    U2U3_SUCC = 4,
} LINK_State_t;

typedef struct __attribute__((packed))
{
    uint8_t u1_enable;
    uint8_t u2_enable;
    uint8_t set_devaddr;
    uint8_t devaddr;
    uint8_t set_isoch_delay;
    uint16_t set_isoch_value;
} USBSS_Dev_Info_t;

extern __attribute__((aligned(4))) uint8_t USBSS_EP0_Buf[DEF_USBSSD_UEP0_SIZE];
extern USBSS_Dev_Info_t USBSS_Dev_Info;
extern volatile uint8_t  USB_Enum_Status;

/* diagnostics: the last SuperSpeed control requests and link-state transitions (console 'u') */
typedef struct { uint32_t us; uint8_t type, req, stalled; uint16_t val, idx, len; } ss_setup_trace_t;
#define SS_SETUP_TRACE_N 16u
#define SS_LINK_TRACE_N  24u
extern volatile ss_setup_trace_t ss_setup_trace[SS_SETUP_TRACE_N];
extern volatile uint8_t          ss_setup_trace_n;
extern volatile uint32_t         usbss_link_trace[SS_LINK_TRACE_N];   /* (systime_us & ~0xFF) | link state nibble */
extern volatile uint8_t          usbss_link_trace_n;
extern volatile uint8_t          usbss_configured_evt;   /* a host finished SET_CONFIGURATION on SuperSpeed (sticky) */
extern volatile uint8_t  usb_ss_on;        /* USBSS controller enabled (owned by usb_link_poll) */
extern volatile uint32_t Chip;
extern volatile uint16_t usbss_link_state_hist[12];
extern volatile uint32_t usbss_link_irqs, usbss_link_lmp_rx, usbss_link_warm_rst;
extern volatile uint8_t  USBSS_DevConfig;
extern volatile uint8_t  USBSS_DevSleepStatus;
extern volatile uint8_t  USBSS_DevEnumStatus;

void USBSS_Device_Init(FunctionalState sta);
void USBSS_Device_Endp_Init(void);
void USBSS_Device_Endp_Deinit(void);
void USBSS_Reset_Init(FunctionalState sta);
void USBSS_LINK_Handle(USBSSH_TypeDef *USBSSHx);
void SET_Device_Address(uint32_t address, USBSSH_TypeDef *USBSSx);
uint32_t USBSS_PHY_Cfg(uint8_t port_num, uint8_t addr, uint16_t data);
void USBSS_CFG_MOD(void);
void USBSS_PLL_Init(FunctionalState sta);

#endif
