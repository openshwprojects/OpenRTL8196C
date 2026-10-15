/*
 * usb.c - USB host for the RTL8196C, see usb.h
 *
 * DMA structures and buffers are accessed through their KSEG1 (uncached)
 * alias so the driver works whether the program is linked at 0xA05xxxxx
 * or in cached KSEG0.  Controller registers and all descriptor words are
 * little-endian: RD()/WR() and LE32() swap.
 */
#include "rtl8196c.h"
#include "usb.h"
#include "timer.h"
#include "gpio.h"
#include "uart.h"
#include "libc.h"
#include "irq.h"                                   /* cctl_write: D-cache invalidate */

#define SYS_CLK_MGR   0xB8000010u
#define SYS_USB_SIE   0xB8000034u
#define EHCI_BASE     0xB8021000u
#define OHCI_BASE     0xB8020000u
#define USB_PHY_IND   0xB80210A4u

int usb_debug;
#define DBG(...) do { if (usb_debug) printf(__VA_ARGS__); } while (0)

static inline uint32_t sw32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
#define LE32(v)  sw32(v)
#define RD(a)    sw32(REG32(a))
#define WR(a, v) (REG32(a) = sw32(v))
#define PHYS(p)  ((uint32_t)(p) & 0x1FFFFFFFu)
#define UNC(p)   ((void *)(PHYS(p) | 0xA0000000u))

/* --------------------------------------------------------- DMA memory */
#define XFER_MAX 1024
#define EHCI_BULK_QTDS (USB_BULK_MAX / 16384)            /* one qTD per 16 KB page group */

struct dma_area {
    uint8_t  hcca[256];                    /* OHCI, 256-byte aligned */
    uint32_t qh[16];                       /* EHCI queue head, 64-byte aligned */
    uint32_t qtd[3][8];                    /* EHCI qTDs, 32-byte aligned */
    uint32_t ed[4];                        /* OHCI endpoint descriptor */
    uint32_t td[4][4];                     /* OHCI transfer descriptors */
    uint8_t  setup[8] __attribute__((aligned(16)));
    uint8_t  buf[XFER_MAX] __attribute__((aligned(64)));
    uint32_t ied[4] __attribute__((aligned(16)));    /* OHCI interrupt ED */
    uint32_t itd[2][4] __attribute__((aligned(16))); /* active TD + dummy tail, alternating */
    uint8_t  ibuf[2][64] __attribute__((aligned(16)));
    uint32_t bqh[16] __attribute__((aligned(64)));   /* EHCI bulk queue head */
    uint32_t bqtd[EHCI_BULK_QTDS][8] __attribute__((aligned(32))); /* chained bulk qTDs, 16 KB each */
    uint32_t bed[4] __attribute__((aligned(16)));    /* OHCI bulk ED */
    uint32_t btd[2][4] __attribute__((aligned(16))); /* OHCI bulk TD + dummy tail */
    uint8_t  bbuf[512] __attribute__((aligned(64)));  /* bounce buffer for small/unaligned bulk pieces */
};
static struct dma_area dma_mem __attribute__((aligned(4096)));
static struct dma_area *D;                 /* uncached alias of dma_mem */

static uint32_t op;                        /* EHCI operational base */
#define USBCMD      (op + 0x00)
#define USBSTS      (op + 0x04)
#define USBINTR     (op + 0x08)
#define FRINDEX     (op + 0x0C)
#define CTRLDSSEG   (op + 0x10)
#define PERIODICLB  (op + 0x14)
#define ASYNCLB     (op + 0x18)
#define CONFIGFLAG  (op + 0x40)
#define PORTSC      (op + 0x44)

#define PS_CCS   (1u << 0)
#define PS_CSC   (1u << 1)
#define PS_PE    (1u << 2)
#define PS_PEC   (1u << 3)
#define PS_OCC   (1u << 5)
#define PS_PR    (1u << 8)
#define PS_PP    (1u << 12)
#define PS_PO    (1u << 13)
#define PS_W1C   (PS_CSC | PS_PEC | PS_OCC)
#define PS_LS(p) (((p) >> 10) & 3u)

#define OHCI_REVISION   (OHCI_BASE + 0x00)
#define OHCI_CONTROL    (OHCI_BASE + 0x04)
#define OHCI_CMDSTATUS  (OHCI_BASE + 0x08)
#define OHCI_INTSTATUS  (OHCI_BASE + 0x0C)
#define OHCI_INTDISABLE (OHCI_BASE + 0x14)
#define OHCI_HCCA       (OHCI_BASE + 0x18)
#define OHCI_CTRLHEAD   (OHCI_BASE + 0x20)
#define OHCI_CTRLCUR    (OHCI_BASE + 0x24)
#define OHCI_BULKHEAD   (OHCI_BASE + 0x28)
#define OHCI_BULKCUR    (OHCI_BASE + 0x2C)
#define OHCI_FMINTERVAL (OHCI_BASE + 0x34)
#define OHCI_PERSTART   (OHCI_BASE + 0x40)
#define OHCI_LSTHRESH   (OHCI_BASE + 0x44)
#define OHCI_RHDESCA    (OHCI_BASE + 0x48)
#define OHCI_RHDESCB    (OHCI_BASE + 0x4C)
#define OHCI_RHSTATUS   (OHCI_BASE + 0x50)
#define OHCI_RHPORT1    (OHCI_BASE + 0x54)

#define RH_CCS   (1u << 0)
#define RH_PES   (1u << 1)
#define RH_PRS   (1u << 4)
#define RH_PPS   (1u << 8)
#define RH_LSDA  (1u << 9)
#define RH_CSC   (1u << 16)
#define RH_PRSC  (1u << 20)

