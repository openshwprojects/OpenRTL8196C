/*
 * spiflash.c - see spiflash.h.  Command sequences follow the boot code's
 * spi_flash.c for the 8196 controller (CS 1 = chip 0 selected, CS 3 = none).
 */
#include "rtl8196c.h"
#include "spiflash.h"
#include "libc.h"

#define CMD_WREN   0x06
#define CMD_RDSR   0x05
#define CMD_RDID   0x9F
#define CMD_PP     0x02
#define CMD_SE     0x20
#define CMD_BE     0xD8

static void wait_ready(void)
{
    while (!(REG32(SF_CSR) & SF_READY))
        ;
}

static void cs_none(void)
{
    REG32(SF_CSR) = SF_LEN(0) | SF_CS(3) | SF_READY;
    wait_ready();
}

/* The window's fast-read settings in SFCR2 are left alone; the vendor code
 * masks the dummy-cycle bits during PIO and restores them, we just save
 * and restore the whole register around every PIO operation. */
static uint32_t saved_cr2;
static void pio_begin(void) { saved_cr2 = REG32(SF_CR2); cs_none(); }
static void pio_end(void)   { cs_none(); REG32(SF_CR2) = saved_cr2; }

static void cmd1(uint8_t cmd)                       /* single-byte command, CS stays low after */
{
    REG32(SF_CSR) = SF_LEN(0) | SF_CS(1) | SF_READY;
    wait_ready();
    REG32(SF_DR) = (uint32_t)cmd << 24;
    wait_ready();
}

uint32_t spiflash_jedec_id(void)
{
    pio_begin();
    cmd1(CMD_RDID);                                  /* 1 byte out, CS stays low */
    REG32(SF_CSR) = SF_LEN(3) | SF_CS(1) | SF_READY; /* now clock 4 bytes in */
    wait_ready();
    uint32_t id = REG32(SF_DR) >> 8;                 /* manufacturer, type, capacity */
    pio_end();
    return id;
}

uint8_t spiflash_status(void)
{
    pio_begin();
    cmd1(CMD_RDSR);
    uint8_t s = (uint8_t)(REG32(SF_DR) >> 24);
    pio_end();
    return s;
}

static void wait_wip_clear(void)
{
    cmd1(CMD_RDSR);
    while (REG32(SF_DR) & 0x01000000u)
        ;
    cs_none();
}

static void write_enable(void)
{
    cmd1(CMD_WREN);
    cs_none();
}

void spiflash_read(uint32_t off, void *buf, uint32_t len)
{
    memcpy(buf, (const void *)(SPIFLASH_MMAP + off), len);
}

static int erase(uint8_t cmd, uint32_t off)
{
    if (off < SPIFLASH_PROTECTED_END || off >= SPIFLASH_SIZE) return -1;
    pio_begin();
    write_enable();
    REG32(SF_CSR) = SF_LEN(3) | SF_CS(1) | SF_READY;
    wait_ready();
    REG32(SF_DR) = ((uint32_t)cmd << 24) | (off & 0xFFFFFFu);
    wait_ready();
    cs_none();
    wait_wip_clear();
    pio_end();
    return 0;
}

int spiflash_erase_sector(uint32_t off) { return erase(CMD_SE, off & ~(SPIFLASH_SECTOR - 1)); }
int spiflash_erase_block(uint32_t off)  { return erase(CMD_BE, off & ~(SPIFLASH_BLOCK - 1)); }

int spiflash_erase_range(uint32_t off, uint32_t len)
{
    uint32_t start = off & ~(SPIFLASH_SECTOR - 1);
    uint32_t end = (off + len + SPIFLASH_SECTOR - 1) & ~(SPIFLASH_SECTOR - 1);
    if (start < SPIFLASH_PROTECTED_END || end > SPIFLASH_SIZE) return -1;
    while (start < end) {
        if ((start & (SPIFLASH_BLOCK - 1)) == 0 && (end - start) >= SPIFLASH_BLOCK) {
            if (spiflash_erase_block(start) < 0) return -1;
            start += SPIFLASH_BLOCK;
        } else {
            if (spiflash_erase_sector(start) < 0) return -1;
            start += SPIFLASH_SECTOR;
        }
    }
    return 0;
}

static void program_page(uint32_t off, const uint8_t *d, uint32_t len)
{
    write_enable();
    REG32(SF_CSR) = SF_LEN(3) | SF_CS(1) | SF_READY;
    wait_ready();
    REG32(SF_DR) = ((uint32_t)CMD_PP << 24) | (off & 0xFFFFFFu);
    wait_ready();
    while (len) {
        uint32_t n = len >= 4 ? 4 : len;
        uint32_t w = 0;
        for (uint32_t i = 0; i < n; i++) w |= (uint32_t)d[i] << (24 - 8 * i);
        REG32(SF_CSR) = SF_LEN(n - 1) | SF_CS(1) | SF_READY;
        wait_ready();
        REG32(SF_DR) = w;
        wait_ready();
        d += n; len -= n;
    }
    cs_none();
    wait_wip_clear();
}

static int is_all_ff(const uint8_t *d, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (d[i] != 0xFFu)
            return 0;
    return 1;
}

int spiflash_write(uint32_t off, const void *data, uint32_t len)
{
    const uint8_t *d = data;
    if (off < SPIFLASH_PROTECTED_END || off + len > SPIFLASH_SIZE) return -1;
    pio_begin();
    while (len) {
        uint32_t n = SPIFLASH_PAGE - (off & (SPIFLASH_PAGE - 1));
        if (n > len) n = len;
        if (!is_all_ff(d, n))
            program_page(off, d, n);
        off += n; d += n; len -= n;
    }
    pio_end();
    return 0;
}

int spiflash_verify(uint32_t off, const void *data, uint32_t len)
{
    return memcmp((const void *)(SPIFLASH_MMAP + off), data, len) == 0 ? 0 : -1;
}

int spiflash_factory_mac(uint8_t mac[6])
{
    const uint8_t *hw = (const uint8_t *)(SPIFLASH_MMAP + 0x6000u);
    if (hw[0] == 'H' && hw[1] == '6' && hw[2] == '0' && hw[3] == '1') {
        const uint8_t *m = hw + 7; /* 0x6007: LAN MAC (e.g. 80:1f:02:ca:ad:92) */
        if ((m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0x00 &&
            (m[0] & m[1] & m[2] & m[3] & m[4] & m[5]) != 0xFF &&
            !(m[0] & 0x01)) {
            memcpy(mac, m, 6);
            return 0;
        }
    }
    static const uint8_t fallback[6] = { 0x02, 0x19, 0x6c, 0x00, 0x00, 0x01 };
    memcpy(mac, fallback, 6);
    return -1;
}
