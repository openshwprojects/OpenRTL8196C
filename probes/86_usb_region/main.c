/*
 * 86_usb_region - is an unstable region on a stick the stick or the driver?
 * Reads one megabyte (REGION_MB) repeatedly, once with 1-sector commands
 * and once with 64 KB commands per round, and reports per sector how many
 * bytes and bits differ from the first read.  Prints counts only, no data.
 * Build cached: --load 0x80500000 --stack 0x80600000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "libc.h"

#define REGION_MB 453u
#define SECTS     2048u
#define ROUNDS    6

static struct usb_msc msc;
static uint8_t ref[SECTS * 512], cur[SECTS * 512];
static uint32_t sect_changes[SECTS];

static int popcount8(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static void compare(const char *how, int round)
{
    uint32_t bytes = 0, bits = 0, sects = 0, ones_to_zero = 0, zero_to_one = 0;
    for (uint32_t s = 0; s < SECTS; s++) {
        uint32_t sb = 0;
        for (uint32_t i = 0; i < 512; i++) {
            uint8_t a = ref[s * 512 + i], b = cur[s * 512 + i];
            if (a != b) {
                sb++;
                bits += popcount8(a ^ b);
                ones_to_zero += popcount8(a & ~b);
                zero_to_one += popcount8(~a & b);
            }
        }
        if (sb) { sects++; sect_changes[s]++; }
        bytes += sb;
    }
    printf("round %d (%s): %u sectors differ, %u bytes, %u bits (1->0 %u, 0->1 %u)\n", round, how, sects, bytes,
           bits, ones_to_zero, zero_to_one);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb region probe: MB %u\n", REGION_MB);
    if (usb_host_init() < 0) return 1;
    usb_vbus(1);
    for (;;) {
        if (usb_poll() != USB_EV_CONNECTED) { delay_ms(20); continue; }
        if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
        uint32_t lba = REGION_MB * SECTS;
        if (msc_read(&msc, lba, SECTS, ref) < 0) { printf("first read failed\n"); continue; }
        uint32_t zeros = 0, ffs = 0;
        for (uint32_t s = 0; s < SECTS; s++) {
            int z = 1, f = 1;
            for (uint32_t i = 0; i < 512; i++) { if (ref[s * 512 + i]) z = 0; if (ref[s * 512 + i] != 0xFF) f = 0; }
            zeros += z; ffs += f;
        }
        printf("first read: %u sectors all 0x00, %u all 0xFF, %u other\n", zeros, ffs, SECTS - zeros - ffs);
        for (int r = 1; r <= ROUNDS; r++) {
            int big = r & 1;
            if (big) {
                if (msc_read(&msc, lba, SECTS, cur) < 0) { printf("read failed\n"); break; }
            } else {
                for (uint32_t s = 0; s < SECTS; s++)
                    if (msc_read(&msc, lba + s, 1, cur + s * 512) < 0) { printf("read failed at %u\n", s); break; }
            }
            compare(big ? "64 KB commands" : "1-sector commands", r);
        }
        uint32_t unstable = 0, first = 0xFFFFFFFFu, last = 0;
        for (uint32_t s = 0; s < SECTS; s++)
            if (sect_changes[s]) { unstable++; if (s < first) first = s; last = s; }
        if (unstable)
            printf("unstable sectors: %u, from sector %u to %u of the MB (LBA %u..%u)\n", unstable, first, last,
                   lba + first, lba + last);
        else
            printf("no unstable sectors this time\n");
        printf("transport: %u commands, %u retries, %u failed, %u resets, %u stalls, %u bulk errors\n",
               msc_stats.commands, msc_stats.retries, msc_stats.failed_status, msc_stats.reset_recoveries,
               msc_stats.stalls, msc_stats.bulk_errors);
        printf("done\n");
    }
}