static struct usb_dev dev;
static struct usb_host_info info;
static int on_ohci;                        /* current device is on the companion */

struct usb_error_counts usb_errors;

static int intr_active;                    /* interrupt IN endpoint armed */
static int intr_cur;                       /* index of the TD the controller is working on */
static uint16_t intr_len;

/* ---------------------------------------------------------- PHY access */
static void phy_strobe(uint32_t lo, uint32_t hi)
{
    REG32(USB_PHY_IND) = 0x00300000u | (lo << 16); delay_ms(5);
    REG32(USB_PHY_IND) = 0x00200000u | (lo << 16); delay_ms(5);
    REG32(USB_PHY_IND) = 0x00300000u | (lo << 16); delay_ms(5);
    REG32(USB_PHY_IND) = 0x00300000u | (hi << 16); delay_ms(5);
    REG32(USB_PHY_IND) = 0x00200000u | (hi << 16); delay_ms(5);
    REG32(USB_PHY_IND) = 0x00300000u | (hi << 16); delay_ms(5);
}

/* exactly the stock SetUSBPhy: the data byte goes through 0xB8000034 */
static void phy_set(uint8_t reg, uint8_t val)
{
    REG32(SYS_USB_SIE) = 0x1F00u | val;
    delay_ms(5);
    delay_ms(100);
    phy_strobe(reg & 0x0Fu, (reg & 0xF0u) >> 4);
}

static uint8_t phy_get(uint8_t reg)
{
    phy_strobe(reg & 0x0Fu, ((reg & 0xF0u) >> 4) - 2);
    return (uint8_t)(REG32(USB_PHY_IND) >> 24);
}

static int phy_patch(void)
{
    static const uint8_t regs[] = { 0xE0, 0xE1, 0xE2, 0xE3, 0xE5 };
    static const uint8_t vals[] = { 0x99, 0xA8, 0x98, 0xC1, 0x91 };
    int ok = 1;
    phy_set(0xF4, 0xE3);                   /* disable host chirp J-K */
    for (unsigned i = 0; i < sizeof regs; i++) {
        phy_set(regs[i], vals[i]);
        uint8_t rb = phy_get(regs[i]);
        if (rb != vals[i]) ok = 0;
        DBG("usb: phy %02x=%02x reads %02x\n", regs[i], vals[i], rb);
    }
    return ok;
}

/* -------------------------------------------------------------- EHCI */
#define QTD_ACTIVE (1u << 7)
#define QTD_HALTED (1u << 6)
#define QTD_ERRMASK 0x7Cu                  /* halted, buffer err, babble, xact err, missed uframe */
#define PID_OUT    0u
#define PID_IN     1u
#define PID_SETUP  2u

static void qtd_fill(uint32_t *q, uint32_t next, uint32_t alt, uint32_t pid,
                     const void *buf, uint32_t len, int toggle, int ioc)
{
    uint32_t b = buf ? PHYS(buf) : 0;
    q[0] = LE32(next ? next : 1u);
    q[1] = LE32(alt ? alt : 1u);
    q[2] = LE32(QTD_ACTIVE | (pid << 8) | (3u << 10) | (ioc ? (1u << 15) : 0) |
                (len << 16) | ((uint32_t)toggle << 31));
    q[3] = LE32(b);                                 /* five page pointers: up to 16 KB + offset */
    q[4] = LE32((b & ~0xFFFu) + 0x1000);
    q[5] = LE32((b & ~0xFFFu) + 0x2000);
    q[6] = LE32((b & ~0xFFFu) + 0x3000);
    q[7] = LE32((b & ~0xFFFu) + 0x4000);
}

static int ehci_async(int on)
{
    uint32_t c = RD(USBCMD);
    c = on ? (c | (1u << 5)) : (c & ~(1u << 5));
    WR(USBCMD, c);
    for (int i = 0; i < 100; i++) {         /* wait for ASS to follow ASE */
        if (((RD(USBSTS) >> 15) & 1u) == (uint32_t)on) return 0;
        delay_us(100);
    }
    return -1;
}

static int ehci_control(int addr, int maxp, uint8_t *setup, int in, uint16_t len)
{
    uint32_t *q0 = D->qtd[0], *q1 = D->qtd[1], *q2 = D->qtd[2];
    uint32_t *status;
    if (len) {
        qtd_fill(q0, PHYS(q1), 0, PID_SETUP, setup, 8, 0, 0);
        qtd_fill(q1, PHYS(q2), in ? PHYS(q2) : 0, in ? PID_IN : PID_OUT, D->buf, len, 1, 0);
        qtd_fill(q2, 0, 0, in ? PID_OUT : PID_IN, 0, 0, 1, 1);
        status = q2;
    } else {
        qtd_fill(q0, PHYS(q1), 0, PID_SETUP, setup, 8, 0, 0);
        qtd_fill(q1, 0, 0, PID_IN, 0, 0, 1, 1);
        status = q1;
    }
    uint32_t *qh = D->qh;
    memset(qh, 0, 64);
    qh[0] = LE32(PHYS(qh) | 2u);                                    /* loop to itself, type QH */
    qh[1] = LE32((uint32_t)addr | (2u << 12) | (1u << 14) | (1u << 15) | ((uint32_t)maxp << 16));
    qh[2] = LE32(1u << 30);
    qh[4] = LE32(PHYS(q0));
    qh[5] = LE32(1u);
    WR(ASYNCLB, PHYS(qh));
    ehci_async(1);

    int rc = -1;
    for (int t = 0; t < 500; t++) {
        uint32_t s = LE32(status[2]);
        uint32_t d = len ? LE32(q1[2]) : 0;
        if ((LE32(q0[2]) | d | s) & QTD_HALTED) { rc = -2; break; }
        if (!(s & QTD_ACTIVE)) {
            rc = len ? (int)len - (int)((d >> 16) & 0x7FFFu) : 0;
            break;
        }
        delay_ms(1);
    }
    ehci_async(0);
    if (rc < 0)
        DBG("usb: ehci xfer rc=%d tok %08x %08x %08x sts %08x\n", rc,
            LE32(q0[2]), LE32(q1[2]), LE32(q2[2]), RD(USBSTS));
    return rc;
}

