/*
 * 21_usb_fatfs - full read/write file system on a USB stick (FatFs).
 *
 * WRITES TO THE STICK.  Use a stick whose contents you can lose.
 *  1. mount, volume info, recursive listing (UTF-8 long names)
 *  2. raw sector test: save 64 KB, write a pattern, verify, restore, verify
 *  3. file test in /rtltest: 1 MB pattern file written, synced, read back
 *     and compared; random-offset reads; append; rename; hello.txt
 *  4. throughput of FatFs writes and reads
 * The files stay on the stick so they can be checked on a PC with
 *   python tools/fat_crc.py E:\
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "ff.h"
#include "libc.h"

#define TEST_LBA   (msc.blocks - 100000u)  /* raw test area near the end, restored afterwards */
#define TEST_SECT  128u             /* 64 KB */
#define FILE_SIZE  (1024u * 1024u)

static struct usb_msc msc;
static FATFS vol;
/* cache-line aligned buffers go straight to the USB controller (zero copy) */
static uint8_t orig[TEST_SECT * 512] __attribute__((aligned(32)));
static uint8_t pat[TEST_SECT * 512] __attribute__((aligned(32)));
static uint8_t back[TEST_SECT * 512] __attribute__((aligned(32)));
static uint8_t io[32 * 1024] __attribute__((aligned(32)));
static uint32_t fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* reproducible pattern: byte i of the file */
static uint8_t pattern_byte(uint32_t i)
{
    uint32_t x = (i >> 2) * 2654435761u + 0x9E3779B9u;
    return (uint8_t)(x >> (8 * (i & 3)));
}

static void list_dir(const char *path, int depth)
{
    DIR d;
    FILINFO fi;
    if (depth > 4 || f_opendir(&d, path) != FR_OK) return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("  %s %10lu  %s\n", (fi.fattrib & AM_DIR) ? "<DIR>" : "     ", (unsigned long)fi.fsize, fi.fname);
        if (fi.fattrib & AM_DIR) {
            char sub[256];
            snprintf(sub, sizeof sub, "%s/%s", path, fi.fname);
            list_dir(sub, depth + 1);
        }
    }
    f_closedir(&d);
}

static void raw_test(void)
{
    printf("raw sector test at LBA %u (%u sectors), original data restored afterwards\n", TEST_LBA, TEST_SECT);
    for (uint32_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 31 + (i >> 9));
    CHECK(msc_read(&msc, TEST_LBA, TEST_SECT, orig) == 0, "read original");
    uint32_t t0 = micros();
    int w = msc_write(&msc, TEST_LBA, TEST_SECT, pat);
    uint32_t wus = elapsed_us(t0);
    CHECK(w == 0, "write pattern rc %d", w);
    memset(back, 0, sizeof back);
    CHECK(msc_read(&msc, TEST_LBA, TEST_SECT, back) == 0, "read back pattern");
    CHECK(memcmp(back, pat, sizeof pat) == 0, "pattern differs after write");
    CHECK(msc_write(&msc, TEST_LBA, TEST_SECT, orig) == 0, "restore original");
    memset(back, 0, sizeof back);
    CHECK(msc_read(&msc, TEST_LBA, TEST_SECT, back) == 0, "read back original");
    CHECK(memcmp(back, orig, sizeof orig) == 0, "original differs after restore");
    CHECK(msc_sync(&msc) == 0, "SYNCHRONIZE CACHE");
    printf("  raw write of 64 KB took %u ms; pattern verified and original restored: %s\n", wus / 1000u,
           fails ? "FAILED" : "OK");
}

