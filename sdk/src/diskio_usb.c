/*
 * diskio_usb.c - FatFs low-level disk interface on top of usb_msc.c
 *
 * One drive ("0:"): the USB mass storage device registered with
 * diskio_usb_attach().  CTRL_SYNC sends SCSI SYNCHRONIZE CACHE so data a
 * stick buffers internally is committed before f_sync()/f_close() return.
 */
#include "ff.h"
#include "diskio.h"
#include "usb_msc.h"
#include "usb.h"

static struct usb_msc *drive;

void diskio_usb_attach(struct usb_msc *m) { drive = m; }

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !drive || !usb_dev()->present) return STA_NOINIT | STA_NODISK;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    return msc_read(drive, (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    return msc_write(drive, (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return msc_sync(drive) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = drive->blocks;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)drive->block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;                      /* erase block size unknown */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
