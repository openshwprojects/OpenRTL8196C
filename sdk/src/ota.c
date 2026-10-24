/*
 * ota.c - Over-The-Air firmware update & RAM execution over TCP
 */
#include "rtl8196c.h"
#include "ota.h"
#include "swcore.h"
#include "flash_layout.h"
#include "spiflash.h"
#include "tcp.h"
#include "net.h"
#include "timer.h"
#include "uart.h"
#include "cli.h"
#include "libc.h"

extern const uint32_t ota_reloc_stub[];
extern const uint32_t ota_reloc_stub_end[];

uint32_t ota_sessions, ota_bytes_rcvd;

static struct {
    tcp_conn_t *conn;
    uint32_t    hdr_got;
    uint8_t     hdr[16];
    uint32_t    mode;
    uint32_t    expected_len;
    uint32_t    target_addr;
    uint32_t    rcvd;
    int         done;
} s_rx;

static struct {
    int         action;     /* 0 = none, 1 = ram jump, 2 = wdt reboot */
    uint32_t    t_sched;
    const void *src;
    uint32_t    dst;
    uint32_t    len;
    uint32_t    entry;
} s_pend;

void ota_ram_jump(const void *src, uint32_t dst_addr, uint32_t len, uint32_t entry_addr)
{
    /* Quiesce switch DMA, USB host DMA, PCIe Wi-Fi DMA, and interrupts before
     * overwriting .text/.data/.bss, matching the Realtek bootloader's "J" behavior. */
    REG32(INTC_GIMR) = 0;
    REG32(CPU_ICR) = 0;
    REG32(CPU_IIMR) = 0;
    for (int p = 0; p < NUM_PHY_PORTS; p++)
        REG32(SW_PCRP(p)) &= ~PCRP_ENABLE_PHYIF;
    if (REG32(SYS_BASE + 0x10) & (1u << 17)) {
        REG32(0xB8021010u) = 0; /* EHCI USBCMD = stop */
        REG32(0xB8020004u) = 0; /* OHCI HcControl = reset */
    }
    if (REG32(SYS_BASE + 0x10) & (1u << 11)) {
        REG32(0xB8B00004u) = 0;                /* disable PCIe root port bus-master DMA */
        REG32(SYS_BASE + 0x10) &= ~(1u << 12); /* assert PCIe PERST# device reset */
    }

    volatile uint32_t *stub = (volatile uint32_t *)OTA_RELOC_ADDR;
    for (const uint32_t *p = ota_reloc_stub; p < ota_reloc_stub_end; p++)
        *stub++ = *p;

    uint32_t words = (len + 3u) / 4u;
    /* Call position-independent stub at OTA_RELOC_ADDR(dst, src, words, entry) */
    typedef void (*reloc_fn_t)(uint32_t dst, uint32_t src, uint32_t nwords, uint32_t entry);
    reloc_fn_t fn = (reloc_fn_t)OTA_RELOC_ADDR;
    fn(dst_addr, (uint32_t)src, words, entry_addr);
    for (;;)
        ;
}

/*
 * Program a validated RAPP image into [slot_off .. slot_off + slot_size).
 * Writes page 0 (containing magic + new_seq + header_crc32) LAST so an
 * interrupted write never leaves a half-written slot looking valid.
 */
int ota_flash_rapp_slot(const void *img, uint32_t total_len,
                        uint32_t slot_off, uint32_t slot_size, uint32_t new_seq)
{
    if (!app_image_valid_mem(img, total_len, slot_size))
        return -1;

    uint8_t page0[SPIFLASH_PAGE];
    memset(page0, 0xFF, sizeof page0);
    uint32_t page0_len = total_len < SPIFLASH_PAGE ? total_len : SPIFLASH_PAGE;
    memcpy(page0, img, page0_len);

    struct app_header *h = (struct app_header *)page0;
    h->seq = new_seq;
    h->header_crc32 = app_header_crc(h);

    if (spiflash_erase_range(slot_off, total_len) < 0)
        return -2;

    if (total_len > SPIFLASH_PAGE) {
        const uint8_t *rest = (const uint8_t *)img + SPIFLASH_PAGE;
        uint32_t rest_len = total_len - SPIFLASH_PAGE;
        if (spiflash_write(slot_off + SPIFLASH_PAGE, rest, rest_len) < 0)
            return -3;
        if (spiflash_verify(slot_off + SPIFLASH_PAGE, rest, rest_len) < 0)
            return -4;
    }

    /* Commit page 0 with magic and new sequence number last */
    if (spiflash_write(slot_off, page0, page0_len) < 0)
        return -5;
    if (spiflash_verify(slot_off, page0, page0_len) < 0)
        return -6;

    if (!app_slot_valid_flash(slot_off, slot_size, 0))
        return -7;

    return 0;
}

