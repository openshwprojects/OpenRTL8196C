/*
 * 18_usb_mouse - read a USB mouse plugged into the router, wheel included.
 *
 * On connect: read the HID report descriptor, parse where buttons, X, Y and
 * the wheel sit, keep the mouse in its own report protocol and decode every
 * report with that layout.  Falls back to the 3-byte boot protocol (no
 * wheel) if the descriptor cannot be parsed.  The left button lights LAN
 * LED 0, the wheel position is shown on LEDs 1..6.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "usb.h"
#include "hid.h"

static struct hid_mouse_layout layout;
static uint8_t rdesc[512];

static void setup_mouse(const struct usb_ep_info *ep)
{
    const struct usb_dev *d = usb_dev();
    uint16_t rlen = 0;
    /* report descriptor length from the HID class descriptor (type 0x21) */
    for (unsigned i = 0; i + 8 < d->cfg_len; i += d->cfg_desc[i] ? d->cfg_desc[i] : 1)
        if (d->cfg_desc[i + 1] == 0x21) { rlen = (uint16_t)(d->cfg_desc[i + 7] | (d->cfg_desc[i + 8] << 8)); break; }
    if (rlen > sizeof rdesc) rlen = sizeof rdesc;

    int n = rlen ? usb_control(0x81, 0x06, 0x2200, ep->iface, rlen, rdesc) : -1;
    printf("report descriptor: %d bytes:", n);
    for (int i = 0; i < n; i++) printf(" %02x", rdesc[i]);
    printf("\n");

    if (n > 0 && hid_parse_mouse(rdesc, (uint32_t)n, &layout) == 0) {
        usb_control(0x21, 0x0B, 1, ep->iface, 0, 0);          /* SET_PROTOCOL(report) */
        hid_print_layout(&layout);
    } else {
        usb_control(0x21, 0x0B, 0, ep->iface, 0, 0);          /* SET_PROTOCOL(boot) */
        layout.valid = 1; layout.report_id = 0; layout.buttons = 3; layout.report_bits = 24;
        layout.btn0 = (struct hid_field){ 0, 1, 0 };
        layout.x = (struct hid_field){ 8, 8, 1 };
        layout.y = (struct hid_field){ 16, 8, 1 };
        layout.wheel = (struct hid_field){ 0, 0, 0 };
        layout.pan = (struct hid_field){ 0, 0, 0 };
        printf("could not parse the report descriptor, using boot protocol (no wheel)\n");
    }
    usb_control(0x21, 0x0A, 0, ep->iface, 0, 0);              /* SET_IDLE(0): report on change */
}

int main(void)
{
    uart_init();
    timer_delay_init();
    gpio_leds_init();
    printf("\n\nusb mouse example (with wheel)\n");

    if (usb_host_init() < 0) {
        printf("usb: controller does not respond\n");
        return 1;
    }
    usb_vbus(1);

    int x = 0, y = 0, wheel = 0, pan = 0, reading = 0;
    uint32_t reports = 0, last_status = micros(), reports_at_status = 0;
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            usb_print_dev(usb_dev());
            struct usb_ep_info ep;
            if (usb_find_endpoint(3 /* HID */, 3 /* interrupt */, 1, &ep) == 0) {
                setup_mouse(&ep);
                int rc = usb_intr_open(ep.ep, ep.maxp);
                printf("endpoint %02x maxp %u: %s\n", ep.ep, ep.maxp, rc == 0 ? "reading reports" : "open failed");
                reading = rc == 0;
                x = y = wheel = pan = 0;
            } else {
                printf("not a HID device with an interrupt endpoint\n");
            }
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("device removed\n");
            reading = 0;
        } else if (ev == USB_EV_ENUM_FAILED) {
            printf("enumeration failed, power-cycling the port\n");
            usb_vbus(0);
            delay_ms(1000);
            usb_vbus(1);
        }

        if (reading) {
            uint8_t r[16];
            int n = usb_intr_poll(r, sizeof r);
            struct hid_mouse_report m;
            if (n > 0 && hid_decode_mouse(&layout, r, (uint32_t)n, &m) == 0) {
                reports++;
                x += m.dx; y += m.dy; wheel += m.wheel; pan += m.pan;
                printf("report %u: buttons %c%c%c  dx %4d dy %4d wheel %3d pan %2d  -> pos %6d %6d wheel %4d  [%d bytes:",
                       reports, (m.buttons & 1) ? 'L' : '-', (m.buttons & 4) ? 'M' : '-', (m.buttons & 2) ? 'R' : '-',
                       m.dx, m.dy, m.wheel, m.pan, x, y, wheel, n);
                for (int i = 0; i < n; i++) printf(" %02x", r[i]);
                printf("]\n");
                int w = wheel % 6;
                if (w < 0) w += 6;
                gpio_leds_write((m.buttons & 1) | (2u << w));
            } else if (n > 0) {
                printf("other report (%d bytes): %02x %02x %02x %02x\n", n, r[0], r[1], r[2], r[3]);
            } else if (n < 0) {
                printf("interrupt transfer error %d, re-opening\n", n);
                struct usb_ep_info ep;
                if (usb_find_endpoint(3, 3, 1, &ep) == 0) usb_intr_open(ep.ep, ep.maxp);
            }
        }
        if (every_ms(&last_status, 10000)) {
            printf("status: %s, %u reports so far, %u in the last 10 s, ports disabled by controller %u\n",
                   reading ? "mouse active" : "no mouse", reports, reports - reports_at_status,
                   usb_errors.port_disabled);
            if (reading && reports == reports_at_status) {   /* quiet: show why */
                usb_print_controller();
                usb_intr_debug();
            }
            reports_at_status = reports;
        }
        delay_ms(2);
    }
}
