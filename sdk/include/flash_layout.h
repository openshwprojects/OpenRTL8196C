/*
 * flash_layout.h - partition table, boot state, and RAPP/cs6c image headers
 * as specified in docs/04-flash.md.
 */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include <stdint.h>

/* Default flash offsets (8 MB MX25L6405) */
#define FLASH_LOADER_OFF      0x000000u
#define FLASH_LOADER_SIZE     0x010000u   /* 64 KB: factory loader + H601/6V03 settings */
#define FLASH_STAGE2_OFF      0x010000u
#define FLASH_STAGE2_SIZE     0x00E000u   /* 56 KB: stage-2 chooser with cs6c header */
#define FLASH_BOOTSTATE_OFF   0x01E000u
#define FLASH_BOOTSTATE_SIZE  0x001000u   /* 4 KB: append-only boot records */
#define FLASH_PARTTABLE_OFF   0x01F000u
#define FLASH_PARTTABLE_SIZE  0x001000u   /* 4 KB: partition table */
#define FLASH_SLOTA_OFF       0x020000u
#define FLASH_SLOTA_SIZE      0x200000u   /* 2 MB: app slot A */
#define FLASH_SLOTB_OFF       0x220000u
#define FLASH_SLOTB_SIZE      0x200000u   /* 2 MB: app slot B */
#define FLASH_LFS_OFF         0x420000u
#define FLASH_LFS_SIZE        0x3E0000u   /* 3.875 MB (992 * 4 KB): LittleFS */

/* Spare unused region in top of stock flash (0x5DB003..0x800000 is all 0x00 on stock)
 * for non-destructive hardware self-tests before committing stage-2 at 0x10000 */
#define FLASH_TEST_BASE       0x7E0000u   /* 128 KB at top of flash */

/* 1. Factory loader header ("cs6c" / "cr6c") */
struct cs6c_header {
    char     signature[4];   /* "cs6c" or "cr6c" */
    uint32_t start_addr;     /* RAM load + jump address */
    uint32_t burn_addr;      /* flash offset */
    uint32_t len;            /* payload + 2-byte big-endian zero-sum checksum */
};

/* 2. Partition table (0x01F000) */
#define PART_MAGIC   0x52505431u   /* 'RPT1' */
#define PART_VERSION 1u
#define PART_MAX     8u

struct part_entry {
    char     name[8];        /* "boot", "state", "appA", "appB", "lfs", ... */
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
};

struct part_table {
    uint32_t          magic;       /* PART_MAGIC */
    uint16_t          version;     /* PART_VERSION */
    uint16_t          count;
    struct part_entry e[PART_MAX];
    uint32_t          crc32;       /* CRC32 over bytes before crc32 */
};

/* 3. App image header (start of each slot) */
#define APP_MAGIC       0x52415050u   /* 'RAPP' */
#define APP_HDR_VERSION 1u

struct app_header {
    uint32_t magic;          /* APP_MAGIC ('RAPP') */
    uint32_t hdr_version;    /* APP_HDR_VERSION (1) */
    uint32_t seq;            /* higher = newer, set by OTA */
    uint32_t load_addr;      /* RAM destination for payload */
    uint32_t entry;          /* entry point to jump to */
    uint32_t length;         /* payload bytes immediately following this 56-byte header */
    uint32_t payload_crc32;  /* IEEE CRC32 of payload */
    char     version[16];    /* NUL-terminated string, e.g. git describe / build tag */
    uint32_t build_time;     /* unix timestamp */
    uint32_t header_crc32;   /* IEEE CRC32 of the first 52 bytes of app_header */
};

/* 4. Boot state (0x01E000): append-only 16-byte records in one 4 KB sector (256 records max) */
#define BOOT_REC_TRY     0x42545259u   /* 'BTRY': stage-2 is about to jump to (slot, seq) */
#define BOOT_REC_CONFIRM 0x42434E46u   /* 'BCNF': app confirmed healthy boot of (slot, seq) */
#define BOOT_MAX_TRIES   3u

struct boot_record {
    uint32_t magic;          /* BOOT_REC_TRY or BOOT_REC_CONFIRM */
    uint32_t slot;           /* 0 = appA, 1 = appB */
    uint32_t seq;            /* app_header.seq */
    uint32_t crc32;          /* IEEE CRC32 of first 12 bytes */
};

/* Partition table API */
void part_table_init_default(struct part_table *pt);
int  part_table_valid(const struct part_table *pt);
int  part_table_read(struct part_table *pt, uint32_t table_off);   /* 0 = from flash, 1 = fallback default used */
int  part_table_write(const struct part_table *pt, uint32_t table_off);
const struct part_entry *part_find(const struct part_table *pt, const char *name);

/* App image validation API */
uint32_t app_header_crc(const struct app_header *h);
int      app_header_valid(const struct app_header *h, uint32_t max_slot_size);
int      app_image_valid_mem(const void *img, uint32_t total_len, uint32_t max_slot_size);
int      app_slot_valid_flash(uint32_t slot_off, uint32_t slot_size, struct app_header *out_hdr);

/* Boot state API (state_off = FLASH_BOOTSTATE_OFF in production) */
struct boot_summary {
    int      last_slot;          /* last tried/confirmed slot (0 or 1, -1 if empty) */
    uint32_t last_seq;
    uint32_t unconfirmed_tries;  /* consecutive BTRY for (last_slot, last_seq) without BCNF */
    int      confirmed;          /* 1 if the latest attempt was confirmed */
    uint32_t records_used;       /* 0..256 */
};

void bootstate_scan(uint32_t state_off, struct boot_summary *sum);
int  bootstate_append(uint32_t state_off, uint32_t magic, uint32_t slot, uint32_t seq);
int  bootstate_record_try(uint32_t state_off, uint32_t slot, uint32_t seq);
int  bootstate_confirm(uint32_t state_off);

/* Stage-2 chooser logic: returns 0 (appA), 1 (appB), or -1 (neither valid) */
int  stage2_choose_slot(const struct part_table *pt, uint32_t state_off, struct app_header *chosen_hdr, uint32_t *chosen_off);

#endif /* FLASH_LAYOUT_H */
