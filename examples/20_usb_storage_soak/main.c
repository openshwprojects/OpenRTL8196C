/*
 * 20_usb_storage_soak - is reading a pendrive reliable?  (read only)
 *
 * 1. Reads the whole device end to end, PASSES times, 64 KB per command,
 *    keeping a CRC32 per megabyte.  Every pass after the first must give
 *    the same CRC for every megabyte.
 * 2. Reports every recovery the transport needed (retries, stalls, resets,
 *    failed commands) and the throughput of each pass.
 * 3. Prints CRC32 and size of every file (all directories) so the numbers
 *    can be compared with `python tools/fat_crc.py <drive>` on a PC.
 * Build it cached for full speed:
 *   python tools/build.py examples/20_usb_storage_soak --load 0x80500000 --stack 0x80600000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "fat.h"
#include "libc.h"

#define PASSES      3
#define CHUNK_SECT  128                  /* 64 KB per READ(10) */
#define MB_SECT     2048
#define MAX_MB      65536

static struct usb_msc msc;
static struct fat_fs fs;
static uint8_t buf[64 * 1024];
static uint8_t filebuf[1024 * 1024];
static uint32_t crc_ref[MAX_MB];

static void print_stats(void)
{
    printf("  transport: %u commands, %u retries, %u failed status, %u phase errors, %u reset recoveries, %u stalls, %u bulk errors\n",
           msc_stats.commands, msc_stats.retries, msc_stats.failed_status, msc_stats.phase_errors,
           msc_stats.reset_recoveries, msc_stats.stalls, msc_stats.bulk_errors);
}

static int soak(void)
{
    uint32_t mbs = msc.blocks / MB_SECT;
    if (mbs > MAX_MB) mbs = MAX_MB;
    uint32_t total_mismatch = 0, total_read_err = 0;
    for (int pass = 0; pass < PASSES; pass++) {
        uint32_t mismatch = 0, read_err = 0, t0 = micros();
        for (uint32_t mb = 0; mb < mbs; mb++) {
            uint32_t crc = 0;
            for (uint32_t s = 0; s < MB_SECT; s += CHUNK_SECT) {
                if (msc_read(&msc, mb * MB_SECT + s, CHUNK_SECT, buf) < 0) {
                    read_err++;
                    if (read_err < 10) printf("  pass %d: read error at sector %u\n", pass + 1, mb * MB_SECT + s);
                    memset(buf, 0, sizeof buf);
                }
                crc = crc32_update(crc, buf, sizeof buf);
            }
            if (pass == 0) crc_ref[mb] = crc;
            else if (crc != crc_ref[mb]) {
                mismatch++;
                if (mismatch < 10) printf("  pass %d: MB %u differs: %08x vs %08x\n", pass + 1, mb, crc, crc_ref[mb]);
            }
            if ((mb & 127) == 127) printf("  pass %d: %u / %u MB\n", pass + 1, mb + 1, mbs);
        }
        uint32_t ms = elapsed_us(t0) / 1000u;
        printf("pass %d done: %u MB in %u s = %u KB/s, %u read errors, %u MB mismatching pass 1\n", pass + 1, mbs,
               ms / 1000u, ms ? mbs * 1024u * 1000u / ms : 0, read_err, mismatch);
        print_stats();
        total_mismatch += mismatch;
        total_read_err += read_err;
    }
    printf("SOAK RESULT: %u passes x %u MB, %u read errors, %u mismatching MB -> %s\n", PASSES, mbs,
           total_read_err, total_mismatch, (total_read_err || total_mismatch) ? "NOT RELIABLE" : "RELIABLE");
    return total_read_err || total_mismatch;
}

/* ------------------------------------------------- file CRCs for the PC */
struct dirlist { struct fat_dirent e[64]; int n; };
static int collect(const struct fat_dirent *e, void *arg)
{
    struct dirlist *d = arg;
    if (e->name[0] != '.' && d->n < 64) d->e[d->n++] = *e;
    return 0;
}

static void file_crcs(uint32_t cluster, const char *path, int depth)
{
    static struct dirlist lists[6];
    if (depth >= 6) return;
    struct dirlist *d = &lists[depth];
    d->n = 0;
    fat_list(&fs, cluster, collect, d);
    for (int i = 0; i < d->n; i++) {
        const struct fat_dirent *e = &d->e[i];
        char full[160];
        snprintf(full, sizeof full, "%s/%s", path, e->name);
        if (e->attr & 0x10) { file_crcs(e->cluster, full, depth + 1); continue; }
        if (e->size > sizeof filebuf) { printf("FILE %10u  (too big for this test)  %s\n", e->size, full); continue; }
        int n = fat_read_file(&fs, e, filebuf, sizeof filebuf);
        printf("FILE %10u  crc32 %08x  %s%s\n", e->size, n >= 0 ? crc32(filebuf, (uint32_t)n) : 0, full,
               n == (int)e->size ? "" : "  (SHORT READ)");
    }
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb storage soak test (read only), running at %08x\n", (unsigned)&main);
    if (usb_host_init() < 0) { printf("usb: controller does not respond\n"); return 1; }
    usb_vbus(1);
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            usb_print_dev(usb_dev());
            memset(&msc_stats, 0, sizeof msc_stats);
            if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
            printf("drive: %s %s, %u sectors (%u MB)\n", msc.vendor, msc.product, msc.blocks, msc.blocks / MB_SECT);
            soak();
            if (fat_mount(&fs, (fat_read_fn)msc_read, &msc) == 0) {
                printf("file checksums (compare with tools/fat_crc.py on a PC):\n");
                file_crcs(0, "", 0);
                printf("FILES END\n");
            }
            print_stats();
            printf("done\n");
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("device removed\n");
        }
        delay_ms(20);
    }
}
