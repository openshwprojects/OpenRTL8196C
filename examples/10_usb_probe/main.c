/*
 * 10_usb_probe - bring up the USB 2.0 (EHCI + OHCI) host and enumerate
 * whatever device is plugged into the router's USB port.
 *
 * Hardware facts discovered from the LT-6408n stock kernel disassembly:
 *   1. GPIO PA4 (bit 4 of 0xB8003500/08/0C) is `usb_pwr`: must be driven
 *      OUTPUT HIGH (1) to enable the 5V VBUS switch on the USB port.
 *   2. GPIO PC3 (bit 19) is the active-low USB status LED.
 *   3. System clock bit 17 of 0xB8000010 enables the USB host block;
 *      the Synopsys PHY is patched via 0xB8000034 + 0xB80210A4.
 *   4. Both EHCI (0xB8021000) and OHCI (0xB8020000) are little-endian on
 *      this big-endian SoC, so all register & descriptor words are byte-swapped.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "ota.h"
#include "libc.h"

#define SYS_CLK_MGR      0xB8000010u
#define SYS_USB_SIE      0xB8000034u
#define EHCI_BASE        0xB8021000u
#define OHCI_BASE        0xB8020000u
#define USB_PHY_INDIRECT 0xB80210A4u

#define GPIO_USB_PWR     GPIO_PIN('A', 4)
#define GPIO_USB_LED     GPIO_PIN('C', 3)

static inline uint32_t bswap(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
static inline uint32_t erd(uint32_t a) { return bswap(REG32(a)); }
static inline void ewr(uint32_t a, uint32_t v) { REG32(a) = bswap(v); }
#define PHYS(p) ((uint32_t)(p) & 0x1FFFFFFFu)

static uint32_t op;
#define USBCMD      (op + 0x00)
#define USBSTS      (op + 0x04)
#define USBINTR     (op + 0x08)
#define FRINDEX     (op + 0x0C)
#define PERIODIC    (op + 0x14)
#define ASYNCLIST   (op + 0x18)
#define CONFIGFLAG  (op + 0x40)
#define PORTSC      (op + 0x44)

/* OHCI registers (offsets from OHCI_BASE = 0xB8020000) */
#define OHCI_REVISION       (OHCI_BASE + 0x00)
#define OHCI_CONTROL        (OHCI_BASE + 0x04)
#define OHCI_CMDSTATUS      (OHCI_BASE + 0x08)
#define OHCI_INTSTATUS      (OHCI_BASE + 0x0C)
#define OHCI_INTDISABLE     (OHCI_BASE + 0x14)
#define OHCI_HCCA           (OHCI_BASE + 0x18)
#define OHCI_CTRL_HEAD_ED   (OHCI_BASE + 0x20)
#define OHCI_CTRL_CURR_ED   (OHCI_BASE + 0x24)
#define OHCI_FMINTERVAL     (OHCI_BASE + 0x34)
#define OHCI_RHDESCA        (OHCI_BASE + 0x48)
#define OHCI_RHSTATUS       (OHCI_BASE + 0x50)
#define OHCI_RHPORT1        (OHCI_BASE + 0x54)

/* ------------------------------------------------------------ PHY patch */
static void phy_strobe(uint8_t reg_l, uint8_t reg_h)
{
    REG32(USB_PHY_INDIRECT) = 0x00300000 | ((uint32_t)reg_l << 16); delay_ms(5);
    REG32(USB_PHY_INDIRECT) = 0x00200000 | ((uint32_t)reg_l << 16); delay_ms(5);
    REG32(USB_PHY_INDIRECT) = 0x00300000 | ((uint32_t)reg_l << 16); delay_ms(5);
    REG32(USB_PHY_INDIRECT) = 0x00300000 | ((uint32_t)reg_h << 16); delay_ms(5);
    REG32(USB_PHY_INDIRECT) = 0x00200000 | ((uint32_t)reg_h << 16); delay_ms(5);
    REG32(USB_PHY_INDIRECT) = 0x00300000 | ((uint32_t)reg_h << 16); delay_ms(5);
}

