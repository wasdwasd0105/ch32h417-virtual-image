/*
 * Dispatch from the mass-storage core to whichever USB transport is active.
 */
#include "usb_transport.h"
#include "debug.h"

volatile usb_xport_t usb_active_xport = USB_XPORT_NONE;
volatile uint8_t     usb_configured   = 0;

void usb_xport_rx_arm(uint8_t *buf, uint32_t maxlen)
{
    if (usb_active_xport == USB_XPORT_SS)
        ss_xport_rx_arm(buf, maxlen);
    else if (usb_active_xport == USB_XPORT_HS)
        hs_xport_rx_arm(buf, maxlen);
}

void usb_xport_tx(const uint8_t *buf, uint32_t len)
{
    if (usb_active_xport == USB_XPORT_SS)
        ss_xport_tx(buf, len);
    else if (usb_active_xport == USB_XPORT_HS)
        hs_xport_tx(buf, len);
}

void usb_xport_stall_in(void)
{
    if (usb_active_xport == USB_XPORT_SS)
        ss_xport_stall_in();
    else if (usb_active_xport == USB_XPORT_HS)
        hs_xport_stall_in();
}

void usb_xport_stall_out(void)
{
    if (usb_active_xport == USB_XPORT_SS)
        ss_xport_stall_out();
    else if (usb_active_xport == USB_XPORT_HS)
        hs_xport_stall_out();
}

uint32_t usb_xport_max_packet(void)
{
    if (usb_active_xport == USB_XPORT_SS)
        return 1024u;
    if (usb_active_xport == USB_XPORT_HS)
        return hs_xport_max_packet();
    return 512u;
}

void usb_xport_flush(void)
{
    if (usb_active_xport == USB_XPORT_SS)
        ss_xport_flush();
    else if (usb_active_xport == USB_XPORT_HS)
        hs_xport_flush();
}
