/*
 * usb_msc.h - USB mass storage (bulk-only transport, SCSI transparent
 * command set): pendrives, card readers, USB disks.  512-byte sectors.
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

struct usb_msc {
    uint8_t  ep_in, ep_out, iface, lun;
    uint16_t maxp_in, maxp_out;
    uint32_t blocks;           /* number of sectors */
    uint32_t block_size;       /* bytes per sector (512 expected) */
    char     vendor[9], product[17], revision[5];
    uint8_t  removable;
    uint32_t tag;
    /* last SCSI sense data after a failed command */
    uint8_t  sense_key, asc, ascq;
};

/* call after usb_poll() reported USB_EV_CONNECTED; 0 = ready, <0 = not a
 * mass storage device or no medium */
int msc_init(struct usb_msc *m);
int msc_read(struct usb_msc *m, uint32_t lba, uint32_t count, void *buf);
int msc_write(struct usb_msc *m, uint32_t lba, uint32_t count, const void *buf);
int msc_test_unit_ready(struct usb_msc *m);
int msc_sync(struct usb_msc *m);           /* SYNCHRONIZE CACHE */
int msc_write_protected(struct usb_msc *m); /* 1 = WP, 0 = writable, <0 = device did not say */

/* FatFs drive "0:" = this device (sdk/src/diskio_usb.c) */
void diskio_usb_attach(struct usb_msc *m);

/* everything the transport had to recover from; all zero on a clean run */
struct msc_stats {
    uint32_t commands, retries, failed_status, phase_errors, reset_recoveries, stalls, bulk_errors;
    uint32_t residue_cmds;      /* commands whose CSW reported unprocessed bytes */
    uint32_t last_residue;      /* dCSWDataResidue of the most recent such command */
};
extern struct msc_stats msc_stats;

#endif
