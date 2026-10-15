/*
 * usb.h - USB 2.0 host for the RTL8196C (one root port)
 *
 * Hardware: a Synopsys EHCI controller at 0xB8021000 (high speed) with one
 * OHCI companion at 0xB8020000 (full/low speed), one shared root port, one
 * Synopsys UTMI PHY.  Both controllers are little-endian on this big-endian
 * SoC.  VBUS is switched by GPIO A4 (high = on), the USB LED is GPIO C3
 * (active low).  Sequence follows the stock LT-6408n kernel.
 *
 * Scope today: root port only (no hub support), control transfers,
 * enumeration with descriptors and strings, hotplug by polling.
 * Bulk/interrupt transfers come next.
 */
#ifndef USB_H
#define USB_H

#include <stdint.h>

enum { USB_SPEED_FULL = 0, USB_SPEED_LOW = 1, USB_SPEED_HIGH = 2 };

enum {
    USB_EV_NONE = 0,
    USB_EV_CONNECTED,       /* device enumerated, usb_dev() is valid */
    USB_EV_DISCONNECTED,
    USB_EV_ENUM_FAILED,     /* something is attached but did not answer */
};

#define USB_CFG_MAX 512

struct usb_dev {
    int      present;
    int      speed;           /* USB_SPEED_* */
    int      addr;
    int      maxp0;
    uint16_t vid, pid;
    uint8_t  dev_desc[18];
    uint8_t  cfg_desc[USB_CFG_MAX];
    uint16_t cfg_len;
    char     manufacturer[48], product[48], serial[48];
};

struct usb_host_info {
    uint16_t ehci_version;
    uint32_t ehci_hcsparams, ehci_hccparams;
    uint32_t ohci_revision, ohci_rhdesca;
    uint8_t  phy_ok;          /* PHY patch read back correctly */
};

int  usb_host_init(void);                /* 0 ok, -1 controller not responding */
void usb_vbus(int on);                   /* 5 V on the port */
int  usb_poll(void);                     /* returns USB_EV_*; does enumeration when a device appears */
const struct usb_dev *usb_dev(void);
const struct usb_host_info *usb_info(void);

/* control transfer on the attached device; returns bytes transferred (IN)
 * or 0 (OUT/no data), negative on error */
int  usb_control(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                 uint16_t wIndex, uint16_t wLength, void *data);

/* bulk transfer on endpoint ep (bit 7 = IN); any length, split into
 * controller-sized pieces.  Returns bytes transferred (a short IN packet
 * ends the transfer early), -2 on STALL (call usb_clear_halt), -3 on
 * timeout, other negatives on error. */
#define USB_BULK_MAX 65536            /* bytes per controller transfer (EHCI: 4 chained qTDs) */
int  usb_bulk(uint8_t ep, uint16_t maxp, void *data, uint32_t len, uint32_t timeout_ms);
int  usb_clear_halt(uint8_t ep);

struct usb_bulk_stats { uint32_t transfers, bytes, us_xfer, us_copy; };
extern struct usb_bulk_stats usb_bulk_stats;   /* where bulk time goes: on the bus vs copying */
extern int usb_zero_copy;                       /* 1 = DMA straight into caller buffers (default) */

/* interrupt IN endpoint (full/low-speed devices, polled every 1 ms frame).
 * usb_intr_poll returns the number of bytes received (and re-arms the
 * transfer), 0 if the device had nothing to send yet, negative on error. */
int  usb_intr_open(uint8_t ep, uint16_t maxp);
int  usb_intr_poll(void *buf, uint16_t max);
void usb_intr_close(void);
void usb_intr_debug(void);              /* dump ED/TD/HCCA state */

/* first endpoint of the given transfer type (2 = bulk, 3 = interrupt) and
 * direction on an interface of class cls */
struct usb_ep_info {
    uint8_t  ep, iface, subclass, protocol, interval;
    uint16_t maxp;
};
int  usb_find_endpoint(uint8_t cls, uint8_t type, int in, struct usb_ep_info *out);

/* helpers */
const char *usb_class_name(uint8_t cls);
const char *usb_speed_name(int speed);
void usb_print_dev(const struct usb_dev *d);     /* descriptors in readable form */
void usb_print_ports(void);                      /* one line with root port state */
void usb_print_controller(void);                 /* OHCI status, frame counter, port change bits */

struct usb_error_counts {
    uint32_t port_disabled;                      /* controller disabled the port while a device was up */
};
extern struct usb_error_counts usb_errors;

extern int usb_debug;                    /* 1 = log transfers and port events */

#endif
