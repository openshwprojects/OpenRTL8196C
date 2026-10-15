/*
 * usb_msc.c - bulk-only transport + SCSI, see usb_msc.h
 */
#include "usb_msc.h"
#include "usb.h"
#include "timer.h"
#include "uart.h"
#include "libc.h"

#define CBW_SIG 0x43425355u   /* "USBC" little-endian */
#define CSW_SIG 0x53425355u   /* "USBS" */
#define TIMEOUT_MS 5000u

static void put32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t get32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

struct msc_stats msc_stats;

/* bulk-only reset recovery: class reset, then clear both endpoint halts */
static void reset_recovery(struct usb_msc *m)
{
    msc_stats.reset_recoveries++;
    usb_control(0x21, 0xFF, 0, m->iface, 0, 0);
    usb_clear_halt(m->ep_in);
    usb_clear_halt(m->ep_out);
}

static int read_csw(struct usb_msc *m, uint8_t *csw)
{
    int n = usb_bulk(m->ep_in, m->maxp_in, csw, 13, TIMEOUT_MS);
    if (n == -2) {                         /* stalled: clear and try once more */
        msc_stats.stalls++;
        usb_clear_halt(m->ep_in);
        n = usb_bulk(m->ep_in, m->maxp_in, csw, 13, TIMEOUT_MS);
    }
    return n;
}

/* returns bytes of data moved, or <0; status != 0 in the CSW gives -100 - status */
static int bot(struct usb_msc *m, const uint8_t *cdb, uint8_t cdb_len, int in, void *data, uint32_t len)
{
    uint8_t cbw[31], csw[13];
    memset(cbw, 0, sizeof cbw);
    put32le(cbw, CBW_SIG);
    put32le(cbw + 4, ++m->tag);
    put32le(cbw + 8, len);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = m->lun;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    msc_stats.commands++;

    int n = usb_bulk(m->ep_out, m->maxp_out, cbw, 31, TIMEOUT_MS);
    if (n != 31) { msc_stats.bulk_errors++; reset_recovery(m); return n < 0 ? n : -5; }

    int moved = 0;
    if (len) {
        moved = usb_bulk(in ? m->ep_in : m->ep_out, in ? m->maxp_in : m->maxp_out, data, len, TIMEOUT_MS);
        if (moved == -2) { msc_stats.stalls++; usb_clear_halt(in ? m->ep_in : m->ep_out); moved = 0; }
        else if (moved < 0) { msc_stats.bulk_errors++; reset_recovery(m); return moved; }
    }
    n = read_csw(m, csw);
    if (n != 13 || get32le(csw) != CSW_SIG || get32le(csw + 4) != m->tag) {
        msc_stats.bulk_errors++;
        reset_recovery(m);
        return -6;
    }
    uint32_t residue = get32le(csw + 8);
    if (residue) { msc_stats.residue_cmds++; msc_stats.last_residue = residue; }
    if (csw[12] == 1) msc_stats.failed_status++;
    if (csw[12] == 2) msc_stats.phase_errors++;
    if (csw[12] != 0) return -100 - csw[12];          /* 1 = command failed, 2 = phase error */
    if (moved != (int)len) msc_stats.bulk_errors++;   /* short data phase: counted, caller decides */
    return moved;
}

static int request_sense(struct usb_msc *m)
{
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 }, s[18];
    memset(s, 0, sizeof s);
    int n = bot(m, cdb, 6, 1, s, 18);
    if (n >= 14) { m->sense_key = s[2] & 0x0F; m->asc = s[12]; m->ascq = s[13]; }
    return n;
}

int msc_test_unit_ready(struct usb_msc *m)
{
    uint8_t cdb[6] = { 0 };
    int rc = bot(m, cdb, 6, 0, 0, 0);
    if (rc <= -100) request_sense(m);
    return rc;
}

