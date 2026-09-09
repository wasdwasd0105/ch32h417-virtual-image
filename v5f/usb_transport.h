/*
 * Interface between the transport-agnostic mass-storage core (msc_core.c)
 * and the two USB device transports:
 *   - USB 3.0 SuperSpeed (USBSS controller, bulk EP1 IN/OUT, 1024 B packets)
 *   - USB 2.0 High/Full speed (USBHS controller, bulk EP2 IN / EP3 OUT)
 *
 * Only one transport is active at a time; usb_active_xport says which.
 * Both transports keep a 2-deep queue for RX buffers and TX buffers so the
 * core can keep one SD access overlapped with one USB transfer.
 */
#ifndef USB_TRANSPORT_H
#define USB_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    USB_XPORT_NONE = 0,
    USB_XPORT_SS,
    USB_XPORT_HS,
} usb_xport_t;

/* Written by the transports from interrupt context */
extern volatile usb_xport_t usb_active_xport;   /* which controller enumerated */
extern volatile uint8_t     usb_configured;     /* SET_CONFIGURATION(1) done   */

/* ---- core -> transport --------------------------------------------------- */
/* Queue a receive buffer.  Completion is reported through
 * msc_evt_rx_complete() once maxlen bytes arrived or a short packet ended the
 * transfer.  At most two buffers may be outstanding.                        */
void usb_xport_rx_arm(uint8_t *buf, uint32_t maxlen);
/* Queue data for the IN endpoint.  Completion: msc_evt_tx_complete().       */
void usb_xport_tx(const uint8_t *buf, uint32_t len);
/* Halt (STALL) the IN / OUT bulk endpoint; queued buffers are discarded.    */
void usb_xport_stall_in(void);
void usb_xport_stall_out(void);
/* Max packet size of the active bulk pipe (1024 / 512 / 64).               */
uint32_t usb_xport_max_packet(void);
/* Discard any queued RX/TX buffers (used when the core restarts).           */
void usb_xport_flush(void);

/* ---- transport -> core (interrupt context) ------------------------------ */
void msc_evt_rx_complete(uint8_t *buf, uint32_t len);
void msc_evt_tx_complete(void);
void msc_evt_reset(void);                    /* bus reset / unconfigured / BOT reset */
void msc_evt_halt_cleared(bool in_endpoint); /* host CLEAR_FEATURE(ENDPOINT_HALT)     */

/* ---- SuperSpeed transport (usbss_msc.c) --------------------------------- */
void ss_xport_rx_arm(uint8_t *buf, uint32_t maxlen);
void ss_xport_tx(const uint8_t *buf, uint32_t len);
void ss_xport_stall_in(void);
void ss_xport_stall_out(void);
void ss_xport_flush(void);
void ss_xport_reset(void);                   /* called on endpoint (re)init */

/* ---- High-speed transport (usbhs_msc.c) --------------------------------- */
void hs_xport_rx_arm(uint8_t *buf, uint32_t maxlen);
void hs_xport_tx(const uint8_t *buf, uint32_t len);
void hs_xport_stall_in(void);
void hs_xport_stall_out(void);
void hs_xport_flush(void);
void hs_xport_reset(void);
uint32_t hs_xport_max_packet(void);

/* Start the USB device: USB 3.0 first, falls back to USB 2.0 automatically. */
void usb_device_start(void);
void usb_device_replug(void);
void usb_link_poll(void);                  /* main loop: owns the SuperSpeed / USB 2.0 controller choice */
void usb_link_dump(void);                  /* console 'u': phase, link state, event counters */
void usb_sup_manual(char c);               /* console: Z pause/resume, j/J USBHS off/on, q/Q USBSS off/on, W both on */
bool usb_host_attached_probe(bool *valid); /* diagnostics: D+/D- pulled down by a host (valid only while USBHS is off) */
bool usb_hs_suspended(void);               /* USB 2.0 controller on but the bus is idle (unplugged or host asleep) */

#endif
