/*
 * 83_disk_layout - read-only: partition table (MBR or GPT) and the file
 * system signature of each partition.  Prints structure only, no file data.
 * Build cached: --load 0x80500000 --stack 0x80F00000
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"
#include "usb_msc.h"
#include "libc.h"

static struct usb_msc msc;
static uint8_t s[512] __attribute__((aligned(32)));

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void fs_sig(uint32_t lba)
{
    if (msc_read(&msc, lba, 1, s) < 0) { printf("      read failed\n"); return; }
    char oem[9];
    for (int i = 0; i < 8; i++) oem[i] = (s[3 + i] >= 32 && s[3 + i] < 127) ? (char)s[3 + i] : '.';
    oem[8] = 0;
    const char *t = "unknown";
    if (!memcmp(s + 3, "EXFAT   ", 8)) t = "exFAT";
    else if (!memcmp(s + 3, "NTFS    ", 8)) t = "NTFS";
    else if (!memcmp(s + 82, "FAT32   ", 8)) t = "FAT32";
    else if (!memcmp(s + 54, "FAT16   ", 8) || !memcmp(s + 54, "FAT12   ", 8)) t = "FAT12/16";
    else if (s[0x438 - 0x400 + 0] == 0x53 && 0) t = "ext";
    printf("      LBA %u: OEM \"%s\", signature %02x%02x -> %s\n", lba, oem, s[510], s[511], t);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\ndisk layout probe (read only)\n");
    if (usb_host_init() < 0) return 1;
    usb_vbus(1);
    for (;;) {
        if (usb_poll() != USB_EV_CONNECTED) { delay_ms(20); continue; }
        if (msc_init(&msc) < 0) { printf("msc_init failed\n"); continue; }
        printf("drive %s %s, %u blocks\n", msc.vendor, msc.product, msc.blocks);
        if (msc_read(&msc, 0, 1, s) < 0) { printf("LBA 0 read failed\n"); continue; }
        printf("LBA 0: signature %02x%02x\n", s[510], s[511]);
        fs_sig(0);
        msc_read(&msc, 0, 1, s);
        int gpt = 0;
        uint32_t parts[4] = { 0 };
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s + 446 + 16 * i;
            if (!e[4]) continue;
            printf("  MBR entry %d: type %02x, start %u, %u sectors\n", i, e[4], le32(e + 8), le32(e + 12));
            if (e[4] == 0xEE) gpt = 1;
            parts[i] = le32(e + 8);
        }
        if (gpt) {
            msc_read(&msc, 1, 1, s);
            uint32_t ent_lba = le32(s + 72), n = le32(s + 80), sz = le32(s + 84);
            printf("  GPT: \"%.8s\", %u entries of %u bytes at LBA %u\n", (char *)s, n, sz, ent_lba);
            static uint8_t ent[512 * 4] __attribute__((aligned(32)));
            msc_read(&msc, ent_lba, 4, ent);
            for (uint32_t i = 0; i < 16 && i * sz < sizeof ent; i++) {
                const uint8_t *e = ent + i * sz;
                uint32_t first = le32(e + 32);
                if (!first) continue;
                printf("  GPT partition %u: LBA %u..%u\n", i, first, le32(e + 40));
                fs_sig(first);
            }
        } else {
            for (int i = 0; i < 4; i++) if (parts[i]) { printf("    partition %d:\n", i); fs_sig(parts[i]); }
        }
        printf("done\n");
    }
}
