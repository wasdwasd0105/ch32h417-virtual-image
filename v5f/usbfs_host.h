/*
 * USB 2.0 full-speed HOST on the board's "USB-FS" Type-C port (PA11 D-, PA12 D+).
 *
 * The USB 3.0 port stays the device link to the computer; this is the other
 * direction: a keyboard, mouse or other full/low-speed device plugs in here.
 *
 * Hardware note: the port's VBUS pin is only connected to the board's 5 V rail
 * through the 2-pin header silkscreened "VBUS 5V" next to the connector.  That
 * header must be closed or nothing can be powered and no device will appear.
 */
#ifndef USBFS_HOST_H
#define USBFS_HOST_H
#include <stdbool.h>
#include <stdint.h>

void usbfs_host_init(void);
/* Quick when nothing changed: reads a register, and polls the device's
 * interrupt endpoint at most once per its bInterval.  Enumeration (only on
 * attach) blocks for a few tens of ms -- call from an idle moment. */
void usbfs_host_poll(void);
/* True when an attach/detach is waiting to be serviced, so the caller can
 * flush write-behind first (same contract as console_has_pending). */
bool usbfs_host_pending(void);
void usbfs_host_status(void);

extern volatile uint32_t usbfs_host_keys;      /* keystrokes delivered to the console */
extern volatile uint32_t usbfs_host_attaches;  /* devices enumerated since boot */
extern volatile uint32_t usbfs_host_errors;    /* transfer failures                 */
#endif