int ota_flash_inactive_slot(const void *img, uint32_t total_len,
                            int *out_slot, uint32_t *out_off, uint32_t *out_seq)
{
    struct part_table pt;
    part_table_read(&pt, FLASH_PARTTABLE_OFF);
    const struct part_entry *pa = part_find(&pt, "appA");
    const struct part_entry *pb = part_find(&pt, "appB");
    uint32_t off_a = pa ? pa->offset : FLASH_SLOTA_OFF;
    uint32_t sz_a  = pa ? pa->size   : FLASH_SLOTA_SIZE;
    uint32_t off_b = pb ? pb->offset : FLASH_SLOTB_OFF;
    uint32_t sz_b  = pb ? pb->size   : FLASH_SLOTB_SIZE;

    struct app_header ha, hb;
    int va = app_slot_valid_flash(off_a, sz_a, &ha);
    int vb = app_slot_valid_flash(off_b, sz_b, &hb);

    int target_slot = 0;
    uint32_t next_seq = 1;
    if (va && !vb) {
        target_slot = 1;
        next_seq = ha.seq + 1;
    } else if (!va && vb) {
        target_slot = 0;
        next_seq = hb.seq + 1;
    } else if (va && vb) {
        target_slot = (ha.seq <= hb.seq) ? 0 : 1;
        next_seq = ((ha.seq > hb.seq) ? ha.seq : hb.seq) + 1;
    }

    uint32_t dst_off = target_slot ? off_b : off_a;
    uint32_t dst_sz  = target_slot ? sz_b  : sz_a;

    if (out_slot) *out_slot = target_slot;
    if (out_off)  *out_off  = dst_off;
    if (out_seq)  *out_seq  = next_seq;

    return ota_flash_rapp_slot(img, total_len, dst_off, dst_sz, next_seq);
}

int ota_boot_active_slot(int verbose)
{
    struct part_table pt;
    int pt_fallback = part_table_read(&pt, FLASH_PARTTABLE_OFF);
    if (verbose)
        printf("\n\n[stage2] OpenRTL8196C chooser (ptable=%s)\n", pt_fallback ? "default" : "flash");

    struct app_header hdr;
    uint32_t slot_off = 0;
    int slot = stage2_choose_slot(&pt, FLASH_BOOTSTATE_OFF, &hdr, &slot_off);
    if (slot < 0)
        return -1;

    if (verbose) {
        printf("[stage2] selected %s at %06x: seq=%u ver=%s len=%u load=%08x entry=%08x\n",
               slot ? "appB" : "appA", slot_off, hdr.seq, hdr.version,
               hdr.length, hdr.load_addr, hdr.entry);
    }
    bootstate_record_try(FLASH_BOOTSTATE_OFF, (uint32_t)slot, hdr.seq);
    const void *payload = (const void *)(SPIFLASH_MMAP + slot_off + sizeof hdr);
    ota_ram_jump(payload, hdr.load_addr, hdr.length, hdr.entry);
    return 0;
}

static void reply_and_close(tcp_conn_t *c, const char *msg)
{
    tcp_send(c, msg, strlen(msg));
    tcp_close(c);
    s_rx.done = 1;
}

static void handle_status(tcp_conn_t *c)
{
    char buf[256];
    struct sb b;
    sb_init(&b, buf, sizeof buf);

    struct part_table pt;
    int pt_fallback = part_table_read(&pt, FLASH_PARTTABLE_OFF);
    struct app_header ha, hb;
    int va = app_slot_valid_flash(FLASH_SLOTA_OFF, FLASH_SLOTA_SIZE, &ha);
    int vb = app_slot_valid_flash(FLASH_SLOTB_OFF, FLASH_SLOTB_SIZE, &hb);
    struct boot_summary sum;
    bootstate_scan(FLASH_BOOTSTATE_OFF, &sum);

    int pick = -1;
    if (va && vb) {
        pick = (hb.seq > ha.seq) ? 1 : 0;
        uint32_t pick_seq = pick ? hb.seq : ha.seq;
        if (sum.last_slot == pick && sum.last_seq == pick_seq &&
            !sum.confirmed && sum.unconfirmed_tries >= BOOT_MAX_TRIES) {
            pick ^= 1;
        }
    } else if (va) {
        pick = 0;
    } else if (vb) {
        pick = 1;
    }

    sb_putf(&b, "OK status ptable=%s slotA=%s(seq=%u) slotB=%s(seq=%u) pick=%d boot(last=%d,seq=%u,tries=%u,conf=%d,recs=%u)\n",
            pt_fallback ? "default" : "flash",
            va ? ha.version : "invalid", va ? ha.seq : 0u,
            vb ? hb.version : "invalid", vb ? hb.seq : 0u,
            pick, sum.last_slot, sum.last_seq, sum.unconfirmed_tries, sum.confirmed, sum.records_used);
    reply_and_close(c, b.buf);
}

