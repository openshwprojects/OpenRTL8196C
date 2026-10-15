/*
 * spiflash.h - the RTL8196C SPI flash controller (0xB8001200) and the
 * memory-mapped flash window at 0xBD000000.
 *
 * Reads go through the window (the loader has already configured the
 * controller for it).  Erase/program use the PIO path: SFCSR selects the
 * chip and the transfer length, SFDR carries the bytes in the high-order
 * byte lanes (big-endian order as configured by the loader).
 */
#ifndef SPIFLASH_H
#define SPIFLASH_H

#include <stdint.h>

#define SPIFLASH_MMAP       0xBD000000u
#define SPIFLASH_SIZE       0x800000u     /* MX25L6405: 8 MB */
#define SPIFLASH_SECTOR     4096u
#define SPIFLASH_BLOCK      65536u
#define SPIFLASH_PAGE       256u
#define SPIFLASH_PROTECTED_END 0x010000u  /* first 64 KB (loader + H601/6V03 settings) never written */

/* "8198-style" controller, which the RTL8196C has (boot code: SUPPORT_SPI_MIO_8198_8196C).
 * Measured: writing the CS idle value to +4 instead of +8 kills the memory window. */
#define SF_CR               0xB8001200u   /* clock divider, byte order, TCS */
#define SF_CR2              0xB8001204u   /* memory window: read command, size, dummy cycles (loader: 0x0bc08000) */
#define SF_CSR              0xB8001208u   /* control/status */
#define SF_DR               0xB800120Cu   /* data (PIO) */
#define SF_DR2              0xB8001210u   /* data for CMD_BYTE-style transfers */
#define   SF_CS(i)          ((uint32_t)(i) << 30)   /* CSB1:CSB0, 1 = deasserted: 1 = chip0 selected, 3 = none */
#define   SF_LEN(i)         ((uint32_t)(i) << 28)   /* bytes-1 per SFDR access, 0..3 */
#define   SF_READY          (1u << 27)

uint32_t spiflash_jedec_id(void);                     /* 0xC22017 for MX25L6405 */
uint8_t  spiflash_status(void);                       /* RDSR */
void     spiflash_read(uint32_t off, void *buf, uint32_t len);   /* via the memory window */
int      spiflash_erase_sector(uint32_t off);         /* 4 KB, off sector aligned */
int      spiflash_erase_block(uint32_t off);          /* 64 KB */
int      spiflash_erase_range(uint32_t off, uint32_t len);       /* 64 KB + 4 KB erases covering [off, off+len) */
int      spiflash_write(uint32_t off, const void *data, uint32_t len);  /* pages, erased area */
int      spiflash_verify(uint32_t off, const void *data, uint32_t len);
int      spiflash_factory_mac(uint8_t mac[6]);        /* read LAN MAC from H601 block at 0x6000, 0 ok / -1 fallback */

#endif