static void phy_set(uint8_t reg, uint8_t val)
{
    REG32(SYS_USB_SIE) = 0x1F00 | val;
    delay_ms(20);
    phy_strobe(reg & 0x0F, (reg & 0xF0) >> 4);
}

static uint8_t phy_get(uint8_t reg)
{
    phy_strobe(reg & 0x0F, (uint8_t)(((reg & 0xF0) >> 4) - 2));
    return (uint8_t)(REG32(USB_PHY_INDIRECT) >> 24);
}

static void dump_phy_regs(const char *tag)
{
    printf("phy [%s] E0..E7:", tag);
    for (uint8_t r = 0xE0; r <= 0xE7; r++)
        printf(" %02x:%02x", r, phy_get(r));
    printf(" | F0..F6:");
    for (uint8_t r = 0xF0; r <= 0xF6; r++)
        printf(" %02x:%02x", r, phy_get(r));
    printf("\n");
}

static void phy_patch(void)
{
    static const uint8_t regs[] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe5 };
    static const uint8_t vals[] = { 0x99, 0xa8, 0x98, 0xc1, 0x91 };
    dump_phy_regs("before");
    phy_set(0xf4, 0xe3);
    for (unsigned i = 0; i < sizeof regs; i++) {
        phy_set(regs[i], vals[i]);
        uint8_t rb = phy_get(regs[i]);
        printf("phy %02x = %02x -> reads %02x %s\n", regs[i], vals[i], rb, rb == vals[i] ? "ok" : "MISMATCH");
    }
    /* Realtek calibration pulse from ehci-rtl8652.c */
    uint8_t e3 = phy_get(0xe3);
    phy_set(0xe3, e3 & ~(1u << 6));
    phy_set(0xe3, e3 | (1u << 6));
    uint8_t e1 = phy_get(0xe1);
    phy_set(0xe1, e1 & ~(1u << 7));
    phy_set(0xe1, e1 | (1u << 7));
    dump_phy_regs("after");
}

/* --------------------------------------------------- EHCI control transfer */
static uint32_t qh[16]     __attribute__((aligned(64)));
static uint32_t qtd[3][8]  __attribute__((aligned(32)));
static uint8_t  xfer_buf[1024] __attribute__((aligned(64)));
static uint8_t  setup_pkt[8]   __attribute__((aligned(32)));

#define QTD_ACTIVE    (1u << 7)
#define QTD_HALTED    (1u << 6)
#define QTD_PID_OUT   (0u << 8)
#define QTD_PID_IN    (1u << 8)
#define QTD_PID_SETUP (2u << 8)
#define QTD_CERR      (3u << 10)
#define QTD_IOC       (1u << 15)

static void qtd_fill(uint32_t *q, uint32_t next_phys, uint32_t pid, const void *buf, uint32_t len, int toggle)
{
    uint32_t b = PHYS(buf);
    q[0] = bswap(next_phys ? next_phys : 1u);
    q[1] = bswap(1u);
    q[2] = bswap(QTD_ACTIVE | pid | QTD_CERR | (len << 16) | ((uint32_t)toggle << 31));
    q[3] = bswap(b);
    q[4] = bswap((b & ~0xFFFu) + 0x1000);
    q[5] = bswap((b & ~0xFFFu) + 0x2000);
    q[6] = 0; q[7] = 0;
}

