/*
 * ota.h - Over-The-Air (Ethernet TCP) firmware update & RAM execution service
 *
 * Protocol on TCP port 9698 (BOARD_OTA_PORT):
 *   16-byte big-endian header:
 *     char     magic[4];    // "ROTA"
 *     uint32_t mode;        // OTA_CMD_*
 *     uint32_t length;      // bytes following this 16-byte header
 *     uint32_t target_addr; // RAM load addr, flash offset, or flags
 *   Followed by `length` bytes of image data, then board replies with a
 *   single line starting with "OK " or "ERR ".
 */
#ifndef OTA_H
#define OTA_H

#include <stdint.h>

#define OTA_STAGE_ADDR  0xA0A00000u   /* 10 MB mark in 32 MB SDRAM (uncached) */
#define OTA_MAX_SIZE    0x00200000u   /* 2 MB max image */
#define OTA_RELOC_ADDR  0xA0400000u   /* safe scratch address for RAM jump relocator */

enum {
    OTA_CMD_STATUS     = 0,  /* query partition/slot/bootstate status */
    OTA_CMD_RAM        = 1,  /* stage in RAM, verify, relocate to target_addr & jump */
    OTA_CMD_FLASH      = 2,  /* validate RAPP, write to inactive slot A/B, bump seq (target_addr & 1 = reboot) */
    OTA_CMD_TEST_FLASH = 3,  /* validate RAPP, write to spare top flash (0x7E0000), verify, restore zeros */
    OTA_CMD_RAW_FLASH  = 4,  /* erase & write raw bytes at target_addr (>= 0x10000), verify */
};

int  ota_init(uint16_t port);
void ota_poll(void);

/* Low-level helpers usable by stage-2, HTTP OTA, or tests */
void ota_ram_jump(const void *src, uint32_t dst_addr, uint32_t len, uint32_t entry_addr);
int  ota_flash_rapp_slot(const void *img, uint32_t total_len,
                         uint32_t slot_off, uint32_t slot_size, uint32_t new_seq);
int  ota_flash_inactive_slot(const void *img, uint32_t total_len,
                             int *out_slot, uint32_t *out_off, uint32_t *out_seq);
int  ota_boot_active_slot(int verbose);

extern uint32_t ota_sessions, ota_bytes_rcvd;

#endif /* OTA_H */