static void finish_upload(tcp_conn_t *c)
{
    char msg[200];
    const uint8_t *stage = (const uint8_t *)OTA_STAGE_ADDR;
    uint32_t len = s_rx.expected_len;
    uint32_t img_crc = crc32(stage, len);

    if (s_rx.mode == OTA_CMD_RAM) {
        uint32_t load_addr = s_rx.target_addr ? s_rx.target_addr : LOAD_ADDR;
        uint32_t entry_addr = load_addr;
        const void *payload = stage;
        uint32_t payload_len = len;

        if (len >= sizeof(struct app_header) && memcmp(stage, "RAPP", 4) == 0) {
            if (!app_image_valid_mem(stage, len, OTA_MAX_SIZE)) {
                reply_and_close(c, "ERR bad RAPP header or payload CRC\n");
                return;
            }
            struct app_header h;
            memcpy(&h, stage, sizeof h);
            load_addr = h.load_addr;
            entry_addr = h.entry;
            payload = stage + sizeof h;
            payload_len = h.length;
            img_crc = h.payload_crc32;
        }

        snprintf(msg, sizeof msg, "OK mode=ram len=%u crc=%08x load=%08x entry=%08x\n",
                 payload_len, img_crc, load_addr, entry_addr);
        reply_and_close(c, msg);
        printf("[ota] RAM image ready (%u bytes, crc %08x) -> jumping to %08x\n",
               payload_len, img_crc, entry_addr);
        s_pend.src = payload;
        s_pend.dst = load_addr;
        s_pend.len = payload_len;
        s_pend.entry = entry_addr;
        s_pend.t_sched = micros();
        s_pend.action = 1;
        return;
    }

    if (s_rx.mode == OTA_CMD_TEST_FLASH) {
        uint32_t test_off = s_rx.target_addr ? s_rx.target_addr : FLASH_TEST_BASE;
        uint32_t max_sz = 0x1F000u; /* up to 124 KB in 0x7E0000..0x7FEFFF */
        if (test_off < 0x600000u || test_off + len > 0x7FF000u || len > max_sz) {
            reply_and_close(c, "ERR test_flash offset out of safe test range (0x600000..0x7FF000)\n");
            return;
        }
        if (!app_image_valid_mem(stage, len, max_sz)) {
            reply_and_close(c, "ERR test_flash requires a valid RAPP image\n");
            return;
        }
        uint32_t t0 = micros();
        int rc = ota_flash_rapp_slot(stage, len, test_off, max_sz, 42);
        uint32_t dt_ms = elapsed_us(t0) / 1000u;
        if (rc < 0) {
            snprintf(msg, sizeof msg, "ERR test_flash write/verify failed (%d)\n", rc);
            reply_and_close(c, msg);
            return;
        }
        struct app_header hdr_back;
        int valid_back = app_slot_valid_flash(test_off, max_sz, &hdr_back);

        /* Restore original zeros so stock flash remains untouched */
        spiflash_erase_range(test_off, len);
        static uint8_t zero_page[SPIFLASH_PAGE];
        memset(zero_page, 0, sizeof zero_page);
        uint32_t end_aligned = (test_off + len + SPIFLASH_SECTOR - 1) & ~(SPIFLASH_SECTOR - 1);
        for (uint32_t off = test_off & ~(SPIFLASH_SECTOR - 1); off < end_aligned; off += SPIFLASH_PAGE)
            spiflash_write(off, zero_page, SPIFLASH_PAGE);

        snprintf(msg, sizeof msg,
                 "OK mode=test_flash off=%06x len=%u seq=%u crc=%08x time_ms=%u valid=%d restored=1\n",
                 test_off, len, hdr_back.seq, hdr_back.payload_crc32, dt_ms, valid_back);
        reply_and_close(c, msg);
        return;
    }

    if (s_rx.mode == OTA_CMD_FLASH) {
        int target_slot = 0;
        uint32_t dst_off = 0, next_seq = 0;
        uint32_t t0 = micros();
        int rc = ota_flash_inactive_slot(stage, len, &target_slot, &dst_off, &next_seq);
        uint32_t dt_ms = elapsed_us(t0) / 1000u;
        if (rc == -1) {
            reply_and_close(c, "ERR bad RAPP image or exceeds slot size\n");
            return;
        }
        if (rc < 0) {
            snprintf(msg, sizeof msg, "ERR flash slot write failed (%d)\n", rc);
            reply_and_close(c, msg);
            return;
        }
        snprintf(msg, sizeof msg, "OK mode=flash slot=%s off=%06x seq=%u len=%u crc=%08x time_ms=%u\n",
                 target_slot ? "appB" : "appA", dst_off, next_seq, len, img_crc, dt_ms);
        reply_and_close(c, msg);
        if (s_rx.target_addr & 1u) {
            s_pend.t_sched = micros();
            s_pend.action = 2;
        }
        return;
    }

    if (s_rx.mode == OTA_CMD_RAW_FLASH) {
        uint32_t dst_off = s_rx.target_addr;
        if (dst_off < SPIFLASH_PROTECTED_END || dst_off + len > SPIFLASH_SIZE || (dst_off & (SPIFLASH_SECTOR - 1)) != 0) {
            reply_and_close(c, "ERR bad raw flash offset or length\n");
            return;
        }
        uint32_t t0 = micros();
        if (spiflash_erase_range(dst_off, len) < 0 ||
            spiflash_write(dst_off, stage, len) < 0 ||
            spiflash_verify(dst_off, stage, len) < 0) {
            reply_and_close(c, "ERR raw flash erase/write/verify failed\n");
            return;
        }
        uint32_t dt_ms = elapsed_us(t0) / 1000u;
        snprintf(msg, sizeof msg, "OK mode=raw off=%06x len=%u crc=%08x time_ms=%u\n",
                 dst_off, len, img_crc, dt_ms);
        reply_and_close(c, msg);
        return;
    }

    reply_and_close(c, "ERR unknown OTA mode\n");
}