/* -------------------------------------------------------------- OHCI */
#define TD_CC(w)   ((w) >> 28)

static void td_fill(uint32_t *td, uint32_t pid, const void *buf, uint32_t len, int toggle, uint32_t next)
{
    uint32_t b = (len && buf) ? PHYS(buf) : 0;
    td[0] = LE32((1u << 18) | (pid << 19) | (7u << 21) | ((2u | (uint32_t)toggle) << 24) | (0xFu << 28));
    td[1] = LE32(b);
    td[2] = LE32(next);
    td[3] = LE32(b ? b + len - 1 : 0);
}

static int ohci_control(int addr, int lowspeed, int maxp, uint8_t *setup, int in, uint16_t len)
{
    uint32_t (*td)[4] = D->td;
    int tail;
    memset(td, 0, sizeof D->td);
    if (len) {
        td_fill(td[0], 0 /* SETUP */, setup, 8, 0, PHYS(td[1]));
        td_fill(td[1], in ? 2u : 1u, D->buf, len, 1, PHYS(td[2]));
        td_fill(td[2], in ? 1u : 2u, 0, 0, 1, PHYS(td[3]));
        tail = 3;
    } else {
        td_fill(td[0], 0, setup, 8, 0, PHYS(td[1]));
        td_fill(td[1], 2u, 0, 0, 1, PHYS(td[2]));
        tail = 2;
    }
    uint32_t tailp = PHYS(td[tail]);

    WR(OHCI_CONTROL, RD(OHCI_CONTROL) & ~(1u << 4));                /* CLE off while we edit */
    delay_ms(1);
    D->ed[0] = LE32((uint32_t)addr | ((uint32_t)lowspeed << 13) | ((uint32_t)maxp << 16));
    D->ed[1] = LE32(tailp);
    D->ed[2] = LE32(PHYS(td[0]));
    D->ed[3] = 0;
    WR(OHCI_CTRLHEAD, PHYS(D->ed));
    WR(OHCI_CTRLCUR, 0);
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) | (1u << 4));
    WR(OHCI_CMDSTATUS, 1u << 1);                                    /* control list filled */

    int rc = -1;
    for (int t = 0; t < 500; t++) {
        uint32_t headp = LE32(D->ed[2]);
        if (headp & 1u) { rc = -2; break; }                         /* halted */
        if ((headp & ~0xFu) == tailp) {
            if (len) {
                uint32_t cbp = LE32(td[1][1]);
                rc = cbp ? (int)(cbp - PHYS(D->buf)) : (int)len;
            } else {
                rc = 0;
            }
            break;
        }
        delay_ms(1);
    }
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) & ~(1u << 4));
    if (rc < 0)
        DBG("usb: ohci xfer rc=%d ed %08x td cc %x %x %x\n", rc, LE32(D->ed[2]),
            TD_CC(LE32(td[0][0])), TD_CC(LE32(td[1][0])), TD_CC(LE32(td[2][0])));
    return rc;
}

/* ------------------------------------------------------ control front end */
static int ctl(int addr, uint8_t bmReq, uint8_t bReq, uint16_t wValue, uint16_t wIndex,
               uint16_t wLen, void *data)
{
    if (wLen > XFER_MAX) wLen = XFER_MAX;
    uint8_t *s = D->setup;
    s[0] = bmReq; s[1] = bReq;
    s[2] = (uint8_t)wValue; s[3] = (uint8_t)(wValue >> 8);
    s[4] = (uint8_t)wIndex; s[5] = (uint8_t)(wIndex >> 8);
    s[6] = (uint8_t)wLen;   s[7] = (uint8_t)(wLen >> 8);
    int in = (bmReq & 0x80) != 0;
    if (!in && wLen && data) memcpy(D->buf, data, wLen);
    int rc = on_ohci ? ohci_control(addr, dev.speed == USB_SPEED_LOW, dev.maxp0, s, in, wLen)
                     : ehci_control(addr, dev.maxp0, s, in, wLen);
    if (in && rc > 0 && data) memcpy(data, D->buf, (uint32_t)rc);
    return rc;
}

int usb_control(uint8_t bmReq, uint8_t bReq, uint16_t wValue, uint16_t wIndex, uint16_t wLen, void *data)
{
    if (!dev.present) return -10;
    return ctl(dev.addr, bmReq, bReq, wValue, wIndex, wLen, data);
}

static void get_string(int addr, uint8_t idx, char *out, unsigned max)
{
    uint8_t b[128];
    out[0] = 0;
    if (!idx) return;
    int n = ctl(addr, 0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, sizeof b, b);
    if (n < 2) return;
    if (b[0] < n) n = b[0];
    unsigned o = 0;
    for (int i = 2; i + 1 < n && o + 1 < max; i += 2)
        out[o++] = (b[i + 1] || b[i] < 32 || b[i] > 126) ? '?' : (char)b[i];
    out[o] = 0;
}

static uint8_t toggles[16][2];