static int ehci_control_xfer(int addr, int speed, int maxp, uint8_t bmReq, uint8_t bReq,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLen, uint8_t *data)
{
    setup_pkt[0] = bmReq; setup_pkt[1] = bReq;
    setup_pkt[2] = (uint8_t)wValue; setup_pkt[3] = (uint8_t)(wValue >> 8);
    setup_pkt[4] = (uint8_t)wIndex; setup_pkt[5] = (uint8_t)(wIndex >> 8);
    setup_pkt[6] = (uint8_t)wLen;   setup_pkt[7] = (uint8_t)(wLen >> 8);

    int in = (bmReq & 0x80) != 0;
    int n = 0;
    if (wLen) {
        qtd_fill(qtd[0], PHYS(qtd[1]), QTD_PID_SETUP, setup_pkt, 8, 0);
        qtd_fill(qtd[1], PHYS(qtd[2]), in ? QTD_PID_IN : QTD_PID_OUT, data, wLen, 1);
        qtd_fill(qtd[2], 0, in ? QTD_PID_OUT : QTD_PID_IN, data, 0, 1);
        n = 3;
    } else {
        qtd_fill(qtd[0], PHYS(qtd[1]), QTD_PID_SETUP, setup_pkt, 8, 0);
        qtd_fill(qtd[1], 0, QTD_PID_IN, data, 0, 1);
        n = 2;
    }
    qtd[n - 1][2] |= bswap(QTD_IOC);

    memset(qh, 0, sizeof qh);
    qh[0] = bswap(PHYS(qh) | 2u);
    qh[1] = bswap((uint32_t)addr | ((uint32_t)speed << 12) | (1u << 14) |
                  (1u << 15) | ((uint32_t)maxp << 16));
    qh[2] = bswap(1u << 30);
    qh[3] = 0;
    qh[4] = bswap(PHYS(qtd[0]));
    qh[5] = bswap(1u);
    qh[6] = 0;

    ewr(ASYNCLIST, PHYS(qh));
    ewr(USBCMD, erd(USBCMD) | (1u << 5));

    int rc = -1;
    for (int t = 0; t < 500; t++) {
        uint32_t tok = bswap(qtd[n - 1][2]);
        if (!(tok & QTD_ACTIVE)) {
            if (tok & QTD_HALTED) { rc = -2; break; }
            if (wLen) {
                uint32_t t1 = bswap(qtd[1][2]);
                if (t1 & QTD_HALTED) { rc = -3; break; }
                rc = (int)wLen - (int)((t1 >> 16) & 0x7FFF);
            } else {
                rc = 0;
            }
            break;
        }
        delay_ms(1);
    }
    ewr(USBCMD, erd(USBCMD) & ~(1u << 5));
    delay_ms(2);
    if (rc < 0)
        printf("  ehci control transfer failed rc=%d tokens %08x %08x %08x sts=%08x\n", rc,
               bswap(qtd[0][2]), bswap(qtd[1][2]), bswap(qtd[2][2]), erd(USBSTS));
    return rc;
}

/* --------------------------------------------------- OHCI control transfer */
static uint8_t  ohci_hcca[256] __attribute__((aligned(256)));
static uint32_t ohci_ed[4]     __attribute__((aligned(32)));
static uint32_t ohci_td[4][4]  __attribute__((aligned(32)));

#define OHCI_PID_SETUP 0u
#define OHCI_PID_OUT   1u
#define OHCI_PID_IN    2u

static void ohci_td_fill(uint32_t *td, uint32_t pid, const void *buf, uint32_t len, int toggle, uint32_t next_phys)
{
    uint32_t ctrl = (1u << 18) /* buffer rounding */
                  | (pid << 19)
                  | (7u << 21) /* DI = no interrupt */
                  | ((2u | (uint32_t)toggle) << 24)
                  | (0xFu << 28); /* CC = NotAccessed */
    uint32_t b = len ? PHYS(buf) : 0;
    uint32_t be = len ? (b + len - 1) : 0;
    td[0] = bswap(ctrl);
    td[1] = bswap(b);
    td[2] = bswap(next_phys);
    td[3] = bswap(be);
}

