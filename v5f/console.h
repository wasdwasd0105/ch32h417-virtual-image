/* Debug / selection console on USART1 (PA9 TX / PA10 RX, via the WCH-LinkE COM port). */
#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdbool.h>
void console_init(void);
/* Read keys.  Commands that touch the SD card (list, mount, eject, tests) are only
 * queued here and run by console_run_pending(), which the MSC loop calls when no
 * transfer is in progress -- an SD command in the middle of a stream would corrupt it. */
void console_poll(void);
/* Inject one key from somewhere other than the serial port, e.g. a USB
 * keyboard attached to the USB-FS host port.  Same commands. */
void console_feed(int c);
void console_run_pending(void);
bool console_has_pending(void);   /* an SD-touching command is queued */
#endif
