/*
 * 22_usb_ntfs - read an NTFS-formatted USB stick or disk (read only).
 *
 * Mounts the first NTFS volume, prints volume info, walks every directory
 * and reads every file completely in 1 MB pieces, printing
 *   FILE <size> crc32 <crc> <path>
 * for each, so the result can be checked on a PC:
 *   python tools/fat_crc.py E:\ --compare board.log
 * Files are read until CRC_BUDGET bytes have been read; later files are
 * listed with "skipped".  Nothing is ever written to the disk.
 *
 * Build cached: python tools/build.py examples/22_usb_ntfs --load 0x80500000 --stack 0x80F00000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "ntfs.h"
#include "libc.h"

#define CRC_BUDGET (2048ull * 1024 * 1024)   /* bytes read for CRCs per run */
#define POOL       8192                       /* directory entries held during the walk */
#define MAX_DEPTH  16

static struct usb_msc msc;
static struct ntfs_fs fs;
static struct ntfs_file file;
static struct ntfs_dirent pool[POOL];
static uint32_t pool_used;
static uint8_t chunk[1024 * 1024] __attribute__((aligned(32)));   /* cache-line aligned: zero-copy DMA */

static struct {
    uint32_t dirs, files, skipped, errors, unsupported, dropped;
    uint64_t bytes_listed, bytes_read;
    uint32_t read_ms;
} st;

static int disk_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return msc_read((struct usb_msc *)ctx, lba, count, buf);
}

/* 64-bit decimal without libgcc division */
static const char *u64dec(uint64_t v, char *buf)
{
    static const uint64_t p10[] = { 10000000000000000000ull, 1000000000000000000ull, 100000000000000000ull,
        10000000000000000ull, 1000000000000000ull, 100000000000000ull, 10000000000000ull, 1000000000000ull,
        100000000000ull, 10000000000ull, 1000000000ull, 100000000ull, 10000000ull, 1000000ull, 100000ull,
        10000ull, 1000ull, 100ull, 10ull, 1ull };
    char *o = buf;
    int started = 0;
    for (unsigned i = 0; i < sizeof p10 / sizeof p10[0]; i++) {
        char d = '0';
        while (v >= p10[i]) { v -= p10[i]; d++; }
        if (d != '0' || started || i + 1 == sizeof p10 / sizeof p10[0]) { *o++ = d; started = 1; }
    }
    *o = 0;
    return buf;
}

static int collect(const struct ntfs_dirent *e, void *arg)
{
    (void)arg;
    if (pool_used >= POOL) { st.dropped++; return 0; }
    pool[pool_used++] = *e;
    return 0;
}

static void crc_file(const struct ntfs_dirent *e, const char *path)
{
    char d1[24];
    int rc = ntfs_open(&fs, e->mft, &file);
    if (rc < 0) {
        printf("  %s: %s\n", path, ntfs_strerror(rc));
        if (rc == NTFS_E_COMPRESSED || rc == NTFS_E_ENCRYPTED) st.unsupported++;
        else st.errors++;
        return;
    }
    st.bytes_listed += file.size;
    if (st.bytes_read + file.size > CRC_BUDGET) {
        printf("FILE %10s  skipped   %s\n", u64dec(file.size, d1), path);
        st.skipped++;
        return;
    }
    uint32_t crc = 0, t0 = micros();
    uint64_t off = 0;
    while (off < file.size) {
        int n = ntfs_read(&fs, &file, off, chunk, sizeof chunk);
        if (n <= 0) {
            printf("  %s: read failed at %s: %s\n", path, u64dec(off, d1), ntfs_strerror(n));
            st.errors++;
            return;
        }
        crc = crc32_update(crc, chunk, (uint32_t)n);
        off += (uint32_t)n;
    }
    st.read_ms += elapsed_us(t0) / 1000u;
    st.bytes_read += file.size;
    st.files++;
    printf("FILE %10s  crc32 %08x  %s\n", u64dec(file.size, d1), crc, path);
}

static void walk(uint32_t dir, char *path, uint32_t plen, int depth)
{
    uint32_t first = pool_used;
    int rc = ntfs_list(&fs, dir, collect, 0);
    if (rc < 0) { printf("  %s/: %s\n", path, ntfs_strerror(rc)); st.errors++; }
    uint32_t last = pool_used;
    st.dirs++;
    for (uint32_t i = first; i < last; i++) {
        const struct ntfs_dirent *e = &pool[i];
        uint32_t nl = strlen(e->name);
        if (plen + 1 + nl >= 1024) { st.errors++; continue; }
        path[plen] = '/';
        memcpy(path + plen + 1, e->name, nl + 1);
        if (e->attr & NTFS_ATTR_DIRECTORY) {
            if ((e->attr & NTFS_ATTR_REPARSE) || depth + 1 >= MAX_DEPTH)
                printf("  %s/: not followed (%s)\n", path, (e->attr & NTFS_ATTR_REPARSE) ? "link" : "too deep");
            else
                walk(e->mft, path, plen + 1 + nl, depth + 1);
        } else {
            crc_file(e, path);
        }
        path[plen] = 0;
    }
    pool_used = first;                                    /* entries of this level no longer needed */
}

static void run(void)
{
    char d1[24], d2[24];
    int rc = ntfs_mount(&fs, disk_read, &msc);
    if (rc < 0) { printf("ntfs_mount: %s\n", ntfs_strerror(rc)); return; }
    printf("NTFS volume at sector %u: label \"%s\", serial %08x%08x, cluster %u bytes, MFT record %u bytes, "
           "%u MB, $MFT in %u fragment(s)\n", fs.part_lba, fs.label, fs.serial_hi, fs.serial_lo,
           1u << fs.cluster_shift, 1u << fs.rec_shift, fs.total_clusters >> (20 - fs.cluster_shift),
           fs.mft_nruns);

    static char path[1024];
    path[0] = 0;
    memset(&st, 0, sizeof st);
    uint32_t t0 = micros();
    walk(NTFS_ROOT, path, 0, 0);
    uint32_t ms = elapsed_us(t0) / 1000u;
    uint32_t kb = (uint32_t)(st.bytes_read >> 10), rms = st.read_ms ? st.read_ms : 1;
    printf("summary: %u directories, %u files read (%s bytes, %s bytes in all files seen), %u skipped, "
           "%u unsupported, %u errors, %u entries dropped\n", st.dirs, st.files, u64dec(st.bytes_read, d1),
           u64dec(st.bytes_listed, d2), st.skipped, st.unsupported, st.errors, st.dropped);
    printf("time: %u ms in all, file data at %u KB/s\n", ms, kb < 4000000u ? kb * 1000u / rms : kb / (rms / 1000u + 1));
    printf("transport: %u commands, %u retries, %u failed, %u resets, %u stalls, %u bulk errors\n",
           msc_stats.commands, msc_stats.retries, msc_stats.failed_status, msc_stats.reset_recoveries,
           msc_stats.stalls, msc_stats.bulk_errors);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb ntfs example (read only), running at %08x\n", (unsigned)&main);
    if (usb_host_init() < 0) { printf("usb: controller does not respond\n"); return 1; }
    usb_vbus(1);
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
            printf("drive: %s %s, %u sectors\n", msc.vendor, msc.product, msc.blocks);
            run();
            printf("done\n");
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("device removed\n");
        }
        delay_ms(20);
    }
}
