/*
 * 19_usb_storage - pendrive / card reader on the router (read only).
 *
 * On connect: SCSI INQUIRY + READ CAPACITY, read benchmarks, the partition
 * table, then mount the FAT volume, walk every directory and read every
 * file completely twice, checking size and CRC32.  Only names, sizes and
 * checksums are printed, never file contents.  Nothing is written.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "fat.h"
#include "libc.h"

static struct usb_msc msc;
static struct fat_fs fs;
static uint8_t big[256 * 1024] __attribute__((aligned(32)));   /* cache-line aligned: zero-copy DMA */

static int msc_reader(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return msc_read((struct usb_msc *)ctx, lba, count, buf);
}

/* ---------------------------------------------------------- benchmarks */
static void bench(const char *what, uint32_t sectors_per_read, uint32_t total_sectors)
{
    memset(&usb_bulk_stats, 0, sizeof usb_bulk_stats);
    uint32_t t0 = micros();
    for (uint32_t lba = 0; lba < total_sectors; lba += sectors_per_read)
        if (msc_read(&msc, lba, sectors_per_read, big) < 0) { printf("  read error at %u\n", lba); return; }
    uint32_t ms = elapsed_us(t0) / 1000u;
    if (!ms) ms = 1;
    uint32_t kb = total_sectors / 2;
    printf("  %-28s %5u KB in %5u ms = %5u KB/s  (bus %u ms, copy %u ms, %u bulk transfers)\n", what, kb, ms,
           kb * 1000u / ms, usb_bulk_stats.us_xfer / 1000u, usb_bulk_stats.us_copy / 1000u,
           usb_bulk_stats.transfers);
}

static void memcpy_bench(void)
{
    static uint8_t a[16384], b[16384];
    uint32_t t0 = micros();
    for (int i = 0; i < 16; i++) memcpy(b, a, sizeof a);
    uint32_t us = elapsed_us(t0);
    printf("  memcpy 256 KB (this image runs %s): %u ms = %u KB/s\n",
           ((uint32_t)&a >> 29) == 5 ? "uncached" : "cached", us / 1000u, 256u * 1000u / (us / 1000u ? us / 1000u : 1));
}

/* ------------------------------------------------- walk and verify files */
struct walk { int depth; uint32_t files, dirs, bytes, errors, ms; };

static int visit(const struct fat_dirent *e, void *arg);

static void walk_dir(uint32_t cluster, struct walk *w)
{
    /* fat_list uses one shared sector buffer; collect entries first, then recurse */
    static struct fat_dirent ents[8][48];
    if (w->depth >= 8) return;
    struct fat_dirent *list = ents[w->depth];
    struct { struct fat_dirent *list; int n; } ctx = { list, 0 };
    fat_list(&fs, cluster, visit, &ctx);
    for (int i = 0; i < ctx.n; i++) {
        const struct fat_dirent *e = &list[i];
        for (int d = 0; d < w->depth; d++) printf("  ");
        if (e->attr & 0x10) {
            printf("  <DIR>  %s\n", e->name);
            w->dirs++;
            w->depth++;
            walk_dir(e->cluster, w);
            w->depth--;
            continue;
        }
        uint32_t want = e->size < sizeof big ? e->size : sizeof big;
        uint32_t t0 = micros();
        int n1 = fat_read_file(&fs, e, big, sizeof big);
        uint32_t c1 = n1 > 0 ? crc32(big, (uint32_t)n1) : 0;
        int n2 = fat_read_file(&fs, e, big, sizeof big);
        uint32_t c2 = n2 > 0 ? crc32(big, (uint32_t)n2) : 0;
        w->ms += elapsed_us(t0) / 1000u;
        int ok = n1 == (int)want && n2 == n1 && c1 == c2;
        printf("  %10u bytes  read %d/%d  crc %08x %s  %s%s\n", e->size, n1, n2, c1,
               ok ? "OK " : "BAD", e->name, e->size > sizeof big ? "  (first 256 KB only)" : "");
        w->files++;
        w->bytes += n1 > 0 ? (uint32_t)n1 * 2 : 0;
        if (!ok) w->errors++;
    }
}

static int visit(const struct fat_dirent *e, void *arg)
{
    struct { struct fat_dirent *list; int n; } *ctx = arg;
    if (e->name[0] == '.' || ctx->n >= 48) return 0;
    ctx->list[ctx->n++] = *e;
    return 0;
}

static void show_drive(void)
{
    int rc = msc_init(&msc);
    if (rc < 0) {
        printf("msc_init failed %d (sense key %x asc %02x ascq %02x)\n", rc, msc.sense_key, msc.asc, msc.ascq);
        return;
    }
    uint32_t mb = (uint32_t)(((uint64_t)msc.blocks * msc.block_size) >> 20);
    printf("drive: \"%s\" \"%s\" rev %s, %s, %u sectors x %u bytes = %u MB\n", msc.vendor, msc.product,
           msc.revision, msc.removable ? "removable" : "fixed", msc.blocks, msc.block_size, mb);

    printf("benchmarks:\n");
    memcpy_bench();
    bench("4 KB per read", 8, 4096);
    bench("16 KB per read", 32, 4096);
    bench("64 KB per read", 128, 4096);
    bench("256 KB per read", 512, 4096);

    rc = fat_mount(&fs, msc_reader, &msc);
    if (rc < 0) {
        printf("fat_mount failed %d%s\n", rc, rc == -3 ? " (exFAT is not supported)" : "");
        return;
    }
    printf("FAT%d volume \"%s\" at sector %u, %u clusters of %u KB\n", fs.type, fs.label, fs.part_lba,
           fs.clusters, fs.sectors_per_cluster / 2);
    struct walk w;
    memset(&w, 0, sizeof w);
    printf("walking all directories, reading every file twice:\n");
    walk_dir(0, &w);
    printf("summary: %u directories, %u files, %u KB read in %u ms, %u errors\n", w.dirs, w.files,
           w.bytes / 1024, w.ms, w.errors);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb storage example (read only)\n");
    if (usb_host_init() < 0) {
        printf("usb: controller does not respond\n");
        return 1;
    }
    usb_vbus(1);
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            usb_print_dev(usb_dev());
            show_drive();
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("device removed\n");
        } else if (ev == USB_EV_ENUM_FAILED) {
            printf("enumeration failed, power-cycling the port\n");
            usb_vbus(0);
            delay_ms(1000);
            usb_vbus(1);
        }
        delay_ms(20);
    }
}