static int enumerate(void)
{
    uint8_t d8[8];
    memset(toggles, 0, sizeof toggles);
    dev.addr = 0;
    dev.maxp0 = dev.speed == USB_SPEED_HIGH ? 64 : 8;
    int n = ctl(0, 0x80, 6, 0x0100, 0, 8, d8);
    if (n < 8) { DBG("usb: get_descriptor(8) failed %d\n", n); return -1; }
    dev.maxp0 = d8[7] ? d8[7] : 8;
    if (ctl(0, 0x00, 5, 1, 0, 0, 0) < 0) { DBG("usb: set_address failed\n"); return -2; }
    delay_ms(20);
    dev.addr = 1;
    n = ctl(1, 0x80, 6, 0x0100, 0, 18, dev.dev_desc);
    if (n < 18) { DBG("usb: get_descriptor(18) failed %d\n", n); return -3; }
    dev.vid = (uint16_t)(dev.dev_desc[8] | (dev.dev_desc[9] << 8));
    dev.pid = (uint16_t)(dev.dev_desc[10] | (dev.dev_desc[11] << 8));
    uint8_t c9[9];
    if (ctl(1, 0x80, 6, 0x0200, 0, 9, c9) >= 9) {
        uint16_t total = (uint16_t)(c9[2] | (c9[3] << 8));
        if (total > USB_CFG_MAX) total = USB_CFG_MAX;
        n = ctl(1, 0x80, 6, 0x0200, 0, total, dev.cfg_desc);
        dev.cfg_len = n > 0 ? (uint16_t)n : 0;
    }
    get_string(1, dev.dev_desc[14], dev.manufacturer, sizeof dev.manufacturer);
    get_string(1, dev.dev_desc[15], dev.product, sizeof dev.product);
    get_string(1, dev.dev_desc[16], dev.serial, sizeof dev.serial);
    if (dev.cfg_len >= 9)
        ctl(1, 0x00, 9, dev.cfg_desc[5], 0, 0, 0);                 /* SET_CONFIGURATION */
    dev.present = 1;
    return 0;
}

/* ---------------------------------------------------------- bring-up */
static void ohci_start(void)
{
    memset(D->hcca, 0, sizeof D->hcca);
    WR(OHCI_INTDISABLE, 0x80000000u);
    WR(OHCI_CONTROL, 0);                                            /* USB reset state */
    delay_ms(50);
    WR(OHCI_CMDSTATUS, 1u);                                         /* host controller reset */
    for (int i = 0; i < 50 && (RD(OHCI_CMDSTATUS) & 1u); i++) delay_ms(1);
    WR(OHCI_FMINTERVAL, 0xA7782EDFu);
    WR(OHCI_PERSTART, 0x2A2Fu);
    WR(OHCI_LSTHRESH, 0x0628u);
    WR(OHCI_CTRLHEAD, 0);
    WR(OHCI_CTRLCUR, 0);
    WR(OHCI_HCCA, PHYS(D->hcca));
    WR(OHCI_CONTROL, 0x83u);                                        /* operational, CBSR 4:1 */
    uint32_t a = RD(OHCI_RHDESCA);
    WR(OHCI_RHDESCA, (a & ~0x900u) | 0x200u);                       /* no power switching */
    WR(OHCI_RHDESCB, 0);
    WR(OHCI_RHSTATUS, 1u << 16);                                    /* set global power */
    WR(OHCI_RHPORT1, RH_PPS);
}

int usb_host_init(void)
{
    D = UNC(&dma_mem);
    memset(&dev, 0, sizeof dev);
    on_ohci = 0;

    usb_vbus(0);                                   /* port unpowered until the host runs */
    gpio_set_dir(GPIO_C3_USB_LED, 1);
    gpio_write(GPIO_C3_USB_LED, 1);                /* LED off (active low) */

    REG32(SYS_CLK_MGR) |= 1u << 17;                /* USB host block clock */
    delay_ms(100);
    uint32_t cap = RD(EHCI_BASE);
    if (cap == 0 || cap == 0xFFFFFFFFu) return -1;
    op = EHCI_BASE + (cap & 0xFFu);
    info.ehci_version = (uint16_t)(cap >> 16);
    info.ehci_hcsparams = RD(EHCI_BASE + 4);
    info.ehci_hccparams = RD(EHCI_BASE + 8);
    info.ohci_revision = RD(OHCI_REVISION);

    info.phy_ok = (uint8_t)phy_patch();

    /* EHCI: halt, reset, run with an empty async schedule */
    WR(USBCMD, RD(USBCMD) & ~1u);
    for (int i = 0; i < 20 && !(RD(USBSTS) & (1u << 12)); i++) delay_ms(1);
    WR(USBCMD, 1u << 1);
    for (int i = 0; i < 100 && (RD(USBCMD) & 2u); i++) delay_ms(1);
    WR(USBINTR, 0);
    WR(CTRLDSSEG, 0);
    WR(USBCMD, (8u << 16) | 1u);                   /* run, interrupt threshold 8 */
    WR(CONFIGFLAG, 1u);                            /* route the port to EHCI */
    delay_ms(5);
    WR(PORTSC, (RD(PORTSC) & ~PS_W1C) | PS_PP);

    ohci_start();
    info.ohci_rhdesca = RD(OHCI_RHDESCA);
    return 0;
}

void usb_vbus(int on)
{
    gpio_set_dir(GPIO_A4_USB_POWER, 1);
    gpio_write(GPIO_A4_USB_POWER, on ? 1 : 0);
}

const struct usb_dev *usb_dev(void) { return &dev; }
const struct usb_host_info *usb_info(void) { return &info; }

/* -------------------------------------------------------- bulk transfers */
/* toggles[endpoint number][in] holds the data toggle, reset on enumeration */

