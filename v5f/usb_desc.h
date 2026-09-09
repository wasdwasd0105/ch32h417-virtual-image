/*
 * USB descriptors for the mass-storage device (SuperSpeed + High/Full speed).
 */
#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

/* USB 2.0 (USBHS controller) endpoint assignment */
#define DEF_USBD_UEP0_SIZE        64
#define DEF_USBD_HS_PACK_SIZE     512
#define DEF_USBD_FS_PACK_SIZE     64
#define HS_EP_IN                  2        /* bulk IN  0x82 */
#define HS_EP_OUT                 3        /* bulk OUT 0x03 */

/* USB 3.0 (USBSS controller) endpoint assignment */
#define DEF_USBSSD_UEP0_SIZE      512
#define SS_EP_BULK                1        /* bulk IN 0x81 / OUT 0x01, 1024 B */
#define SS_BULK_BURST             16       /* packets per burst (bMaxBurst = 15) */

#define DEF_STRING_DESC_LANG      0x00
#define DEF_STRING_DESC_MANU      0x01
#define DEF_STRING_DESC_PROD      0x02
#define DEF_STRING_DESC_SERN      0x03

extern const uint8_t SS_DeviceDescriptor[];
extern const uint8_t SS_ConfigDescriptor[];
extern const uint8_t SS_BOSDescriptor[];

extern const uint8_t HS_DeviceDescriptor[];
extern const uint8_t HS_ConfigDescriptor[];
extern const uint8_t FS_ConfigDescriptor[];
extern const uint8_t HS_QualifierDescriptor[];
extern uint8_t       HS_OtherSpeedDescriptor[];   /* built at run time */
extern uint8_t       FS_OtherSpeedDescriptor[];

extern const uint8_t StringLangID[];
extern const uint8_t StringManufacturer[];
extern const uint8_t StringProduct[];
extern uint8_t       StringSerial[];              /* built from the chip unique ID */

#define SS_DEVICE_DESC_LEN     (SS_DeviceDescriptor[0])
#define SS_CONFIG_DESC_LEN     ((uint16_t)SS_ConfigDescriptor[2] | ((uint16_t)SS_ConfigDescriptor[3] << 8))
#define SS_BOS_DESC_LEN        ((uint16_t)SS_BOSDescriptor[2] | ((uint16_t)SS_BOSDescriptor[3] << 8))
#define HS_DEVICE_DESC_LEN     (HS_DeviceDescriptor[0])
#define HS_CONFIG_DESC_LEN     ((uint16_t)HS_ConfigDescriptor[2] | ((uint16_t)HS_ConfigDescriptor[3] << 8))
#define FS_CONFIG_DESC_LEN     ((uint16_t)FS_ConfigDescriptor[2] | ((uint16_t)FS_ConfigDescriptor[3] << 8))
#define HS_QUALIFIER_DESC_LEN  (HS_QualifierDescriptor[0])

void usb_desc_init(void);   /* fills in the serial number string */

#endif
