/*
 * 84_usb_write - which bulk OUT path sends the right data?
 * Works on 64 sectors near the end of the disk (blocks - 100000): saves
 * them, writes test patterns through every buffer path, reads back through
 * the bounce buffer, then restores and verifies the original contents.
 * Build cached: --load 0x80500000 --stack 0x80F00000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "libc.h"
#include "irq.h"

#define SECTS 64u

static struct usb_msc msc;
static uint32_t lba;
static uint8_t orig[SECTS * 512] __attribute__((aligned(32)));
static uint8_t wbuf[SECTS * 512] __attribute__((aligned(4096)));
static uint8_t rbuf[SECTS * 512] __attribute__((aligned(4096)));
static uint32_t fails;

static uint8_t pat(uint32_t i, uint32_t seed)
{
    uint32_t v = seed ^ ((i & ~3u) * 0x01000193u);
    return (uint8_t)(v >> (8 * (3 - (i & 3))));
}

static void test(const char *what, int zc, uint8_t *src, uint32_t sectors, uint32_t seed)
{
    uint32_t n = sectors * 512;
    for (uint32_t i = 0; i < n; i++) src[i] = pat(i, seed);
    usb_zero_copy = zc;
    int w = msc_write(&msc, lba, sectors, src);
    usb_zero_copy = 0;
    memset(rbuf, 0, n);
    int r = msc_read(&msc, lba, sectors, rbuf);
    uint32_t bad = 0, ff = 0, first = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        if (rbuf[i] != pat(i, seed)) { bad++; if (first == 0xFFFFFFFFu) first = i; }
        ff += rbuf[i] == 0xFF;
    }
    if (bad || w || r) fails++;
    printf("  %-32s write %d read %d: %u of %u bytes differ (%u FF)", what, w, r, bad, n, ff);
    if (bad) printf(", first at %u", first);
    printf("\n");
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb write probe (save / test / restore)\n");
    if (usb_host_init() < 0) return 1;
    usb_vbus(1);
    for (;;) {
        if (usb_poll() != USB_EV_CONNECTED) { delay_ms(20); continue; }
        if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
        fails = 0;
        lba = msc.blocks - 100000u;
        printf("drive %s %s, %u blocks; test area LBA %u..%u\n", msc.vendor, msc.product, msc.blocks, lba,
               lba + SECTS - 1);
        printf("write protect: %d\n", msc_write_protected(&msc));
        usb_zero_copy = 0;
        if (msc_read(&msc, lba, SECTS, orig) < 0) { printf("cannot read the test area, not writing\n"); continue; }
        uint32_t check = crc32(orig, sizeof orig);
        printf("original saved, crc %08x\n", check);

        test("bounce, 1 sector", 0, wbuf, 1, 0x11111111u);
        test("bounce, 8 sectors", 0, wbuf, 8, 0x22222222u);
        test("bounce, 64 sectors", 0, wbuf, 64, 0x33333333u);
        test("direct cached, 8 sectors", 1, wbuf, 8, 0x44444444u);
        test("direct uncached src, 8 sectors", 1,
             (uint8_t *)(((uint32_t)wbuf & 0x1FFFFFFFu) | 0xA0000000u), 8, 0x55555555u);
        test("direct cached, 64 sectors", 1, wbuf, 64, 0x66666666u);

        /* restore and verify */
        usb_zero_copy = 1;
        int w = msc_write(&msc, lba, SECTS, orig);
        int s = msc_sync(&msc);
        usb_zero_copy = 0;
        memset(rbuf, 0, sizeof rbuf);
        int r = msc_read(&msc, lba, SECTS, rbuf);
        uint32_t back = crc32(rbuf, sizeof rbuf);
        printf("restore: write %d sync %d read %d, crc %08x -> %s\n", w, s, r, back,
               back == check ? "original restored" : "RESTORE FAILED");
        if (back != check) fails++;
        printf("transport: %u commands, %u retries, %u failed, %u resets, %u stalls, %u bulk errors, "
               "%u with residue (last %u)\n", msc_stats.commands, msc_stats.retries, msc_stats.failed_status,
               msc_stats.reset_recoveries, msc_stats.stalls, msc_stats.bulk_errors, msc_stats.residue_cmds,
               msc_stats.last_residue);
        printf("RESULT: %s\n", fails ? "FAILED" : "ALL OK");
    }
}
