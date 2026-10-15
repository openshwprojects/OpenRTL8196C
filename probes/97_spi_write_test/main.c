/*
 * 97_spi_write_test - FIRST FLASH WRITE.  Uses only the last 4 KB sector
 * (0x7FF000), which the factory left filled with 0x00 and nothing uses.
 *
 *   erase -> expect 0xFF, program a pattern -> verify, erase -> 0xFF,
 *   finally program the original zeros back so the chip looks as before.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "spiflash.h"
#include "libc.h"

#define TEST_OFF 0x7FF000u

static uint8_t buf[SPIFLASH_SECTOR];

static int all(uint8_t v)
{
    spiflash_read(TEST_OFF, buf, sizeof buf);
    for (uint32_t i = 0; i < sizeof buf; i++) if (buf[i] != v) return 0;
    return 1;
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nspi write test on sector %08x\n", TEST_OFF);
    printf("jedec %06x status %02x, sector all-zero before: %d\n", spiflash_jedec_id(), spiflash_status(), all(0x00));

    uint32_t t0 = micros();
    spiflash_erase_sector(TEST_OFF);
    printf("erase: %u us, all 0xFF: %d, window[0x10000]=%08x\n", elapsed_us(t0), all(0xFF), REG32(SPIFLASH_MMAP + 0x10000));

    for (uint32_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7 + 3);
    t0 = micros();
    spiflash_write(TEST_OFF, buf, sizeof buf);
    uint32_t t_write = elapsed_us(t0);
    int ok = spiflash_verify(TEST_OFF, buf, sizeof buf) == 0;
    printf("program 4 KB: %u us, verify %s\n", t_write, ok ? "OK" : "FAILED");
    if (!ok) {
        uint8_t rb[16]; spiflash_read(TEST_OFF, rb, 16);
        printf("  first bytes: %02x %02x %02x %02x %02x %02x %02x %02x (expect 03 0a 11 18 1f 26 2d 34)\n",
               rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
    }

    spiflash_erase_sector(TEST_OFF);
    printf("erase again: all 0xFF: %d\n", all(0xFF));
    memset(buf, 0, sizeof buf);
    spiflash_write(TEST_OFF, buf, sizeof buf);
    printf("restored zeros: %d\n", all(0x00));
    printf("window check: [0]=%08x [10000]=%08x [120000]=%08x\n", REG32(SPIFLASH_MMAP),
           REG32(SPIFLASH_MMAP + 0x10000), REG32(SPIFLASH_MMAP + 0x120000));
    printf("done\n");
    return 0;
}