static int ohci_control_xfer(int addr, int lowspeed, int maxp, uint8_t bmReq, uint8_t bReq,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLen, uint8_t *data)
{
    setup_pkt[0] = bmReq; setup_pkt[1] = bReq;
    setup_pkt[2] = (uint8_t)wValue; setup_pkt[3] = (uint8_t)(wValue >> 8);
    setup_pkt[4] = (uint8_t)wIndex; setup_pkt[5] = (uint8_t)(wIndex >> 8);
    setup_pkt[6] = (uint8_t)wLen;   setup_pkt[7] = (uint8_t)(wLen >> 8);

    memset(ohci_td, 0, sizeof(ohci_td));
    int in = (bmReq & 0x80) != 0;
    int tail_idx;
    if (wLen) {
        ohci_td_fill(ohci_td[0], OHCI_PID_SETUP, setup_pkt, 8, 0, PHYS(ohci_td[1]));
        ohci_td_fill(ohci_td[1], in ? OHCI_PID_IN : OHCI_PID_OUT, data, wLen, 1, PHYS(ohci_td[2]));
        ohci_td_fill(ohci_td[2], in ? OHCI_PID_OUT : OHCI_PID_IN, 0, 0, 1, PHYS(ohci_td[3]));
        tail_idx = 3;
    } else {
        ohci_td_fill(ohci_td[0], OHCI_PID_SETUP, setup_pkt, 8, 0, PHYS(ohci_td[1]));
        ohci_td_fill(ohci_td[1], OHCI_PID_IN, 0, 0, 1, PHYS(ohci_td[2]));
        tail_idx = 2;
    }

    uint32_t tail_phys = PHYS(ohci_td[tail_idx]);
    ohci_ed[0] = bswap((uint32_t)addr | ((uint32_t)lowspeed << 13) | ((uint32_t)maxp << 16));
    ohci_ed[1] = bswap(tail_phys);
    ohci_ed[2] = bswap(PHYS(ohci_td[0]));
    ohci_ed[3] = 0;

    ewr(OHCI_CTRL_HEAD_ED, PHYS(ohci_ed));
    ewr(OHCI_CTRL_CURR_ED, 0);
    ewr(OHCI_CONTROL, erd(OHCI_CONTROL) | (1u << 4)); /* CLE */
    ewr(OHCI_CMDSTATUS, 1u << 1);                     /* CLF */

    int rc = -1;
    for (int t = 0; t < 500; t++) {
        uint32_t headp = bswap(ohci_ed[2]);
        if (headp & 1u) { /* Halted */
            rc = -2;
            break;
        }
        if ((headp & ~0xFu) == tail_phys) {
            if (wLen) {
                uint32_t cbp = bswap(ohci_td[1][1]);
                if (cbp == 0)
                    rc = (int)wLen;
                else
                    rc = (int)(cbp - PHYS(data));
            } else {
                rc = 0;
            }
            break;
        }
        delay_ms(1);
    }
    ewr(OHCI_CONTROL, erd(OHCI_CONTROL) & ~(1u << 4));
    delay_ms(2);
    if (rc < 0) {
        printf("  ohci control transfer failed rc=%d ed2=%08x td0=%08x td1=%08x td2=%08x\n",
               rc, bswap(ohci_ed[2]), bswap(ohci_td[0][0]), bswap(ohci_td[1][0]), bswap(ohci_td[2][0]));
    }
    return rc;
}

/* ------------------------------------------- Shared USB descriptor printer */
typedef int (*ctrl_xfer_fn)(int addr, int speed, int maxp, uint8_t bmReq, uint8_t bReq,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLen, uint8_t *data);

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0: return "per-interface";
    case 1: return "audio";
    case 2: return "comm/CDC";
    case 3: return "HID";
    case 6: return "image";
    case 7: return "printer";
    case 8: return "mass storage";
    case 9: return "hub";
    case 0x0A: return "CDC data";
    case 0xE0: return "wireless";
    case 0xEF: return "misc";
    case 0xFF: return "vendor-specific";
    default: return "?";
    }
}

