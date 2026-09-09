/*
 * USB Mass Storage Class, Bulk-Only Transport (BOT) + SCSI transparent
 * command set, backed by the microSD card.  Transport agnostic: talks to the
 * USB controller through usb_transport.h.
 */
#ifndef MSC_CORE_H
#define MSC_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* BOT class requests (handled by the transports' EP0 code) */
#define MSC_REQ_BULK_ONLY_RESET   0xFF
#define MSC_REQ_GET_MAX_LUN       0xFE

void msc_init(void);
void msc_task(void);        /* never returns */
/* true while the host holds PREVENT ALLOW MEDIUM REMOVAL on the mounted image:
 * hosts stop polling for removal once they hold the lock, so an unmount behind
 * their back leaves a stale volume (the console refuses unless forced). */
bool msc_image_locked(void);
bool msc_sd_ejected(void);                 /* the card LUN is out of the host's view (host eject, or the console-L bounce) */
void msc_sd_reinsert(void);                /* console L: card LUN absent for 2.5 s, then back = re-inserted for the host */
int  msc_image_unacknowledged(void);       /* 0 none, 1 DVD LUN, 2 disk LUN: image swapped/removed but the host still shows the old volume */

/* statistics for the debug console */
extern volatile uint32_t msc_stat_read_sectors;
extern volatile uint32_t msc_stat_write_sectors;
extern volatile uint32_t msc_stat_commands;
extern volatile uint32_t msc_last_cmd_us;      /* systime of the last CBW received */
extern volatile uint32_t msc_stat_wb_flushes, msc_stat_wb_continued;   /* write-behind: streams closed / commands that continued one */

#endif
