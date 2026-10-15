/*
 * hid.h - minimal HID report descriptor parser and mouse report decoder
 *
 * Finds where buttons, X, Y, the vertical wheel and the horizontal wheel
 * (AC Pan) sit inside a mouse's input report, so reports can be decoded
 * in the device's own "report protocol" instead of the 3-byte boot
 * protocol (which has no wheel).
 */
#ifndef HID_H
#define HID_H

#include <stdint.h>

struct hid_field {
    uint16_t bit;        /* bit offset inside the report, after the report ID byte */
    uint8_t  size;       /* bits, 0 = field not present */
    uint8_t  is_signed;
} __attribute__((aligned(4)));   /* 4-byte copies without lwl/lwr (Lexra core) */

struct hid_mouse_layout {
    int      valid;
    uint8_t  report_id;  /* 0 = reports carry no ID byte */
    uint8_t  buttons;    /* number of button bits */
    struct hid_field btn0, x, y, wheel, pan;
    uint16_t report_bits;
};

struct hid_mouse_report {
    uint32_t buttons;    /* bit 0 = left, 1 = right, 2 = middle, ... */
    int32_t  dx, dy, wheel, pan;
};

int hid_parse_mouse(const uint8_t *desc, uint32_t len, struct hid_mouse_layout *out);
/* returns 0 on success, -1 if the report is not the mouse report (other ID or too short) */
int hid_decode_mouse(const struct hid_mouse_layout *l, const uint8_t *rep, uint32_t len,
                     struct hid_mouse_report *out);
void hid_print_layout(const struct hid_mouse_layout *l);

#endif
