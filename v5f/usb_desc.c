/*
 * USB descriptors: one mass-storage interface (SCSI transparent, BOT).
 */
#include "usb_desc.h"
#include "app_config.h"
#include "ch32h417.h"
#include <string.h>

#define LO(x) ((uint8_t)((x) & 0xFF))
#define HI(x) ((uint8_t)(((x) >> 8) & 0xFF))

/* ======================= USB 3.0 SuperSpeed ============================== */
const uint8_t SS_DeviceDescriptor[] =
{
    0x12, 0x01,             /* bLength, DEVICE */
    0x00, 0x03,             /* bcdUSB 3.00 -- NOT 3.1/3.2: a device claiming those must carry a SuperSpeedPlus
                             * capability in its BOS, and Windows rejects one that does not (macOS does not check) */
    0x00, 0x00, 0x00,       /* class/subclass/protocol in interface */
    0x09,                   /* bMaxPacketSize0 = 2^9 = 512 */
    LO(USB_VID), HI(USB_VID),
    LO(USB_PID), HI(USB_PID),
    LO(USB_BCD_DEVICE), HI(USB_BCD_DEVICE),
    0x01, 0x02, 0x03,       /* iManufacturer, iProduct, iSerialNumber */
    0x01,                   /* bNumConfigurations */
};

const uint8_t SS_ConfigDescriptor[] =
{
    /* configuration: 9 + 9 + 2*(7+6) = 44 */
    0x09, 0x02, 44, 0x00,
    0x01,                   /* bNumInterfaces */
    0x01,                   /* bConfigurationValue */
    0x00,                   /* iConfiguration */
    0x80,                   /* bus powered */
    0x32,                   /* 50 * 8 mA = 400 mA */

    /* interface: mass storage, SCSI transparent, bulk-only */
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,

    /* bulk IN 0x81, 1024 B + SS companion (burst 16) */
    0x07, 0x05, 0x80 | SS_EP_BULK, 0x02, 0x00, 0x04, 0x00,
    0x06, 0x30, SS_BULK_BURST - 1, 0x00, 0x00, 0x00,

    /* bulk OUT 0x01, 1024 B + SS companion (burst 16) */
    0x07, 0x05, SS_EP_BULK, 0x02, 0x00, 0x04, 0x00,
    0x06, 0x30, SS_BULK_BURST - 1, 0x00, 0x00, 0x00,
};

const uint8_t SS_BOSDescriptor[] =
{
    0x05, 0x0F, 0x16, 0x00, 0x02,                   /* BOS, 22 bytes, 2 caps */
    /* USB 2.0 extension: LPM supported */
    0x07, 0x10, 0x02, 0x06, 0x00, 0x00, 0x00,
    /* SuperSpeed device capability */
    0x0A, 0x10, 0x03,
    0x00,                                           /* bmAttributes (no LTM) */
    0x0E, 0x00,                                     /* wSpeedsSupported: FS, HS, SS */
    0x01,                                           /* bFunctionalitySupport: full speed and up */
    0x0A,                                           /* bU1DevExitLat 10 us */
    0xFF, 0x07,                                     /* wU2DevExitLat 2047 us */
};

/* ======================= USB 2.0 High/Full speed ========================= */
const uint8_t HS_DeviceDescriptor[] =
{
    0x12, 0x01,
    0x00, 0x02,             /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,
    DEF_USBD_UEP0_SIZE,
    LO(USB_VID), HI(USB_VID),
    LO(USB_PID), HI(USB_PID),
    LO(USB_BCD_DEVICE), HI(USB_BCD_DEVICE),
    0x01, 0x02, 0x03,
    0x01,
};

const uint8_t HS_ConfigDescriptor[] =
{
    0x09, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 0xC8,   /* 400 mA */
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x80 | HS_EP_IN,  0x02, LO(DEF_USBD_HS_PACK_SIZE), HI(DEF_USBD_HS_PACK_SIZE), 0x00,
    0x07, 0x05, HS_EP_OUT,        0x02, LO(DEF_USBD_HS_PACK_SIZE), HI(DEF_USBD_HS_PACK_SIZE), 0x00,
};

const uint8_t FS_ConfigDescriptor[] =
{
    0x09, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 0xC8,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x80 | HS_EP_IN,  0x02, LO(DEF_USBD_FS_PACK_SIZE), HI(DEF_USBD_FS_PACK_SIZE), 0x00,
    0x07, 0x05, HS_EP_OUT,        0x02, LO(DEF_USBD_FS_PACK_SIZE), HI(DEF_USBD_FS_PACK_SIZE), 0x00,
};

const uint8_t HS_QualifierDescriptor[] =
{
    0x0A, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, DEF_USBD_UEP0_SIZE, 0x01, 0x00,
};

uint8_t HS_OtherSpeedDescriptor[sizeof(FS_ConfigDescriptor)] = { 0x09, 0x07 };
uint8_t FS_OtherSpeedDescriptor[sizeof(HS_ConfigDescriptor)] = { 0x09, 0x07 };

/* ============================== strings ================================== */
const uint8_t StringLangID[] = { 0x04, 0x03, 0x09, 0x04 };

const uint8_t StringManufacturer[] =
{
    2 + 2 * 6, 0x03,
    'w', 0, 'c', 0, 'h', 0, '.', 0, 'c', 0, 'n', 0,
};

const uint8_t StringProduct[] =
{
    2 + 2 * 20, 0x03,
    'C', 0, 'H', 0, '3', 0, '2', 0, 'H', 0, '4', 0, '1', 0, '7', 0, ' ', 0, 'I', 0, 'S', 0, 'O', 0, ' ', 0, 'M', 0, 'o', 0, 'u', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0,
};

/* 24 hex digits from the 96-bit electronic signature unique ID */
uint8_t StringSerial[2 + 2 * 24] = { 2 + 2 * 24, 0x03 };

void usb_desc_init(void)
{
    const uint32_t *uid = (const uint32_t *)0x1FFFF7E8;   /* R32_ESIG_UNIID1..3 */
    int pos = 2;
    for (int w = 0; w < 3; w++)
    {
        uint32_t v = uid[w];
        for (int i = 7; i >= 0; i--)
        {
            StringSerial[pos++] = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF];
            StringSerial[pos++] = 0;
        }
    }
    memcpy(&HS_OtherSpeedDescriptor[2], &FS_ConfigDescriptor[2], sizeof(FS_ConfigDescriptor) - 2);
    memcpy(&FS_OtherSpeedDescriptor[2], &HS_ConfigDescriptor[2], sizeof(HS_ConfigDescriptor) - 2);
}