static void print_string_desc(ctrl_xfer_fn xfer, int addr, int speed, int maxp, uint8_t idx, const char *what)
{
    if (!idx) return;
    int n = xfer(addr, speed, maxp, 0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, 255, xfer_buf);
    if (n < 2) return;
    printf("  %s: ", what);
    for (int i = 2; i + 1 < n && i < xfer_buf[0]; i += 2)
        uart_putc(xfer_buf[i + 1] ? '?' : (char)xfer_buf[i]);
    printf("\n");
}

static void enumerate_device(ctrl_xfer_fn xfer, int speed, const char *bus_label)
{
    int addr = 0, maxp = 8;
    memset(xfer_buf, 0, sizeof(xfer_buf));
    int n = xfer(addr, speed, maxp, 0x80, 6, 0x0100, 0, 8, xfer_buf);
    if (n < 8) {
        printf("[%s] get_descriptor(8) failed (n=%d)\n", bus_label, n);
        return;
    }
    maxp = xfer_buf[7];
    if (maxp == 0) maxp = 8;
    printf("[%s] bMaxPacketSize0=%d\n", bus_label, maxp);

    if (xfer(addr, speed, maxp, 0x00, 5, 1, 0, 0, 0) < 0) {
        printf("[%s] set_address(1) failed\n", bus_label);
        return;
    }
    delay_ms(15);
    addr = 1;

    n = xfer(addr, speed, maxp, 0x80, 6, 0x0100, 0, 18, xfer_buf);
    if (n < 18) {
        printf("[%s] get_descriptor(18) failed (n=%d)\n", bus_label, n);
        return;
    }
    uint16_t vid = xfer_buf[8] | (xfer_buf[9] << 8);
    uint16_t pid = xfer_buf[10] | (xfer_buf[11] << 8);
    uint16_t bcd_dev = xfer_buf[12] | (xfer_buf[13] << 8);
    uint8_t dev_class = xfer_buf[4], dev_sub = xfer_buf[5], dev_proto = xfer_buf[6];
    uint8_t mfg_idx = xfer_buf[14], prod_idx = xfer_buf[15], ser_idx = xfer_buf[16];
    uint8_t num_cfg = xfer_buf[17];

    printf("device: USB %x.%02x  VID %04x PID %04x  rev %x.%02x  class %02x/%02x/%02x (%s)  configs %u\n",
           xfer_buf[3], xfer_buf[2], vid, pid, bcd_dev >> 8, bcd_dev & 0xFF,
           dev_class, dev_sub, dev_proto, class_name(dev_class), num_cfg);

    print_string_desc(xfer, addr, speed, maxp, mfg_idx,  "manufacturer");
    print_string_desc(xfer, addr, speed, maxp, prod_idx, "product");
    print_string_desc(xfer, addr, speed, maxp, ser_idx,  "serial");

    n = xfer(addr, speed, maxp, 0x80, 6, 0x0200, 0, 9, xfer_buf);
    if (n < 9) {
        printf("config descriptor header failed\n");
        return;
    }
    uint16_t total = xfer_buf[2] | (xfer_buf[3] << 8);
    if (total > sizeof xfer_buf) total = sizeof xfer_buf;
    n = xfer(addr, speed, maxp, 0x80, 6, 0x0200, 0, total, xfer_buf);
    if (n < 9) {
        printf("full config descriptor failed\n");
        return;
    }
    printf("config: %u bytes, %u interfaces, %s, %u mA\n", n, xfer_buf[4],
           (xfer_buf[7] & 0x40) ? "self-powered" : "bus-powered", xfer_buf[8] * 2);
    for (int i = 0; i + 1 < n; i += xfer_buf[i] ? xfer_buf[i] : 1) {
        uint8_t len = xfer_buf[i], type = xfer_buf[i + 1];
        if (type == 4 && len >= 9)
            printf("  interface %u (alt %u): class %02x/%02x/%02x (%s), %u endpoints\n",
                   xfer_buf[i + 2], xfer_buf[i + 3],
                   xfer_buf[i + 5], xfer_buf[i + 6], xfer_buf[i + 7],
                   class_name(xfer_buf[i + 5]), xfer_buf[i + 4]);
        else if (type == 5 && len >= 7)
            printf("    endpoint %02x (%s) %s maxp %u interval %u\n",
                   xfer_buf[i + 2], (xfer_buf[i + 2] & 0x80) ? "IN" : "OUT",
                   (const char *[]){ "control", "iso", "bulk", "interrupt" }[xfer_buf[i + 3] & 3],
                   xfer_buf[i + 4] | (xfer_buf[i + 5] << 8), xfer_buf[i + 6]);
    }
}

