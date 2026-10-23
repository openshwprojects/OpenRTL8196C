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

#define MAX_USB_DRIVES 4
static struct usb_msc *drives[MAX_USB_DRIVES];

void diskio_usb_attach_slot(int pdrv, struct usb_msc *m)
{
    if (pdrv >= 0 && pdrv < MAX_USB_DRIVES)
        drives[pdrv] = m;
}

void diskio_usb_attach(struct usb_msc *m)
{
    drives[0] = m;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv >= MAX_USB_DRIVES) return STA_NOINIT | STA_NODISK;
    struct usb_msc *d = drives[pdrv];
    if (!d || !d->dev || !d->dev->present) return STA_NOINIT | STA_NODISK;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    return msc_read(drives[pdrv], (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    return msc_write(drives[pdrv], (uint32_t)sector, count, buff) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (disk_status(pdrv)) return RES_NOTRDY;
    struct usb_msc *d = drives[pdrv];
    switch (cmd) {
    case CTRL_SYNC:
        return msc_sync(d) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = d->blocks;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)d->block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;                      /* erase block size unknown */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
