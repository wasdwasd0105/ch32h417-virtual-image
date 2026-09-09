/*
 * Image store: finds .iso / .img files on the card (FAT32 / exFAT via FatFs),
 * mounts one of them as an extent table (runs of contiguous SD sectors) so the
 * mass-storage layer can stream it straight from the card, and implements the
 * selection rules (ISOMOUNT.TXT, last choice, single image).
 * The card is only ever READ here: the host owns the filesystem.
 */
#ifndef IMAGE_STORE_H
#define IMAGE_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

typedef enum { IMG_KIND_NONE = 0, IMG_KIND_ISO, IMG_KIND_DISK } img_kind_t;
/* container format of a disk image (decided when it is mounted) */
typedef enum { IMG_FMT_RAW = 0, IMG_FMT_VHD_FIXED, IMG_FMT_VHD_DYN, IMG_FMT_VMDK_FLAT, IMG_FMT_VMDK_SPARSE } img_fmt_t;
#define IMG_ZERO_RUN 0xFFFFFFFFu   /* img_map(): the run is unallocated in a sparse image -- read as zeros */

typedef struct
{
    char       name[IMG_NAME_MAX];
    uint64_t   size;
    img_kind_t kind;
    uint8_t    in_subdir;        /* 0: card root, 1: /IMG_SUBDIR */
} img_entry_t;

typedef struct { uint32_t file_sector, sd_sector, sectors; } img_extent_t;   /* 512-byte units */

typedef struct
{
    bool         mounted;
    img_kind_t   kind;
    bool         writable;       /* .img only, and only when asked for ("rw") */
    char         name[IMG_NAME_MAX];
    uint8_t      in_subdir;
    uint64_t     size;
    uint32_t     block_size;     /* 2048 for ISO (CD/DVD LUN), 512 for disk images */
    uint32_t     blocks;         /* size / block_size */
    uint32_t     n_extents;
    img_extent_t ext[IMG_MAX_EXTENTS];     /* the file that holds the data (the -flat companion for a flat VMDK) */
    uint32_t     file_sectors;             /* length of that file in 512-byte sectors */
    img_fmt_t    fmt;
    uint32_t     data_offset;              /* raw / fixed VHD / flat VMDK: file sector of virtual sector 0 */
    uint32_t     tbl_sector;               /* VHD dyn: BAT file sector;  VMDK sparse: grain directory file sector */
    uint32_t     unit_sectors;             /* VHD dyn: block size;       VMDK sparse: grain size (sectors) */
    uint32_t     tbl_entries;              /* VHD dyn: BAT entries;      VMDK sparse: GTEs per grain table */
    uint32_t     bitmap_sectors;           /* VHD dyn: sector bitmap in front of every block */
    uint32_t     generation;     /* bumped on every mount/unmount; the MSC layer turns a change into UNIT ATTENTION */
} img_mount_t;

extern img_entry_t img_list[IMG_MAX];
extern uint32_t    img_count;
extern img_mount_t img_cur;
extern bool        img_fs_ok;
extern char        img_fs_type[8];

int      img_scan(void);                                   /* (re)mount FatFs and list images; <0 = no filesystem */
bool     img_mount_index(uint32_t index, bool want_rw);    /* index into img_list */
bool     img_mount_name(const char *name, bool want_rw);   /* by (case-insensitive) file name */
void     img_unmount(void);
bool     img_auto_mount(void);                             /* ISOMOUNT.TXT -> last choice -> the only image */
bool     img_remount_last(void);                           /* after a host "load" / USB re-plug */
/* Map file sectors to SD sectors: returns the length of the contiguous run starting at
 * file sector `fsec` (<= count, 0 = beyond the image) and its first SD sector. */
uint32_t img_map(uint32_t fsec, uint32_t count, uint32_t *sd_sector);
const char *img_fmt_name(void);                          /* "raw", "VHD fixed", ... of the mounted image */
const char *img_fmt_tag(void);                           /* short form for the LCD */
void     img_print_list(void);
void     img_print_status(void);
void     img_purge_tree(const char *root);
bool     img_cycle_next(void);                            /* user key / console 'N': next image in /IMG_SUBDIR, 0 = none; true = a mount is scheduled */
void     img_crc_check(void);                             /* console 'C': CRC-32 of the whole virtual disk */
void     img_key_pos(uint32_t *pos, uint32_t *n);         /* current position for the display */
bool     img_deferred_due(void);                           /* a key-selected image is waiting to be mounted */
bool     img_deferred_waiting(void);                       /* a key-selected mount is scheduled (due or not) */
void     img_deferred_mount(void);                        /* mount it (between transfers only) */                /* repair aid: delete a directory tree on the card */

#endif