static void ota_on_data(tcp_conn_t *c, const uint8_t *data, uint32_t len)
{
    if (c != s_rx.conn || s_rx.done)
        return;

    while (len && s_rx.hdr_got < 16) {
        s_rx.hdr[s_rx.hdr_got++] = *data++;
        len--;
        if (s_rx.hdr_got == 16) {
            if (memcmp(s_rx.hdr, "ROTA", 4) != 0) {
                reply_and_close(c, "ERR bad magic (expected ROTA)\n");
                return;
            }
            s_rx.mode         = rd32(s_rx.hdr + 4);
            s_rx.expected_len = rd32(s_rx.hdr + 8);
            s_rx.target_addr  = rd32(s_rx.hdr + 12);
            s_rx.rcvd         = 0;

            if (s_rx.mode == OTA_CMD_STATUS) {
                handle_status(c);
                return;
            }
            if (s_rx.expected_len == 0 || s_rx.expected_len > OTA_MAX_SIZE) {
                reply_and_close(c, "ERR invalid OTA length\n");
                return;
            }
        }
    }

    if (len && s_rx.hdr_got == 16 && !s_rx.done) {
        uint32_t rem = s_rx.expected_len - s_rx.rcvd;
        uint32_t n = len < rem ? len : rem;
        memcpy((uint8_t *)OTA_STAGE_ADDR + s_rx.rcvd, data, n);
        s_rx.rcvd += n;
        ota_bytes_rcvd += n;
        if (s_rx.rcvd == s_rx.expected_len)
            finish_upload(c);
    }
}

static void ota_on_event(tcp_conn_t *c, int ev)
{
    if (ev == TCP_EV_ACCEPT) {
        if (s_rx.conn && s_rx.conn != c)
            tcp_abort(s_rx.conn);
        memset(&s_rx, 0, sizeof s_rx);
        s_rx.conn = c;
        ota_sessions++;
    } else if (ev == TCP_EV_PEER_FIN) {
        if (c == s_rx.conn && !s_rx.done)
            reply_and_close(c, "ERR premature EOF\n");
    } else if (ev == TCP_EV_CLOSED) {
        if (c == s_rx.conn)
            s_rx.conn = 0;
    }
}

int ota_init(uint16_t port)
{
    memset(&s_rx, 0, sizeof s_rx);
    memset(&s_pend, 0, sizeof s_pend);
    return tcp_listen(port, ota_on_data, ota_on_event);
}

void ota_poll(void)
{
    if (!s_pend.action)
        return;
    /* Let the TCP OK response and FIN flush out for 80 ms before jumping/rebooting */
    if (elapsed_us(s_pend.t_sched) < 80000u)
        return;
    int act = s_pend.action;
    s_pend.action = 0;
    if (act == 1)
        ota_ram_jump(s_pend.src, s_pend.dst, s_pend.len, s_pend.entry);
    else if (act == 2)
        wdt_reboot();
}