/*
 * Up to USB_BULK_MAX bytes as a chain of 16 KB qTDs, so the async schedule
 * is switched on and off once per transfer instead of once per 16 KB.  On a
 * short IN packet the controller follows the (invalid) alternate pointer and
 * stops: the qTDs after the short one stay active and are simply ignored.
 */
static int ehci_bulk(uint8_t ep, uint16_t maxp, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    int in = (ep & 0x80) != 0, num = ep & 0x0F;
    uint32_t *qh = D->bqh;
    int nq = len ? (int)((len + 16383u) / 16384u) : 1;
    for (int i = 0; i < nq; i++) {
        uint32_t off = (uint32_t)i * 16384u;
        uint32_t n = len - off < 16384u ? len - off : 16384u;
        qtd_fill(D->bqtd[i], i + 1 < nq ? PHYS(D->bqtd[i + 1]) : 0, 0, in ? PID_IN : PID_OUT,
                 len ? buf + off : 0, len ? n : 0, 0, i + 1 == nq);
    }
    memset(qh, 0, 64);
    qh[0] = LE32(PHYS(qh) | 2u);
    /* DTC = 0: the queue head keeps the data toggle, seeded from our copy */
    qh[1] = LE32((uint32_t)dev.addr | ((uint32_t)num << 8) | (2u << 12) | (1u << 15) | ((uint32_t)maxp << 16));
    qh[2] = LE32(1u << 30);
    qh[4] = LE32(PHYS(D->bqtd[0]));
    qh[5] = LE32(1u);
    qh[6] = LE32((uint32_t)toggles[num][in] << 31);
    WR(ASYNCLB, PHYS(qh));
    ehci_async(1);
    int rc = -1, cur = 0;
    uint32_t got = 0, t0 = micros();
    for (;;) {
        uint32_t tok = LE32(D->bqtd[cur][2]);
        if (tok & QTD_HALTED) { rc = -2; break; }
        if (!(tok & QTD_ACTIVE)) {
            uint32_t want = len - (uint32_t)cur * 16384u;
            if (want > 16384u) want = 16384u;
            uint32_t left = (tok >> 16) & 0x7FFFu;
            got += want - left;
            if (left || cur + 1 == nq) { rc = (int)got; break; }   /* short packet or last qTD */
            cur++;
            continue;
        }
        if (elapsed_us(t0) > timeout_ms * 1000u) { rc = -3; break; }
    }
    ehci_async(0);
    toggles[num][in] = (uint8_t)(LE32(qh[6]) >> 31);
    if (rc < 0)
        DBG("usb: ehci bulk ep %02x len %u rc %d qtd %d token %08x\n", ep, len, rc, cur, LE32(D->bqtd[cur][2]));
    return rc;
}

static int ohci_bulk(uint8_t ep, uint16_t maxp, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    int in = (ep & 0x80) != 0, num = ep & 0x0F;
    uint32_t (*td)[4] = D->btd;
    uint32_t b = PHYS(buf);                       /* <= 4 KB per TD: spans at most two pages */
    memset(td, 0, sizeof D->btd);
    /* buffer rounding, IN/OUT, no interrupt, toggle from the ED carry */
    td[0][0] = LE32((1u << 18) | ((in ? 2u : 1u) << 19) | (7u << 21) | (0xFu << 28));
    td[0][1] = LE32(len ? b : 0);
    td[0][2] = LE32(PHYS(td[1]));
    td[0][3] = LE32(len ? b + len - 1 : 0);
    uint32_t tailp = PHYS(td[1]);

    WR(OHCI_CONTROL, RD(OHCI_CONTROL) & ~(1u << 5));                /* BLE off while editing */
    delay_us(1100);
    D->bed[0] = LE32((uint32_t)dev.addr | ((uint32_t)num << 7) | ((uint32_t)maxp << 16));
    D->bed[1] = LE32(tailp);
    D->bed[2] = LE32(PHYS(td[0]) | ((uint32_t)toggles[num][in] << 1));
    D->bed[3] = 0;
    WR(OHCI_BULKHEAD, PHYS(D->bed));
    WR(OHCI_BULKCUR, 0);
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) | (1u << 5));
    WR(OHCI_CMDSTATUS, 1u << 2);                                    /* bulk list filled */

    int rc = -1;
    uint32_t t0 = micros();
    for (;;) {
        uint32_t headp = LE32(D->bed[2]);
        if (headp & 1u) { rc = -2; break; }
        if ((headp & ~0xFu) == tailp) {
            uint32_t cbp = LE32(td[0][1]);
            rc = len ? (cbp ? (int)(cbp - b) : (int)len) : 0;
            break;
        }
        if (elapsed_us(t0) > timeout_ms * 1000u) { rc = -3; break; }
    }
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) & ~(1u << 5));
    toggles[num][in] = (uint8_t)((LE32(D->bed[2]) >> 1) & 1u);
    if (rc < 0)
        DBG("usb: ohci bulk ep %02x len %u rc %d cc %x\n", ep, len, rc, TD_CC(LE32(td[0][0])));
    return rc;
}

struct usb_bulk_stats usb_bulk_stats;
int usb_zero_copy = 1;