static void copy_trim(char *dst, const uint8_t *src, int n)
{
    memcpy(dst, src, n);
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

int msc_init(struct usb_msc *m)
{
    struct usb_ep_info in, out;
    memset(m, 0, sizeof *m);
    if (usb_find_endpoint(0x08, 2, 1, &in) < 0 || usb_find_endpoint(0x08, 2, 0, &out) < 0)
        return -1;                                     /* no mass storage interface */
    if (in.subclass != 0x06 || in.protocol != 0x50)
        printf("msc: subclass %02x protocol %02x (expected 06/50), trying anyway\n", in.subclass, in.protocol);
    m->ep_in = in.ep;   m->maxp_in = in.maxp;
    m->ep_out = out.ep; m->maxp_out = out.maxp;
    m->iface = in.iface;

    uint8_t maxlun = 0;
    if (usb_control(0xA1, 0xFE, 0, m->iface, 1, &maxlun) < 1) maxlun = 0;   /* GET MAX LUN, may stall */

    uint8_t inq[36], cdb[12];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x12; cdb[4] = 36;                        /* INQUIRY */
    memset(inq, 0, sizeof inq);
    if (bot(m, cdb, 6, 1, inq, 36) < 0) return -2;
    m->removable = (inq[1] & 0x80) != 0;
    copy_trim(m->vendor, inq + 8, 8);
    copy_trim(m->product, inq + 16, 16);
    copy_trim(m->revision, inq + 32, 4);

    /* the medium may need a moment after power-up: TEST UNIT READY until it settles */
    int ready = 0;
    for (int i = 0; i < 20 && !ready; i++) {
        if (msc_test_unit_ready(m) == 0) ready = 1;
        else delay_ms(100);
    }
    if (!ready) return -3;                             /* no medium (empty card reader) */

    uint8_t cap[8];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x25;                                     /* READ CAPACITY(10) */
    if (bot(m, cdb, 10, 1, cap, 8) != 8) { request_sense(m); return -4; }
    m->blocks = get32be(cap) + 1;
    m->block_size = get32be(cap + 4);
    (void)maxlun;
    return 0;
}

static int rw10(struct usb_msc *m, uint8_t op, uint32_t lba, uint32_t count, void *buf, int in)
{
    uint8_t *p = buf;
    const uint32_t per_cmd = USB_BULK_MAX / 512;       /* sectors per SCSI command: one bulk transfer */
    while (count) {
        uint32_t n = count > per_cmd ? per_cmd : count;
        uint8_t cdb[10] = { op, 0 };
        put32be(cdb + 2, lba);
        cdb[7] = (uint8_t)(n >> 8);
        cdb[8] = (uint8_t)n;
        int rc = -1;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt) msc_stats.retries++;
            rc = bot(m, cdb, 10, in, p, n * m->block_size);
            if (rc == (int)(n * m->block_size)) break;
            if (rc <= -100) request_sense(m);
        }
        if (rc != (int)(n * m->block_size)) return rc < 0 ? rc : -7;
        lba += n; count -= n; p += n * m->block_size;
    }
    return 0;
}

int msc_read(struct usb_msc *m, uint32_t lba, uint32_t count, void *buf)
{
    return rw10(m, 0x28, lba, count, buf, 1);
}

int msc_write(struct usb_msc *m, uint32_t lba, uint32_t count, const void *buf)
{
    return rw10(m, 0x2A, lba, count, (void *)buf, 0);
}

/* MODE SENSE header, WP bit: 1 = write protected, 0 = writable, <0 = unknown.
 * Tries MODE SENSE(6) first, MODE SENSE(10) if the device rejects it. */
int msc_write_protected(struct usb_msc *m)
{
    uint8_t cdb[10], h[8];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x1A; cdb[2] = 0x3F; cdb[4] = 4;          /* MODE SENSE(6), all pages, header only */
    memset(h, 0, sizeof h);
    int rc = bot(m, cdb, 6, 1, h, 4);
    if (rc >= 3) return (h[2] & 0x80) != 0;
    if (rc <= -100) request_sense(m);
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x5A; cdb[2] = 0x3F; cdb[8] = 8;          /* MODE SENSE(10) */
    rc = bot(m, cdb, 10, 1, h, 8);
    if (rc >= 4) return (h[3] & 0x80) != 0;
    if (rc <= -100) request_sense(m);
    return rc < 0 ? rc : -1;
}

/* SYNCHRONIZE CACHE(10): commit the device's write cache.  Many sticks
 * answer "invalid command" because they write through; that is fine. */
int msc_sync(struct usb_msc *m)
{
    uint8_t cdb[10] = { 0x35, 0 };
    int rc = bot(m, cdb, 10, 0, 0, 0);
    if (rc <= -100) { request_sense(m); return m->sense_key == 0x05 ? 0 : rc; }
    return rc < 0 ? rc : 0;
}