static uint32_t ehci_periodic[1024] __attribute__((aligned(4096)));

static void ohci_setup_controller(void)
{
    memset(ohci_hcca, 0, sizeof(ohci_hcca));
    ewr(OHCI_INTDISABLE, 0x80000000u);
    ewr(OHCI_CONTROL, 0);
    delay_ms(50);
    ewr(OHCI_CMDSTATUS, 1u); /* HCR */
    for (int i = 0; i < 50 && (erd(OHCI_CMDSTATUS) & 1u); i++)
        delay_ms(1);
    ewr(OHCI_CTRL_HEAD_ED, 0);
    ewr(OHCI_CTRL_CURR_ED, 0);
    ewr(OHCI_HCCA, PHYS(ohci_hcca));
    ewr(OHCI_FMINTERVAL, 0xA7782EDFu);
    ewr(OHCI_BASE + 0x40, 0x00002A2Fu); /* HcPeriodicStart */
    ewr(OHCI_BASE + 0x44, 0x00000628u); /* HcLSThreshold */
    ewr(OHCI_CONTROL, 0x83u);           /* USB_OPERATIONAL | CBSR=3 */
    uint32_t rh_a = erd(OHCI_RHDESCA);
    rh_a = (rh_a & ~0x900u) | 0x200u;   /* clear PSM/OCPM, set NPS (Always Powered) */
    ewr(OHCI_RHDESCA, rh_a);
    ewr(OHCI_BASE + 0x4C, 0);           /* HcRhDescriptorB = 0 */
    ewr(OHCI_RHSTATUS, 0x00010000u);    /* SetGlobalPower (LPSC) */
    ewr(OHCI_RHPORT1, 0x00000100u);     /* SetPortPower */
}

static void ohci_init_and_enumerate(void)
{
    ohci_setup_controller();
    delay_ms(300);

    uint32_t ps = erd(OHCI_RHPORT1);
    printf("[ohci] HcRevision=%08x HcControl=%08x HcRhDescA=%08x HcRhPort1=%08x HcRhPort2=%08x\n",
           erd(OHCI_REVISION), erd(OHCI_CONTROL), erd(OHCI_RHDESCA), ps, erd(OHCI_BASE + 0x58));
    if (!(ps & 1u)) {
        printf("[ohci] No device connected on OHCI root port.\n");
        return;
    }

    ewr(OHCI_RHPORT1, 1u << 4); /* SetPortReset */
    for (int i = 0; i < 100; i++) {
        delay_ms(2);
        if (!(erd(OHCI_RHPORT1) & (1u << 4)))
            break;
    }
    ewr(OHCI_RHPORT1, 1u << 20); /* clear PRSC */
    delay_ms(30);

    ps = erd(OHCI_RHPORT1);
    int lowspeed = (ps >> 9) & 1;
    printf("[ohci] After reset: HcRhPortStatus1=%08x (%s-SPEED, PES=%u)\n",
           ps, lowspeed ? "LOW" : "FULL", (ps >> 1) & 1);
    enumerate_device(ohci_control_xfer, lowspeed, lowspeed ? "OHCI-LS" : "OHCI-FS");
}