int usb_bulk(uint8_t ep, uint16_t maxp, void *data, uint32_t len, uint32_t timeout_ms)
{
    if (!dev.present) return -10;
    int in = (ep & 0x80) != 0;
    /* Zero copy: the controller moves data straight to/from the caller's
     * buffer.  The D-cache is write-back, so dirty lines are written back
     * before every direct transfer (OUT: the device reads RAM; IN: no dirty
     * line may later be evicted over fresh data) and invalidated after IN.
     * Direct buffers must cover whole cache lines (32-byte aligned start and
     * length), so no unrelated variable shares a line with DMA data; small
     * or unaligned pieces (CBW, CSW, sense data) use the bounce buffer. */
    int direct = usb_zero_copy && data && (((uint32_t)data | len) & 31u) == 0 && len >= 512;
    uint32_t chunk_max = !direct ? sizeof D->bbuf : on_ohci ? 4096u : USB_BULK_MAX;
    uint32_t done = 0;
    uint8_t *p = data;
    do {
        uint32_t n = len - done;
        if (n > chunk_max) n = chunk_max;
        uint8_t *buf = direct ? p + done : D->bbuf;
        uint32_t t0 = micros();
        if (direct) cctl_write(in ? CCTL_DWBINVAL : CCTL_DWB);
        else if (!in && n) memcpy(D->bbuf, p + done, n);
        uint32_t t1 = micros();
        int rc = on_ohci ? ohci_bulk(ep, maxp, buf, n, timeout_ms) : ehci_bulk(ep, maxp, buf, n, timeout_ms);
        uint32_t t2 = micros();
        if (in && direct) cctl_write(CCTL_DWBINVAL);                /* drop lines read during the transfer */
        if (rc < 0) return done ? (int)done : rc;
        if (in && rc > 0 && !direct) memcpy(p + done, D->bbuf, (uint32_t)rc);
        uint32_t t3 = micros();
        usb_bulk_stats.us_copy += ((t1 - t0) & TC_DATA_MAX) + ((t3 - t2) & TC_DATA_MAX);
        usb_bulk_stats.us_xfer += (t2 - t1) & TC_DATA_MAX;
        usb_bulk_stats.bytes += (uint32_t)rc;
        usb_bulk_stats.transfers++;
        done += (uint32_t)rc;
        if ((uint32_t)rc < n) break;                                /* short packet ends the transfer */
    } while (done < len);
    return (int)done;
}

int usb_clear_halt(uint8_t ep)
{
    int rc = usb_control(0x02, 0x01, 0, ep, 0, 0);                  /* CLEAR_FEATURE(ENDPOINT_HALT) */
    toggles[ep & 0x0F][(ep & 0x80) != 0] = 0;
    return rc;
}

/* -------------------------------------------- interrupt IN (OHCI periodic) */
static void itd_arm(int i, int next)
{
    uint32_t *td = D->itd[i];
    uint32_t b = PHYS(D->ibuf[i]);
    /* buffer rounding, PID IN, no interrupt, toggle from the ED, CC not accessed */
    td[0] = LE32((1u << 18) | (2u << 19) | (7u << 21) | (0xFu << 28));
    td[1] = LE32(b);
    td[2] = LE32(PHYS(D->itd[next]));
    td[3] = LE32(b + intr_len - 1);
}

void usb_intr_close(void)
{
    if (!intr_active) return;
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) & ~(1u << 2));                /* periodic list off */
    delay_ms(2);
    uint32_t *tab = (uint32_t *)D->hcca;
    for (int i = 0; i < 32; i++) tab[i] = 0;
    intr_active = 0;
}

int usb_intr_open(uint8_t ep, uint16_t maxp)
{
    if (!dev.present) return -10;
    if (!on_ohci) return -11;                /* high-speed periodic schedule: not yet */
    usb_intr_close();
    if (maxp == 0 || maxp > 64) maxp = 64;
    intr_len = maxp;
    memset(D->itd, 0, sizeof D->itd);
    itd_arm(0, 1);                           /* TD 0 active, TD 1 is the dummy tail */
    D->ied[0] = LE32((uint32_t)dev.addr | ((uint32_t)(ep & 0x0F) << 7) |
                     ((uint32_t)(dev.speed == USB_SPEED_LOW) << 13) | ((uint32_t)maxp << 16));
    D->ied[1] = LE32(PHYS(D->itd[1]));
    D->ied[2] = LE32(PHYS(D->itd[0]));       /* toggle carry starts at DATA0 */
    D->ied[3] = 0;
    uint32_t *tab = (uint32_t *)D->hcca;     /* poll it every frame (1 ms) */
    for (int i = 0; i < 32; i++) tab[i] = LE32(PHYS(D->ied));
    intr_cur = 0;
    intr_active = 1;
    WR(OHCI_CONTROL, RD(OHCI_CONTROL) | (1u << 2));                 /* periodic list on */
    return 0;
}

int usb_intr_poll(void *buf, uint16_t max)
{
    if (!intr_active || !dev.present) return -10;
    uint32_t head = LE32(D->ied[2]);
    if (head & 1u) return -2;                /* endpoint halted (STALL or errors) */
    int tail = intr_cur ^ 1;
    if ((head & ~0xFu) != PHYS(D->itd[tail]))
        return 0;                            /* nothing yet: the device keeps NAKing */
    uint32_t *td = D->itd[intr_cur];
    uint32_t cc = TD_CC(LE32(td[0]));
    uint32_t cbp = LE32(td[1]);
    int n = cbp ? (int)(cbp - PHYS(D->ibuf[intr_cur])) : (int)intr_len;
    if (cc != 0 && cc != 9)                  /* 9 = data underrun, normal with rounding */
        n = -(int)(100 + cc);
    else if (buf)
        memcpy(buf, D->ibuf[intr_cur], (uint32_t)(n < max ? n : max));
    /* re-arm: the old tail becomes the active TD, the finished TD the new tail */
    memset(D->itd[intr_cur], 0, 16);
    itd_arm(tail, intr_cur);
    D->ied[1] = LE32(PHYS(D->itd[intr_cur]));
    intr_cur = tail;
    return n;
}

