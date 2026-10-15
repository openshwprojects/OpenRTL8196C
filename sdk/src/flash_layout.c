/*
 * flash_layout.c - partition table, boot state, RAPP validation, stage-2 chooser
 */
#include "rtl8196c.h"
#include "flash_layout.h"
#include "spiflash.h"
#include "libc.h"

void part_table_init_default(struct part_table *pt)
{
    memset(pt, 0, sizeof *pt);
    pt->magic = PART_MAGIC;
    pt->version = PART_VERSION;
    pt->count = 6;

    strcpy(pt->e[0].name, "boot");
    pt->e[0].offset = FLASH_STAGE2_OFF;
    pt->e[0].size   = FLASH_STAGE2_SIZE;

    strcpy(pt->e[1].name, "state");
    pt->e[1].offset = FLASH_BOOTSTATE_OFF;
    pt->e[1].size   = FLASH_BOOTSTATE_SIZE;

    strcpy(pt->e[2].name, "ptable");
    pt->e[2].offset = FLASH_PARTTABLE_OFF;
    pt->e[2].size   = FLASH_PARTTABLE_SIZE;

    strcpy(pt->e[3].name, "appA");
    pt->e[3].offset = FLASH_SLOTA_OFF;
    pt->e[3].size   = FLASH_SLOTA_SIZE;

    strcpy(pt->e[4].name, "appB");
    pt->e[4].offset = FLASH_SLOTB_OFF;
    pt->e[4].size   = FLASH_SLOTB_SIZE;

    strcpy(pt->e[5].name, "lfs");
    pt->e[5].offset = FLASH_LFS_OFF;
    pt->e[5].size   = FLASH_LFS_SIZE;

    pt->crc32 = crc32(pt, sizeof(*pt) - sizeof(uint32_t));
}

int part_table_valid(const struct part_table *pt)
{
    if (pt->magic != PART_MAGIC || pt->version != PART_VERSION || pt->count == 0 || pt->count > PART_MAX)
        return 0;
    uint32_t c = crc32(pt, sizeof(*pt) - sizeof(uint32_t));
    return c == pt->crc32;
}

int part_table_read(struct part_table *pt, uint32_t table_off)
{
    spiflash_read(table_off, pt, sizeof *pt);
    if (part_table_valid(pt))
        return 0;
    part_table_init_default(pt);
    return 1;
}

int part_table_write(const struct part_table *pt, uint32_t table_off)
{
    struct part_table copy = *pt;
    copy.crc32 = crc32(&copy, sizeof(copy) - sizeof(uint32_t));
    if (spiflash_erase_sector(table_off) < 0)
        return -1;
    if (spiflash_write(table_off, &copy, sizeof copy) < 0)
        return -1;
    return spiflash_verify(table_off, &copy, sizeof copy);
}

const struct part_entry *part_find(const struct part_table *pt, const char *name)
{
    for (uint16_t i = 0; i < pt->count && i < PART_MAX; i++)
        if (strncmp(pt->e[i].name, name, sizeof pt->e[i].name) == 0)
            return &pt->e[i];
    return 0;
}

uint32_t app_header_crc(const struct app_header *h)
{
    return crc32(h, sizeof(*h) - sizeof(uint32_t));
}

int app_header_valid(const struct app_header *h, uint32_t max_slot_size)
{
    if (h->magic != APP_MAGIC || h->hdr_version != APP_HDR_VERSION)
        return 0;
    if (h->length == 0 || sizeof(*h) + h->length > max_slot_size)
        return 0;
    /* load_addr must be in KSEG0 (0x80000000..0x81FFFFFF) or KSEG1 (0xA0000000..0xA1FFFFFF) */
    uint32_t phys = h->load_addr & 0x1FFFFFFFu;
    uint32_t seg  = h->load_addr & 0xE0000000u;
    if ((seg != 0x80000000u && seg != 0xA0000000u) || phys < 0x00001000u || phys + h->length > 0x02000000u)
        return 0;
    return app_header_crc(h) == h->header_crc32;
}

int app_image_valid_mem(const void *img, uint32_t total_len, uint32_t max_slot_size)
{
    if (total_len < sizeof(struct app_header))
        return 0;
    struct app_header h;
    memcpy(&h, img, sizeof h);
    if (!app_header_valid(&h, max_slot_size))
        return 0;
    if (total_len < sizeof(h) + h.length)
        return 0;
    const uint8_t *payload = (const uint8_t *)img + sizeof(h);
    return crc32(payload, h.length) == h.payload_crc32;
}

int app_slot_valid_flash(uint32_t slot_off, uint32_t slot_size, struct app_header *out_hdr)
{
    struct app_header h;
    spiflash_read(slot_off, &h, sizeof h);
    if (!app_header_valid(&h, slot_size))
        return 0;
    const void *payload = (const void *)(SPIFLASH_MMAP + slot_off + sizeof h);
    if (crc32(payload, h.length) != h.payload_crc32)
        return 0;
    if (out_hdr)
        *out_hdr = h;
    return 1;
}

static int record_is_blank(const struct boot_record *r)
{
    return r->magic == 0xFFFFFFFFu && r->slot == 0xFFFFFFFFu &&
           r->seq   == 0xFFFFFFFFu && r->crc32 == 0xFFFFFFFFu;
}

static int record_is_valid(const struct boot_record *r)
{
    if (r->magic != BOOT_REC_TRY && r->magic != BOOT_REC_CONFIRM)
        return 0;
    if (r->slot > 1)
        return 0;
    return crc32(r, sizeof(*r) - sizeof(uint32_t)) == r->crc32;
}

