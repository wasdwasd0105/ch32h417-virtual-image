/*
 * USBHS (USB 2.0 high/full speed) device controller used as fallback when the
 * host has no SuperSpeed port.  Provides the same bulk transport as the
 * SuperSpeed path (EP2 IN 0x82, EP3 OUT 0x03).
 */
#ifndef USBHS_MSC_H
#define USBHS_MSC_H

#include "ch32h417.h"
#include "ch32h417_usb.h"

void USBHS_Device_Init(FunctionalState sta);
void USBHS_Device_Endp_Init(void);

extern volatile uint8_t USBHS_DevEnumStatus;
extern volatile uint8_t  usb_hs_on;             /* controller enabled */
extern volatile uint8_t  usb_hs_bus_rst_evt;    /* bus reset seen (cleared by usb_link_poll) */
extern volatile uint32_t usb_hs_bus_resets;
extern volatile uint32_t usb_hs_setups, usb_hs_set_addr, usb_hs_set_cfg, usb_hs_suspends;

#endif