void usb_intr_debug(void)
{
    uint32_t *tab = (uint32_t *)D->hcca;
    printf("usb intr: active %d cur %d  ED %08x %08x %08x %08x  TD0 %08x %08x %08x %08x  TD1 %08x %08x %08x %08x\n",
           intr_active, intr_cur, LE32(D->ied[0]), LE32(D->ied[1]), LE32(D->ied[2]), LE32(D->ied[3]),
           LE32(D->itd[0][0]), LE32(D->itd[0][1]), LE32(D->itd[0][2]), LE32(D->itd[0][3]),
           LE32(D->itd[1][0]), LE32(D->itd[1][1]), LE32(D->itd[1][2]), LE32(D->itd[1][3]));
    printf("usb intr: HcControl %08x HcHCCA %08x (ours %08x) HcFmNumber %08x hcca.frame %04x hcca[0] %08x HcIntStatus %08x HcPeriodCur %08x\n",
           RD(OHCI_CONTROL), RD(OHCI_HCCA), PHYS(D->hcca), RD(OHCI_BASE + 0x3C),
           (unsigned)(LE32(tab[32]) & 0xFFFF), LE32(tab[0]), RD(OHCI_INTSTATUS), RD(OHCI_BASE + 0x1C));
    printf("usb intr: OHCI regs:");
    for (int i = 0; i < 23; i++) printf(" %02x:%08x", i * 4, RD(OHCI_BASE + 4 * i));
    printf("\n");
}

int usb_find_endpoint(uint8_t cls, uint8_t type, int in, struct usb_ep_info *out)
{
    const uint8_t *c = dev.cfg_desc;
    int cur_iface = -1, match = 0;
    uint8_t sub = 0, proto = 0;
    for (unsigned i = 0; i + 1 < dev.cfg_len; ) {
        uint8_t len = c[i], t = c[i + 1];
        if (len < 2) break;
        if (t == 4 && len >= 9) {
            cur_iface = c[i + 2];
            match = (c[i + 5] == cls);
            sub = c[i + 6];
            proto = c[i + 7];
        } else if (t == 5 && len >= 7 && match &&
                   (c[i + 3] & 3) == type && ((c[i + 2] & 0x80) != 0) == (in != 0)) {
            out->ep = c[i + 2];
            out->maxp = (uint16_t)(c[i + 4] | (c[i + 5] << 8));
            out->interval = c[i + 6];
            out->iface = (uint8_t)cur_iface;
            out->subclass = sub;
            out->protocol = proto;
            return 0;
        }
        i += len;
    }
    return -1;
}

static void release_companion(void)
{
    uint32_t p = RD(PORTSC);
    if (p & PS_PO) WR(PORTSC, (p & ~PS_W1C) & ~PS_PO);
}

int usb_poll(void)
{
    uint32_t p = RD(PORTSC);
    uint32_t o = RD(OHCI_RHPORT1);

    if (dev.present) {
        int gone = on_ohci ? !(o & RH_CCS) : !(p & PS_CCS);
        /* a port the controller disabled (bus error, babble) stops sending SOF /
         * keep-alives: the device suspends.  Treat it as a disconnect so the next
         * poll resets the port and enumerates again. */
        int disabled = on_ohci ? !(o & RH_PES) : !(p & PS_PE);
        if (!gone && !disabled) return USB_EV_NONE;
        if (gone)
            DBG("usb: disconnect (portsc %08x ohci %08x)\n", p, o);
        else {
            usb_errors.port_disabled++;
            printf("usb: port disabled by the controller (portsc %08x ohci port %08x HcControl %08x HcIntStatus %08x), re-enumerating\n",
                   p, o, RD(OHCI_CONTROL), RD(OHCI_INTSTATUS));
        }
        usb_intr_close();
        memset(&dev, 0, sizeof dev);
        if (on_ohci) WR(OHCI_RHPORT1, RH_CSC);
        WR(PORTSC, p & ~PS_PO);                  /* also acks CSC/PEC/OCC */
        on_ohci = 0;
        gpio_write(GPIO_C3_USB_LED, 1);
        return USB_EV_DISCONNECTED;
    }

    if (!(p & PS_CCS) && !(o & RH_CCS)) {
        if (p & PS_W1C) WR(PORTSC, p);           /* ack stale change bits */
        return USB_EV_NONE;
    }

    delay_ms(100);                               /* connect debounce (USB 2.0: 100 ms) */
    p = RD(PORTSC);
    int rc;
    if ((p & PS_CCS) && !(p & PS_PO)) {
        if (PS_LS(p) == 1) {                     /* K state: low-speed, give it to OHCI */
            DBG("usb: low-speed device, handing to OHCI\n");
            WR(PORTSC, (p & ~PS_W1C) | PS_PO);
            delay_ms(50);
        } else {
            WR(PORTSC, (p & ~PS_W1C & ~PS_PE) | PS_PR);
            delay_ms(50);
            WR(PORTSC, RD(PORTSC) & ~PS_W1C & ~PS_PR);
            for (int i = 0; i < 20 && (RD(PORTSC) & PS_PR); i++) delay_ms(1);
            delay_ms(10);
            p = RD(PORTSC);
            DBG("usb: after EHCI reset portsc %08x\n", p);
            if (p & PS_PE) {
                on_ohci = 0;
                dev.speed = USB_SPEED_HIGH;
                rc = enumerate();
                goto done;
            }
            DBG("usb: full-speed device, handing to OHCI\n");
            WR(PORTSC, (p & ~PS_W1C) | PS_PO);
            delay_ms(50);
        }
    }

    /* full/low speed on the companion */
    o = RD(OHCI_RHPORT1);
    for (int i = 0; i < 50 && !(o & RH_CCS); i++) { delay_ms(2); o = RD(OHCI_RHPORT1); }
    if (!(o & RH_CCS)) {
        DBG("usb: companion sees nothing (portsc %08x ohci %08x)\n", RD(PORTSC), o);
        release_companion();
        return USB_EV_ENUM_FAILED;
    }
    WR(OHCI_RHPORT1, RH_CSC);
    WR(OHCI_RHPORT1, RH_PRS);
    for (int i = 0; i < 50 && !(RD(OHCI_RHPORT1) & RH_PRSC); i++) delay_ms(2);
    WR(OHCI_RHPORT1, RH_PRSC);
    delay_ms(20);
    o = RD(OHCI_RHPORT1);
    DBG("usb: after OHCI reset port %08x\n", o);
    on_ohci = 1;
    dev.speed = (o & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    rc = enumerate();

done:
    if (rc < 0) {
        memset(&dev, 0, sizeof dev);
        on_ohci = 0;
        release_companion();                     /* next attempt starts on EHCI again */
        return USB_EV_ENUM_FAILED;
    }
    gpio_write(GPIO_C3_USB_LED, 0);
    return USB_EV_CONNECTED;
}

/* ------------------------------------------------------------- printing */
const char *usb_speed_name(int s)
{
    return s == USB_SPEED_HIGH ? "high (480M)" : s == USB_SPEED_LOW ? "low (1.5M)" : "full (12M)";
}

const char *usb_class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "per-interface";
    case 0x01: return "audio";
    case 0x02: return "communications (CDC)";
    case 0x03: return "HID";
    case 0x05: return "physical";
    case 0x06: return "image";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "smart card";
    case 0x0E: return "video";
    case 0xDC: return "diagnostic";
    case 0xE0: return "wireless";
    case 0xEF: return "miscellaneous";
    case 0xFE: return "application specific";
    case 0xFF: return "vendor specific";
    default:   return "?";
    }
}

