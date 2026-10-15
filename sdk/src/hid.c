/*
 * hid.c - see hid.h.  Handles short items, report IDs, usage lists and
 * usage ranges, constant padding and 32-bit "extended" usages.
 */
#include "hid.h"
#include "uart.h"
#include "libc.h"

#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_BUTTON          0x09
#define PAGE_CONSUMER        0x0C
#define USAGE_X              0x30
#define USAGE_Y              0x31
#define USAGE_WHEEL          0x38
#define USAGE_AC_PAN         0x238

#define MAX_IDS    8
#define MAX_USAGES 16

struct candidate {
    struct hid_mouse_layout l;
    uint16_t bits;           /* running offset for this report ID */
    int used;
};

static uint32_t item_value(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_signed(const uint8_t *p, int size)
{
    uint32_t v = item_value(p, size);
    if (size == 1) return (int8_t)v;
    if (size == 2) return (int16_t)v;
    return (int32_t)v;
}

int hid_parse_mouse(const uint8_t *d, uint32_t len, struct hid_mouse_layout *out)
{
    struct candidate c[MAX_IDS];
    memset(c, 0, sizeof c);
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t lmin = 0;
    uint32_t usages[MAX_USAGES];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    int have_range = 0;

    for (uint32_t i = 0; i < len; ) {
        uint8_t b = d[i];
        if (b == 0xFE) {                                 /* long item */
            if (i + 2 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = b & 3;
        if (size == 3) size = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + size > len) break;
        const uint8_t *p = d + i + 1;
        uint32_t v = item_value(p, size);

        if (type == 1) {                                 /* global */
            switch (tag) {
            case 0: page = v; break;
            case 1: lmin = item_signed(p, size); break;
            case 7: rsize = v; break;
            case 8: rid = v; break;
            case 9: rcount = v; break;
            }
        } else if (type == 2) {                          /* local */
            uint32_t full = size == 4 ? v : (page << 16) | v;
            if (tag == 0 && nusages < MAX_USAGES) usages[nusages++] = full;
            else if (tag == 1) { umin = full; have_range = 1; }
            else if (tag == 2) { umax = full; have_range = 1; }
        } else if (type == 0) {                          /* main */
            if (tag == 8) {                              /* Input */
                int slot = -1;
                for (int k = 0; k < MAX_IDS; k++)
                    if (c[k].used && c[k].l.report_id == rid) { slot = k; break; }
                if (slot < 0)
                    for (int k = 0; k < MAX_IDS; k++)
                        if (!c[k].used) { slot = k; c[k].used = 1; c[k].l.report_id = (uint8_t)rid; break; }
                if (slot >= 0) {
                    struct candidate *cd = &c[slot];
                    int constant = v & 1, variable = (v >> 1) & 1;
                    for (uint32_t n = 0; n < rcount; n++) {
                        uint16_t bit = (uint16_t)(cd->bits + n * rsize);
                        if (!constant && variable) {
                            uint32_t u;
                            if (nusages) u = usages[n < (uint32_t)nusages ? n : (uint32_t)nusages - 1];
                            else if (have_range) u = umin + n <= umax ? umin + n : umax;
                            else u = 0;
                            uint16_t upage = (uint16_t)(u >> 16), uid = (uint16_t)u;
                            struct hid_field f = { bit, (uint8_t)rsize, lmin < 0 };
                            if (upage == PAGE_BUTTON) {
                                if (!cd->l.buttons) cd->l.btn0 = f;
                                cd->l.buttons++;
                            } else if (upage == PAGE_GENERIC_DESKTOP && uid == USAGE_X) cd->l.x = f;
                            else if (upage == PAGE_GENERIC_DESKTOP && uid == USAGE_Y) cd->l.y = f;
                            else if (upage == PAGE_GENERIC_DESKTOP && uid == USAGE_WHEEL) cd->l.wheel = f;
                            else if (upage == PAGE_CONSUMER && uid == USAGE_AC_PAN) cd->l.pan = f;
                        }
                    }
                    cd->bits = (uint16_t)(cd->bits + rcount * rsize);
                    cd->l.report_bits = cd->bits;
                }
            }
            if (tag == 8 || tag == 9 || tag == 11 || tag == 10) {   /* main items clear locals */
                nusages = 0; have_range = 0; umin = umax = 0;
            }
        }
        i += 1 + size;
    }

    for (int k = 0; k < MAX_IDS; k++) {
        if (c[k].used && c[k].l.x.size && c[k].l.y.size) {
            *out = c[k].l;
            out->valid = 1;
            return 0;
        }
    }
    memset(out, 0, sizeof *out);
    return -1;
}

static int32_t get_field(const struct hid_field *f, const uint8_t *data, uint32_t nbytes)
{
    if (!f->size) return 0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < f->size && i < 32; i++) {
        uint32_t bit = f->bit + i;
        if (bit / 8 >= nbytes) break;
        if (data[bit / 8] & (1u << (bit % 8))) v |= 1u << i;
    }
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;
    return (int32_t)v;
}

int hid_decode_mouse(const struct hid_mouse_layout *l, const uint8_t *rep, uint32_t len,
                     struct hid_mouse_report *out)
{
    if (!l->valid) return -1;
    if (l->report_id) {
        if (len < 1 || rep[0] != l->report_id) return -1;
        rep++;
        len--;
    }
    if (len * 8 < l->report_bits) return -1;
    out->buttons = 0;
    for (int i = 0; i < l->buttons && i < 32; i++) {
        uint32_t bit = l->btn0.bit + (uint32_t)i * l->btn0.size;
        if (bit / 8 < len && (rep[bit / 8] & (1u << (bit % 8)))) out->buttons |= 1u << i;
    }
    out->dx = get_field(&l->x, rep, len);
    out->dy = get_field(&l->y, rep, len);
    out->wheel = get_field(&l->wheel, rep, len);
    out->pan = get_field(&l->pan, rep, len);
    return 0;
}

void hid_print_layout(const struct hid_mouse_layout *l)
{
    if (!l->valid) { printf("hid: no mouse layout\n"); return; }
    printf("hid mouse layout: report id %u, %u bits; buttons %u at bit %u; X %u@%u%s; Y %u@%u%s; wheel %s",
           l->report_id, l->report_bits, l->buttons, l->btn0.bit,
           l->x.size, l->x.bit, l->x.is_signed ? "s" : "", l->y.size, l->y.bit, l->y.is_signed ? "s" : "",
           l->wheel.size ? "" : "none");
    if (l->wheel.size) printf("%u@%u", l->wheel.size, l->wheel.bit);
    if (l->pan.size) printf("; pan %u@%u", l->pan.size, l->pan.bit);
    printf("\n");
}