static void file_test(void)
{
    FIL f;
    UINT n;
    FRESULT r;
    printf("file test in /rtltest\n");
    r = f_mkdir("/rtltest");
    CHECK(r == FR_OK || r == FR_EXIST, "mkdir %d", r);

    /* write 1 MB in 32 KB pieces */
    uint32_t t0 = micros();
    r = f_open(&f, "/rtltest/pattern.bin", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK(r == FR_OK, "open for write %d", r);
    uint32_t crc = 0;
    for (uint32_t off = 0; off < FILE_SIZE && r == FR_OK; off += sizeof io) {
        for (uint32_t i = 0; i < sizeof io; i++) io[i] = pattern_byte(off + i);
        crc = crc32_update(crc, io, sizeof io);
        r = f_write(&f, io, sizeof io, &n);
        CHECK(r == FR_OK && n == sizeof io, "write at %u: %d (%u bytes)", off, r, n);
    }
    r = f_close(&f);
    CHECK(r == FR_OK, "close after write %d", r);
    uint32_t wms = elapsed_us(t0) / 1000u;

    /* read back and compare */
    t0 = micros();
    r = f_open(&f, "/rtltest/pattern.bin", FA_READ);
    CHECK(r == FR_OK, "open for read %d", r);
    CHECK(f_size(&f) == FILE_SIZE, "size %lu", (unsigned long)f_size(&f));
    uint32_t bad = 0;
    for (uint32_t off = 0; off < FILE_SIZE && r == FR_OK; off += sizeof io) {
        r = f_read(&f, io, sizeof io, &n);
        for (uint32_t i = 0; i < n; i++) if (io[i] != pattern_byte(off + i)) bad++;
    }
    uint32_t rms = elapsed_us(t0) / 1000u;
    CHECK(bad == 0, "%u bytes differ on read back", bad);

    /* random-offset reads */
    uint32_t seed = 12345, rbad = 0;
    for (int k = 0; k < 200; k++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t off = (seed >> 8) % (FILE_SIZE - 600), len = 1 + (seed % 599);
        f_lseek(&f, off);
        f_read(&f, io, len, &n);
        for (uint32_t i = 0; i < n; i++) if (io[i] != pattern_byte(off + i)) rbad++;
        if (n != len) rbad++;
    }
    CHECK(rbad == 0, "random reads: %u bad bytes", rbad);
    f_close(&f);
    printf("  1 MB written in %u ms (%u KB/s), read and compared in %u ms (%u KB/s), 200 random reads %s, crc32 %08x\n",
           wms, wms ? 1024000u / wms : 0, rms, rms ? 1024000u / rms : 0, rbad ? "BAD" : "OK", crc);

    /* append + rename + stat */
    r = f_open(&f, "/rtltest/hello.txt", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK(r == FR_OK, "open hello %d", r);
    f_printf(&f, "Hello from the RTL8196C router (OpenRTL8196C, FatFs)\r\n");
    f_close(&f);
    r = f_open(&f, "/rtltest/hello.txt", FA_OPEN_APPEND | FA_WRITE);
    f_printf(&f, "second line, appended - zażółć gęślą jaźń\r\n");
    f_close(&f);
    f_unlink("/rtltest/renamed.txt");
    r = f_rename("/rtltest/hello.txt", "/rtltest/renamed.txt");
    CHECK(r == FR_OK, "rename %d", r);
    r = f_rename("/rtltest/renamed.txt", "/rtltest/hello.txt");
    CHECK(r == FR_OK, "rename back %d", r);
    FILINFO fi;
    r = f_stat("/rtltest/hello.txt", &fi);
    CHECK(r == FR_OK && fi.fsize > 60, "stat %d size %lu", r, (unsigned long)fi.fsize);
    char line[128];
    r = f_open(&f, "/rtltest/hello.txt", FA_READ);
    printf("  hello.txt (%lu bytes):\n", (unsigned long)fi.fsize);
    while (f_gets(line, sizeof line, &f)) printf("    %s", line);
    f_close(&f);
}

static void run(void)
{
    FRESULT r = f_mount(&vol, "0:", 1);
    if (r != FR_OK) { printf("f_mount failed %d\n", r); return; }
    static const char *const types[] = { "?", "FAT12", "FAT16", "FAT32", "exFAT" };
    char label[24] = "";
    DWORD serial = 0, free_clust = 0;
    FATFS *fsp;
    f_getlabel("0:", label, &serial);
    f_getfree("0:", &free_clust, &fsp);
    printf("volume: %s, label \"%s\", serial %08lx, cluster %u KB, %lu MB free of %lu MB\n",
           types[vol.fs_type <= 4 ? vol.fs_type : 0], label, (unsigned long)serial, vol.csize / 2,
           (unsigned long)(free_clust * vol.csize / 2048), (unsigned long)((vol.n_fatent - 2) * vol.csize / 2048));
    printf("contents before the test:\n");
    list_dir("", 0);

    raw_test();
    file_test();

    printf("contents after the test:\n");
    list_dir("", 0);
    f_mount(0, "0:", 0);
    msc_sync(&msc);
    printf("transport: %u commands, %u retries, %u failed status, %u resets, %u stalls, %u bulk errors, "
           "%u with residue\n", msc_stats.commands, msc_stats.retries, msc_stats.failed_status,
           msc_stats.reset_recoveries, msc_stats.stalls, msc_stats.bulk_errors, msc_stats.residue_cmds);
    printf("RESULT: %s (%u failures)\n", fails ? "FAILED" : "ALL OK", fails);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb fatfs read/write example, running at %08x\n", (unsigned)&main);
    if (usb_host_init() < 0) { printf("usb: controller does not respond\n"); return 1; }
    usb_vbus(1);
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            usb_print_dev(usb_dev());
            if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
            diskio_usb_attach(&msc);
            printf("drive: %s %s, %u sectors\n", msc.vendor, msc.product, msc.blocks);
            run();
            printf("done\n");
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("device removed\n");
        }
        delay_ms(20);
    }
}
