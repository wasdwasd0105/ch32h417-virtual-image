/*
 * FatFs disk I/O glue over the SDIO driver.  The MCU only ever writes to the
 * card from the console repair command (img_purge_tree), which runs while no
 * host command is in flight; images are served through the streaming path.
 * FatFs' sector buffers are not guaranteed to be word aligned, and the SDIO
 * DMA needs that, so unaligned requests go through a bounce buffer.
 */
#include "ff.h"
#include "diskio.h"
#include "sd_sdio.h"
#include <string.h>

__attribute__((aligned(16))) static uint8_t bounce[512];

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return sd_card.present ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;                 /* the application owns card initialisation */
    return sd_card.present ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!sd_card.present)
        return RES_NOTRDY;
    if (((uint32_t)buff & 3u) == 0)
        return sd_read_blocks(buff, (uint32_t)sector, count) == SD_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < count; i++)
    {
        if (sd_read_blocks(bounce, (uint32_t)sector + i, 1) != SD_OK)
            return RES_ERROR;
        memcpy(buff + 512u * i, bounce, 512);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!sd_card.present)
        return RES_NOTRDY;
    if (((uint32_t)buff & 3u) == 0)
        return sd_write_blocks(buff, (uint32_t)sector, count) == SD_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < count; i++)
    {
        memcpy(bounce, buff + 512u * i, 512);
        if (sd_write_blocks(bounce, (uint32_t)sector + i, 1) != SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd)
    {
        case CTRL_SYNC:        return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = sd_card.block_count; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
        default:               return RES_PARERR;
    }
}
