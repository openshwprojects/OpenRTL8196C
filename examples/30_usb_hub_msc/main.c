/*
 * 30_usb_hub_msc - USB Hub + Multiple Simultaneous USB Mass Storage Drives
 *
 * Enumerates a USB 2.0/1.1 Hub (class 0x09) on the root port, powers and
 * resets all downstream hub ports, assigns unique USB addresses (2, 3, ...)
 * to each attached USB flash drive, mounts each drive simultaneously
 * (/usb0, /usb1, ... via FatFs 0:..3: or NTFS fallback), lists their root
 * directories, and runs an interleaved cross-drive read verification test.
 */
#include "rtl8196c.h"
#include "board_config.h"
#include "irq.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "spiflash.h"
#include "swcore.h"
#include "net.h"
#include "dhcp.h"
#include "tcp.h"
#include "ota.h"
#include "usb.h"
#include "usb_msc.h"
#include "ff.h"
#include "ntfs.h"
#include "libc.h"

#define MAX_DRIVES 4

static struct usb_msc mscs[MAX_DRIVES];
static FATFS fat_vols[MAX_DRIVES];
static struct ntfs_fs ntfs_vol;
static uint8_t sbuf_a[4096] __attribute__((aligned(32)));
static uint8_t sbuf_b[4096] __attribute__((aligned(32)));

static int msc_ntfs_reader(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return msc_read((struct usb_msc *)ctx, lba, count, buf);
}

static int ntfs_print_cb(const struct ntfs_dirent *e, void *arg)
{
    int *cnt = (int *)arg;
    if (e->name[0] == '$' || e->name[0] == '.')
        return 0;
    if (*cnt < 20) {
        if (e->attr & NTFS_ATTR_DIRECTORY)
            printf("    <DIR>          %s/\n", e->name);
        else
            printf("    %12u B  %s\n", (uint32_t)e->size, e->name);
    }
    (*cnt)++;
    return 0;
}