static void dump_portsc(const char *tag)
{
    uint32_t p = erd(PORTSC);
    printf("%s PORTSC=%08x CCS=%u CSC=%u PE=%u PEC=%u OCA=%u FPR=%u SUSP=%u PR=%u LS=%u PP=%u PO=%u\n",
           tag, p, p & 1, (p >> 1) & 1, (p >> 2) & 1, (p >> 3) & 1, (p >> 4) & 1, (p >> 6) & 1,
           (p >> 7) & 1, (p >> 8) & 1, (p >> 10) & 3, (p >> 12) & 1, (p >> 13) & 1);
}

static void dump_block(const char *name, uint32_t base, int nwords, int swap)
{
    printf("=== %s (%08x) ===\n", name, base);
    for (int i = 0; i < nwords; i++) {
        if (i % 4 == 0) printf("  %08x:", base + 4 * i);
        uint32_t v = REG32(base + 4 * i);
        printf(" %08x", swap ? bswap(v) : v);
        if (i % 4 == 3 || i == nwords - 1) printf("\n");
    }
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb probe v7 (full register & PHY diagnostics)\n");
    printf("initial: PIN_MUX=%08x PIN_MUX2=%08x SIE=%08x CLK=%08x CNR=%08x DIR=%08x DAT=%08x\n",
           REG32(PIN_MUX_SEL), REG32(SYS_BASE + 0x44), REG32(SYS_USB_SIE), REG32(SYS_CLK_MGR),
           REG32(GPIO_PABCD_CNR), REG32(GPIO_PABCD_DIR), REG32(GPIO_PABCD_DAT));

    uint32_t orig_sie = REG32(SYS_USB_SIE);

    /* Exact stock kernel rtl_gpio_init (0x8035aac8) + usb_init (0x8035d998) */
    REG32(PIN_MUX_SEL)     = 0x00343C00u;
    REG32(SYS_BASE + 0x44) = 0x00E00000u;
    REG32(GPIO_PABCD_CNR)  = 0xFFF6FF83u;
    REG32(GPIO_PABCD_DIR)  = (1u << 2) | (1u << 4) | (1u << 6) | (1u << 16) | (1u << 19);
    REG32(GPIO_PABCD_DAT) |= (1u << 4) | (1u << 19);

    REG32(SYS_CLK_MGR) |= 1u << 17;
    delay_ms(100);
    uint32_t cap0 = REG32(EHCI_BASE);
    if (cap0 == 0) {
        printf("EHCI still reads zero: host block not clocked, giving up\n");
        REG32(WDTCNR) = WDT_BITE_NOW;
        for (;;) ;
    }
    uint32_t caplen = cap0 >> 24;
    op = EHCI_BASE + caplen;

    dump_block("EHCI pre-init (LE)", EHCI_BASE, 46, 1);
    dump_block("OHCI pre-init (LE)", OHCI_BASE, 24, 1);

    /* 1. Patch USB 2.0 PHY first (before ehci_reset, matching ehci_hcd_rtl8652_init) */
    phy_patch();
    /* Restore upper bits of SYS_USB_SIE (0x000A1F00) if cleared by phy_set */
    REG32(SYS_USB_SIE) = (orig_sie & 0xFFFF0000u) | (REG32(SYS_USB_SIE) & 0xFFFFu);
    printf("after phy_patch: SYS_USB_SIE=%08x\n", REG32(SYS_USB_SIE));

    /* 2. Initialize EHCI (matching ehci_rtl8652_reset + ehci_run) */
    for (int i = 0; i < 1024; i++)
        ehci_periodic[i] = bswap(1u);
    memset(qh, 0, sizeof(qh));
    qh[0] = bswap(PHYS(qh) | 2u);
    qh[1] = bswap(1u << 15);
    qh[4] = bswap(1u);
    qh[5] = bswap(1u);

    ewr(USBCMD, erd(USBCMD) & ~1u);
    delay_ms(10);
    ewr(USBCMD, erd(USBCMD) | 2u);
    for (int i = 0; i < 100 && (erd(USBCMD) & 2u); i++)
        delay_ms(1);
    REG32(op + 0x210) = 0x01fd0020u;

    ewr(USBCMD, erd(USBCMD) | 2u);
    for (int i = 0; i < 100 && (erd(USBCMD) & 2u); i++)
        delay_ms(1);
    REG32(op + 0x210) = 0x01fd0020u;

    ewr(PERIODIC, PHYS(ehci_periodic));
    ewr(ASYNCLIST, PHYS(qh));
    ewr(op + 0x10, 0); /* CTRLDSSEGMENT */
    ewr(USBCMD, 0x00080901u);
    ewr(CONFIGFLAG, 1u);
    (void)erd(USBCMD);
    delay_ms(10);
    ewr(USBINTR, 0);
    ewr(PORTSC, erd(PORTSC) | (1u << 12)); /* Port Power */
    delay_ms(50);

    /* 3. Initialize OHCI companion AFTER EHCI (matching kernel initcall order) */
    ohci_setup_controller();
    delay_ms(300);

    dump_block("EHCI post-init (LE)", EHCI_BASE, 46, 1);
    dump_block("OHCI post-init (LE)", OHCI_BASE, 24, 1);

    int found = 0;
    for (int round = 0; round < 10; round++) {
        uint32_t p = erd(PORTSC);
        uint32_t op1 = erd(OHCI_RHPORT1);
        if (!(p & 1u) && !(op1 & 1u)) {
            printf("[round %d] EHCI PORTSC=%08x OHCI PORT1=%08x USBSTS=%08x FRINDEX=%08x\n",
                   round, p, op1, erd(USBSTS), erd(FRINDEX));
            if (round == 3) {
                printf("Setting CONFIGFLAG=0 + PO=1 (handing root port to OHCI companion)...\n");
                ewr(PORTSC, (p & ~0x2Au) | (1u << 13));
                ewr(CONFIGFLAG, 0);
            } else if (round == 6) {
                printf("Restoring CONFIGFLAG=1 (returning root port to EHCI)...\n");
                ewr(CONFIGFLAG, 1u);
                ewr(PORTSC, erd(PORTSC) | (1u << 12));
            }
            delay_ms(400);
            continue;
        }
        found = 1;
        gpio_write(GPIO_USB_LED, 0);
        if (!(p & 1u) && (op1 & 1u)) {
            printf("=> Device detected on OHCI (PORT1=%08x)!\n", op1);
            ohci_init_and_enumerate();
            break;
        }
        if (((p >> 10) & 3) == 1) {
            printf("device present, line state K: LOW-SPEED device -> handing to OHCI\n");
            ewr(PORTSC, (p & ~0x2Au) | (1u << 13));
            delay_ms(100);
            dump_portsc("handed over");
            ohci_init_and_enumerate();
            break;
        }
        printf("device present on EHCI (PORTSC=%08x), resetting port\n", p);
        ewr(PORTSC, (p & ~0x2Au & ~(1u << 2)) | (1u << 8));
        delay_ms(60);
        ewr(PORTSC, erd(PORTSC) & ~0x2Au & ~(1u << 8));
        for (int i = 0; i < 20 && (erd(PORTSC) & (1u << 8)); i++)
            delay_ms(1);
        delay_ms(20);
        dump_portsc("after reset");
        p = erd(PORTSC);
        if (p & (1u << 2)) {
            printf("=> HIGH-SPEED device enabled, reading descriptors\n");
            enumerate_device(ehci_control_xfer, 2, "EHCI-HS");
        } else {
            printf("=> FULL-SPEED device: handing over to OHCI companion\n");
            ewr(PORTSC, (p & ~0x2Au) | (1u << 13));
            delay_ms(100);
            dump_portsc("handed over");
            ohci_init_and_enumerate();
        }
        break;
    }
    (void)found;
    printf("done, rebooting via hardware watchdog...\n");
    ewr(USBCMD, 0);
    ewr(OHCI_CONTROL, 0);
    delay_ms(50);
    REG32(WDTCNR) = WDT_BITE_NOW;
    for (;;)
        ;
    return 0;
}