void usb_print_dev(const struct usb_dev *d)
{
    const uint8_t *dd = d->dev_desc;
    printf("usb device: %04x:%04x  %s speed  USB %x.%02x  class %02x/%02x/%02x (%s)  ep0 maxp %d\n",
           d->vid, d->pid, usb_speed_name(d->speed), dd[3], dd[2], dd[4], dd[5], dd[6],
           usb_class_name(dd[4]), d->maxp0);
    if (d->manufacturer[0]) printf("  manufacturer: %s\n", d->manufacturer);
    if (d->product[0])      printf("  product:      %s\n", d->product);
    if (d->serial[0])       printf("  serial:       %s\n", d->serial);
    const uint8_t *c = d->cfg_desc;
    if (d->cfg_len >= 9)
        printf("  config %u: %u interface(s), %s, max %u mA\n", c[5], c[4],
               (c[7] & 0x40) ? "self-powered" : "bus-powered", c[8] * 2u);
    for (unsigned i = 0; i + 1 < d->cfg_len; ) {
        uint8_t len = c[i], type = c[i + 1];
        if (len < 2) break;
        if (type == 4 && len >= 9)
            printf("  interface %u alt %u: class %02x/%02x/%02x (%s), %u endpoint(s)\n",
                   c[i + 2], c[i + 3], c[i + 5], c[i + 6], c[i + 7], usb_class_name(c[i + 5]), c[i + 4]);
        else if (type == 5 && len >= 7) {
            static const char *const tt[] = { "control", "isochronous", "bulk", "interrupt" };
            printf("    endpoint %02x %-3s %-11s maxp %u interval %u\n", c[i + 2],
                   (c[i + 2] & 0x80) ? "IN" : "OUT", tt[c[i + 3] & 3],
                   (unsigned)(c[i + 4] | (c[i + 5] << 8)), c[i + 6]);
        } else if (type == 0x21 && len >= 6)
            printf("    HID descriptor: report descriptor %u bytes\n", (unsigned)(c[i + 7] | (c[i + 8] << 8)));
        i += len;
    }
}

void usb_print_controller(void)
{
    uint32_t o = RD(OHCI_RHPORT1);
    printf("usb ohci: HcControl %08x HcCmdStatus %08x HcIntStatus %08x HcFmNumber %08x HcFmRemaining %08x port %08x (ccs %u pes %u pss %u lsda %u csc %u pesc %u pssc %u ocic %u prsc %u)  EHCI PORTSC %08x USBSTS %08x\n",
           RD(OHCI_CONTROL), RD(OHCI_CMDSTATUS), RD(OHCI_INTSTATUS), RD(OHCI_BASE + 0x3C), RD(OHCI_BASE + 0x38),
           o, o & 1, (o >> 1) & 1, (o >> 2) & 1, (o >> 9) & 1, (o >> 16) & 1, (o >> 17) & 1, (o >> 18) & 1,
           (o >> 19) & 1, (o >> 20) & 1, RD(PORTSC), RD(USBSTS));
}

void usb_print_ports(void)
{
    uint32_t p = RD(PORTSC), o = RD(OHCI_RHPORT1);
    printf("usb ports: EHCI PORTSC=%08x (conn %u en %u line %u power %u owner %s)  OHCI port=%08x (conn %u en %u ls %u)  VBUS gpio=%d\n",
           p, p & 1, (p >> 2) & 1, PS_LS(p), (p >> 12) & 1, (p & PS_PO) ? "OHCI" : "EHCI",
           o, o & 1, (o >> 1) & 1, (o >> 9) & 1, gpio_read(GPIO_A4_USB_POWER));
}