static void inspect_and_mount_drives(void)
{
    int ndevs = usb_dev_count();
    printf("\n============================================================\n");
    printf("USB Topology: %d device(s) enumerated\n", ndevs);
    printf("============================================================\n");

    for (int i = 0; i < ndevs; i++) {
        const struct usb_dev *d = usb_get_dev(i);
        if (d && d->present)
            usb_print_dev(d);
    }

    int drive_cnt = 0;
    for (int i = 0; i < ndevs && drive_cnt < MAX_DRIVES; i++) {
        const struct usb_dev *d = usb_get_dev(i);
        if (!d || !d->present || d->is_hub)
            continue;

        struct usb_msc *m = &mscs[drive_cnt];
        int rc = msc_init_dev(m, d);
        if (rc < 0) {
            printf("\n[addr %d] Not a ready MSC drive (rc=%d sense=%x/%02x/%02x)\n",
                   d->addr, rc, m->sense_key, m->asc, m->ascq);
            continue;
        }

        int slot = drive_cnt++;
        uint32_t mb = (uint32_t)(((uint64_t)m->blocks * m->block_size) >> 20);
        printf("\n------------------------------------------------------------\n");
        printf("Drive /usb%d (/mnt/sd%c) -> USB addr %d (hub %u port %u)\n",
               slot, 'a' + slot, d->addr, d->hub_addr, d->hub_port);
        printf("  SCSI: \"%s\" \"%s\" rev %s (%s)\n",
               m->vendor, m->product, m->revision, m->removable ? "removable" : "fixed");
        printf("  Size: %u sectors x %u B = %u MB\n", m->blocks, m->block_size, mb);

        diskio_usb_attach_slot(slot, m);
        char drv_path[4] = { (char)('0' + slot), ':', 0, 0 };
        FRESULT fr = f_mount(&fat_vols[slot], drv_path, 1);
        if (fr == FR_OK) {
            static const char *const fst[] = { "?", "FAT12", "FAT16", "FAT32", "exFAT" };
            char label[32];
            DWORD vsn = 0;
            label[0] = 0;
            f_getlabel(drv_path, label, &vsn);
            printf("  FS:   %s (FatFs volume \"%s\", label=\"%s\", SN=%08X)\n",
                   fst[fat_vols[slot].fs_type <= 4 ? fat_vols[slot].fs_type : 0],
                   drv_path, label, (unsigned)vsn);

            DIR dir;
            FILINFO fno;
            if (f_opendir(&dir, drv_path) == FR_OK) {
                int entries = 0;
                printf("  Root directory (/usb%d/):\n", slot);
                while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
                    if (entries < 20) {
                        if (fno.fattrib & AM_DIR)
                            printf("    <DIR>          %s/\n", fno.fname);
                        else
                            printf("    %12u B  %s\n", (unsigned)fno.fsize, fno.fname);
                    }
                    entries++;
                }
                f_closedir(&dir);
                if (entries > 20)
                    printf("    ... (%d total entries)\n", entries);
                else if (entries == 0)
                    printf("    (empty directory)\n");
            }
        } else if (ntfs_mount(&ntfs_vol, msc_ntfs_reader, m) == 0) {
            printf("  FS:   NTFS (label=\"%s\", cluster=%u B)\n",
                   ntfs_vol.label, 1u << ntfs_vol.cluster_shift);
            int entries = 0;
            printf("  Root directory (/usb%d/):\n", slot);
            ntfs_list(&ntfs_vol, NTFS_ROOT, ntfs_print_cb, &entries);
            if (entries > 20)
                printf("    ... (%d total entries)\n", entries);
        } else {
            printf("  FS:   Unrecognized filesystem (f_mount=%d)\n", fr);
        }
    }

    if (drive_cnt >= 2) {
        printf("\n============================================================\n");
        printf("Interleaved Dual-Drive Read Test (/usb0 <-> /usb1)\n");
        printf("============================================================\n");
        uint32_t crc0_first = 0, crc1_first = 0;
        int ok = 1;
        for (int round = 0; round < 4; round++) {
            if (msc_read(&mscs[0], 0, 8, sbuf_a) < 0) { ok = 0; break; }
            if (msc_read(&mscs[1], 0, 8, sbuf_b) < 0) { ok = 0; break; }
            uint32_t c0 = crc32(sbuf_a, 4096);
            uint32_t c1 = crc32(sbuf_b, 4096);
            if (round == 0) {
                crc0_first = c0;
                crc1_first = c1;
            } else if (c0 != crc0_first || c1 != crc1_first) {
                ok = 0;
            }
            printf("  round %d: /usb0 LBA 0..7 crc=%08x | /usb1 LBA 0..7 crc=%08x\n",
                   round, c0, c1);
        }
        printf("Interleaved dual-drive test: %s (%d drives active)\n",
               ok ? "PASS" : "FAIL", drive_cnt);
    } else {
        printf("\nTotal MSC drives mounted: %d\n", drive_cnt);
    }
}

int main(void)
{
    irq_init();
    uart_init();
    timer_delay_init();
    gpio_leds_init();

    uint8_t mac[6];
    static const uint8_t static_ip[4] = BOARD_IP;
    spiflash_factory_mac(mac);
    net_init(mac, static_ip);
    ota_init(BOARD_OTA_PORT);
    dhcp_start("OpenRTL8196C-USBHub");

    printf("\n============================================================\n");
    printf("30_usb_hub_msc — USB Hub + Multi-Drive Mass Storage Test\n");
    printf("============================================================\n");

    usb_debug = 1;
    if (usb_host_init() < 0) {
        printf("usb: controller does not respond\n");
        return 1;
    }
    usb_vbus(1);

    int last_dev_count = 0;
    uint32_t last_hub_poll = timer_ms();

    for (;;) {
        net_poll();
        dhcp_poll();
        ota_poll();
        tcp_poll();

        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            last_dev_count = usb_dev_count();
            inspect_and_mount_drives();
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("root USB device removed\n");
            last_dev_count = 0;
        } else if (ev == USB_EV_ENUM_FAILED) {
            printf("enumeration failed, power-cycling VBUS\n");
            usb_vbus(0);
            delay_ms(800);
            usb_vbus(1);
        } else if (usb_dev()->present && usb_dev()->is_hub &&
                   (uint32_t)(timer_ms() - last_hub_poll) >= 1000u) {
            last_hub_poll = timer_ms();
            if (usb_hub_scan_all() > 0 || usb_dev_count() != last_dev_count) {
                last_dev_count = usb_dev_count();
                inspect_and_mount_drives();
            }
        }
        delay_ms(10);
    }
}