void bootstate_scan(uint32_t state_off, struct boot_summary *sum)
{
    const struct boot_record *recs = (const struct boot_record *)(SPIFLASH_MMAP + state_off);
    sum->last_slot = -1;
    sum->last_seq = 0;
    sum->unconfirmed_tries = 0;
    sum->confirmed = 0;
    sum->records_used = 0;

    uint32_t n_recs = SPIFLASH_SECTOR / sizeof(struct boot_record);
    for (uint32_t i = 0; i < n_recs; i++) {
        struct boot_record r;
        memcpy(&r, &recs[i], sizeof r);
        if (record_is_blank(&r))
            break;
        sum->records_used = i + 1;
        if (!record_is_valid(&r))
            continue;
        if (r.magic == BOOT_REC_TRY) {
            if (sum->last_slot == (int)r.slot && sum->last_seq == r.seq && !sum->confirmed) {
                sum->unconfirmed_tries++;
            } else {
                sum->last_slot = (int)r.slot;
                sum->last_seq  = r.seq;
                sum->unconfirmed_tries = 1;
                sum->confirmed = 0;
            }
        } else if (r.magic == BOOT_REC_CONFIRM) {
            if (sum->last_slot == (int)r.slot && sum->last_seq == r.seq) {
                sum->unconfirmed_tries = 0;
                sum->confirmed = 1;
            }
        }
    }
}

int bootstate_append(uint32_t state_off, uint32_t magic, uint32_t slot, uint32_t seq)
{
    struct boot_summary sum;
    bootstate_scan(state_off, &sum);
    uint32_t n_recs = SPIFLASH_SECTOR / sizeof(struct boot_record);

    /* If sector is not formatted (first record not blank and no valid records) or full, erase and compact */
    if (sum.records_used >= n_recs || (sum.records_used > 0 && sum.last_slot < 0)) {
        if (spiflash_erase_sector(state_off) < 0)
            return -1;
        sum.records_used = 0;
        /* Preserve unconfirmed streak across sector rollover if needed */
        if (sum.last_slot >= 0 && !sum.confirmed && magic == BOOT_REC_TRY &&
            (uint32_t)sum.last_slot == slot && sum.last_seq == seq) {
            for (uint32_t k = 0; k < sum.unconfirmed_tries && k < BOOT_MAX_TRIES; k++) {
                struct boot_record prev = { BOOT_REC_TRY, slot, seq, 0 };
                prev.crc32 = crc32(&prev, sizeof(prev) - sizeof(uint32_t));
                spiflash_write(state_off + sum.records_used * sizeof(prev), &prev, sizeof prev);
                sum.records_used++;
            }
        }
    }

    struct boot_record rec = { magic, slot, seq, 0 };
    rec.crc32 = crc32(&rec, sizeof(rec) - sizeof(uint32_t));
    uint32_t off = state_off + sum.records_used * sizeof(rec);
    if (spiflash_write(off, &rec, sizeof rec) < 0)
        return -1;
    return spiflash_verify(off, &rec, sizeof rec);
}

int bootstate_record_try(uint32_t state_off, uint32_t slot, uint32_t seq)
{
    return bootstate_append(state_off, BOOT_REC_TRY, slot, seq);
}

int bootstate_confirm(uint32_t state_off)
{
    struct boot_summary sum;
    bootstate_scan(state_off, &sum);
    if (sum.last_slot < 0)
        return 0; /* nothing to confirm */
    if (sum.confirmed)
        return 0; /* already confirmed */
    return bootstate_append(state_off, BOOT_REC_CONFIRM, (uint32_t)sum.last_slot, sum.last_seq);
}

int stage2_choose_slot(const struct part_table *pt, uint32_t state_off,
                       struct app_header *chosen_hdr, uint32_t *chosen_off)
{
    const struct part_entry *pa = part_find(pt, "appA");
    const struct part_entry *pb = part_find(pt, "appB");
    uint32_t off_a = pa ? pa->offset : FLASH_SLOTA_OFF;
    uint32_t sz_a  = pa ? pa->size   : FLASH_SLOTA_SIZE;
    uint32_t off_b = pb ? pb->offset : FLASH_SLOTB_OFF;
    uint32_t sz_b  = pb ? pb->size   : FLASH_SLOTB_SIZE;

    struct app_header ha, hb;
    int va = app_slot_valid_flash(off_a, sz_a, &ha);
    int vb = app_slot_valid_flash(off_b, sz_b, &hb);

    if (!va && !vb)
        return -1;

    struct boot_summary sum;
    bootstate_scan(state_off, &sum);

    int pick;
    if (va && vb) {
        pick = (hb.seq > ha.seq) ? 1 : 0;
        uint32_t pick_seq = pick ? hb.seq : ha.seq;
        /* 3-strike fallback: if preferred slot failed to confirm BOOT_MAX_TRIES times in a row, fall back */
        if (sum.last_slot == pick && sum.last_seq == pick_seq &&
            !sum.confirmed && sum.unconfirmed_tries >= BOOT_MAX_TRIES) {
            pick ^= 1;
        }
    } else {
        pick = va ? 0 : 1;
    }

    if (chosen_hdr) *chosen_hdr = pick ? hb : ha;
    if (chosen_off) *chosen_off = pick ? off_b : off_a;
    return pick;
}
