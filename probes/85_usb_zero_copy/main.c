/*
 * 85_usb_zero_copy - bulk IN straight into caller buffers on a write-back
 * D-cache.  The reference read goes through the bounce buffer; the direct
 * reads land in buffers pre-filled with dirty 0xEE lines, so a missing
 * write-back/invalidate shows up as a mismatch.
 * Build cached: --load 0x80500000 --stack 0x80F00000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "libc.h"

static struct usb_msc msc;
static uint8_t ref[64 * 1024] __attribute__((aligned(32)));
static uint8_t pages[2][64 * 1024 + 4096] __attribute__((aligned(4096)));

static void try_read(const char *what, uint8_t *dst, uint32_t sectors)
{
    memset(dst, 0xEE, sectors * 512);                  /* dirty cached lines over the target */
    int rc = msc_read(&msc, 1000, sectors, dst);
    printf("  %-28s %08x %3u sectors: rc %d, %s\n", what, (unsigned)dst, sectors, rc,
           memcmp(dst, ref, sectors * 512) == 0 ? "matches" : "DIFFERS");
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nzero copy probe\n");
    if (usb_host_init() < 0) return 1;
    usb_vbus(1);
    for (;;) {
        if (usb_poll() != USB_EV_CONNECTED) { delay_ms(20); continue; }
        if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
        usb_zero_copy = 0;
        int rc = msc_read(&msc, 1000, 128, ref);
        printf("reference (bounce buffer) rc %d, crc %08x\n", rc, crc32(ref, sizeof ref));
        usb_zero_copy = 1;
        uint8_t *u = (uint8_t *)(((uint32_t)pages[0] & 0x1FFFFFFFu) | 0xA0000000u);
        try_read("uncached, page-aligned", u, 8);
        try_read("cached, page-aligned", pages[1], 8);
        try_read("cached, offset 0x800", pages[1] + 0x800, 8);
        try_read("cached, offset 0x020", pages[1] + 0x20, 128);
        try_read("cached, offset 0x004 (bounce)", pages[1] + 4, 8);
        try_read("cached, 64 KB", pages[0], 128);
        int bad = 0;
        uint32_t t0 = micros();
        for (int i = 0; i < 50; i++) {
            memset(pages[i & 1], 0xEE, sizeof ref);
            if (msc_read(&msc, 1000, 128, pages[i & 1]) < 0 || memcmp(pages[i & 1], ref, sizeof ref)) bad++;
        }
        printf("  50 x 64 KB direct reads over dirty lines: %d bad, %u ms incl. memset+compare\n", bad,
               elapsed_us(t0) / 1000u);
        printf("transport: %u commands, %u retries, %u bulk errors\n", msc_stats.commands, msc_stats.retries,
               msc_stats.bulk_errors);
        printf("done\n");
    }
}
