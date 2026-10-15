/*
 * 96_spi_probe - read-only checks of the SPI controller driver: the window
 * must survive RDID / RDSR, and the ids must be real.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "spiflash.h"

static void show(const char *tag)
{
    printf("%-22s SFCR=%08x SFCR2=%08x SFCSR=%08x  win[0]=%08x win[10000]=%08x win[120000]=%08x\n", tag,
           REG32(SF_CR), REG32(SF_CR2), REG32(SF_CSR), REG32(SPIFLASH_MMAP), REG32(SPIFLASH_MMAP + 0x10000),
           REG32(SPIFLASH_MMAP + 0x120000));
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nspi probe v2 (8198-style registers)\n");
    show("A initial");
    for (int i = 0; i < 3; i++) {
        uint32_t id = spiflash_jedec_id();
        uint8_t sr = spiflash_status();
        printf("   RDID %06x (expect c22017)  RDSR %02x\n", id, sr);
        show("B after RDID+RDSR");
    }
    uint8_t buf[16];
    spiflash_read(0x10000, buf, 16);
    printf("   read(0x10000): %02x%02x%02x%02x %02x%02x%02x%02x (expect 63723663 80500000)\n",
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    printf("done\n");
    return 0;
}
