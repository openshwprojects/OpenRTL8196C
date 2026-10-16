/*
 * wl.c - RTL8192CE Wi-Fi driver, see wl.h.
 *
 * The MAC/BB/RF bring-up is the sequence proven in examples/25_wifi_scan
 * (Linux rtl92ce_hw_init); this file adds the fuse read, transmit, channel
 * control and op-mode/BSSID so the scan/AP/STA demos can run on top.
 * TX power is left at the factory PHY_REG_PG defaults loaded during BB
 * config: fine for close-range demos; the fuse read is used for the MAC
 * address and for reporting the calibrated levels.
 */
#include "wl.h"
#include "rtl8196c.h"
#include "pcie.h"
#include "timer.h"
#include "uart.h"
#include "libc.h"

/* ---- registers (rtl8192ce reg.h) ---- */
#define REG_SYS_ISO_CTRL 0x0000
#define REG_SYS_FUNC_EN  0x0002
#define REG_APS_FSMCO    0x0004
#define REG_SYS_CLKR     0x0008
#define REG_SPS0_CTRL    0x0011
#define REG_RSV_CTRL     0x001C
#define REG_RF_CTRL      0x001F
#define REG_AFE_XTAL     0x0024
#define REG_AFE_PLL      0x0028
#define REG_EFUSE_CTRL   0x0030
#define REG_EFUSE_TEST   0x0034
#define REG_MCUFWDL      0x0080
#define REG_SYS_CFG      0x00F0
#define REG_CR           0x0100
#define MSR              (REG_CR + 2)
#define REG_PBP          0x0104
#define REG_TRXDMA_CTRL  0x010C
#define REG_TRXFF_BNDY   0x0114
#define REG_RXFF_PTR     0x011C
#define REG_HIMR         0x0120
#define REG_HISR         0x0124
#define REG_HIMRE        0x0128
#define REG_HISRE        0x012C
#define REG_MCUTST_1     0x01C0
#define REG_LLT_INIT     0x01E0
#define REG_RQPN         0x0200
#define REG_TDECTRL      0x0208
#define REG_PCIE_CTRL_REG 0x0300
#define REG_INT_MIG      0x0304
#define REG_BCNQ_DESA    0x0308
#define REG_HQ_DESA      0x0310
#define REG_MGQ_DESA     0x0318
#define REG_VOQ_DESA     0x0320
#define REG_VIQ_DESA     0x0328
#define REG_BEQ_DESA     0x0330
#define REG_BKQ_DESA     0x0338
#define REG_RX_DESA      0x0340
#define REG_FWHW_TXQ_CTRL 0x0420
#define REG_HWSEQ_CTRL   0x0423
#define REG_SPEC_SIFS    0x0428
#define REG_RL           0x042A
#define REG_DARFRC       0x0430
#define REG_RARFRC       0x0438
#define REG_RRSR         0x0440
#define REG_AGGLEN_LMT   0x0458
#define REG_AMPDU_MIN_SP 0x045C
#define REG_FAST_EDCA    0x0460
#define REG_INIRTS_RATE  0x0480
#define REG_PROT_MODE    0x04C8
#define REG_BAR_MODE     0x04CC
#define REG_BCNTCFG      0x0510
#define REG_PIFS         0x0512
#define REG_SIFS_CTX     0x0514
#define REG_SIFS_TRX     0x0516
#define REG_AGGR_BREAK   0x051A
#define REG_SLOT         0x051B
#define REG_TXPAUSE      0x0522
#define REG_TBTT_PROHIB  0x0540
#define REG_NAV_PROT     0x0546
#define REG_BCN_CTRL     0x0550
#define REG_BCN_INTERVAL 0x0554
#define REG_ATIMWND      0x055A
#define REG_USTIME_TSF   0x055C
#define REG_BCN_MAX_ERR  0x055D
#define REG_RXTSF_CCK    0x055E
#define REG_RXTSF_OFDM   0x055F
#define REG_BWOPMODE     0x0603
#define REG_RCR          0x0608
#define REG_RX_DRVINFO_SZ 0x060F
#define REG_MACID        0x0610
#define REG_BSSID        0x0618
#define REG_MAR          0x0620
#define REG_MAC_SPEC_SIFS 0x063A
#define REG_ACKTO        0x0640
#define REG_RXFLTMAP0    0x06A0
#define REG_RXFLTMAP2    0x06A4
#define FW_START         0x1000
#define FW_PAGE          4096u

#define RFPGA0_RFMOD        0x800
#define RFPGA0_XA_HSSI1     0x820
#define RFPGA0_XA_HSSI2     0x824
#define RFPGA0_XB_HSSI1     0x828
#define RFPGA0_XB_HSSI2     0x82C
#define RFPGA0_XA_LSSI      0x840
#define RFPGA0_XB_LSSI      0x844
#define RFPGA0_XA_RFIFOE    0x860
#define RFPGA0_XB_RFIFOE    0x864
#define RFPGA0_XAB_RFIFSW   0x870
#define RFPGA0_ANAPARM2     0x884
#define RFPGA0_XA_LSSIRB    0x8A0
#define RFPGA0_XB_LSSIRB    0x8A4
#define TRANSCEIVEA_HSPI_RB 0x8B8
#define TRANSCEIVEB_HSPI_RB 0x8BC
#define RFPGA1_RFMOD        0x900

#define RCR_AAP (1u<<0)
#define RCR_APM (1u<<1)
#define RCR_AM  (1u<<2)
#define RCR_AB  (1u<<3)
#define RCR_ACRC32 (1u<<8)
#define RCR_CBSSID_DATA (1u<<6)
#define RCR_CBSSID_BCN  (1u<<7)
#define RCR_ADF (1u<<11)                                   /* accept data frames */
#define RCR_ACF (1u<<12)                                   /* accept control frames */
#define RCR_AMF (1u<<13)                                   /* accept management frames */
#define RCR_HTC_LOC (1u<<14)
#define RCR_APP_PHYST (1u<<28)
#define RCR_APPFCS (1u<<31)

/* ---- register access ---- */
uint32_t wl_rd32(struct wl_dev *d, uint32_t r) { return REG32(d->mem + r); }
void wl_wr32(struct wl_dev *d, uint32_t r, uint32_t v) { REG32(d->mem + r) = v; }
uint8_t wl_rd8(struct wl_dev *d, uint32_t r) { return *(volatile uint8_t *)(d->mem + r); }
void wl_wr8(struct wl_dev *d, uint32_t r, uint8_t v) { *(volatile uint8_t *)(d->mem + r) = v; }
static uint16_t rd16(struct wl_dev *d, uint32_t r) { return *(volatile uint16_t *)(d->mem + r); }
void wl_wr16(struct wl_dev *d, uint32_t r, uint16_t v) { *(volatile uint16_t *)(d->mem + r) = v; }
#define rd32 wl_rd32
#define wr32 wl_wr32
#define rd8  wl_rd8
#define wr8  wl_wr8

static int lowbit(uint32_t m) { int s = 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static void bb_set(struct wl_dev *d, uint32_t r, uint32_t mask, uint32_t v) { wr32(d, r, (rd32(d, r) & ~mask) | ((v << lowbit(mask)) & mask)); }
static uint32_t bb_get(struct wl_dev *d, uint32_t r, uint32_t mask) { return (rd32(d, r) & mask) >> lowbit(mask); }

static const uint32_t rf_lssi[2] = { RFPGA0_XA_LSSI, RFPGA0_XB_LSSI };
static const uint32_t rf_hssi1[2] = { RFPGA0_XA_HSSI1, RFPGA0_XB_HSSI1 };
static const uint32_t rf_hssi2[2] = { RFPGA0_XA_HSSI2, RFPGA0_XB_HSSI2 };
static const uint32_t rf_rb[2] = { RFPGA0_XA_LSSIRB, RFPGA0_XB_LSSIRB };
static const uint32_t rf_rbpi[2] = { TRANSCEIVEA_HSPI_RB, TRANSCEIVEB_HSPI_RB };

static void rf_write(struct wl_dev *d, int path, uint32_t reg, uint32_t val)
{
    wr32(d, rf_lssi[path], ((reg & 0x3F) << 20) | (val & 0xFFFFF));
    delay_us(1);
}
static uint32_t rf_read(struct wl_dev *d, int path, uint32_t reg)
{
    uint32_t a = rd32(d, RFPGA0_XA_HSSI2);
    uint32_t b = path ? rd32(d, rf_hssi2[path]) : a;
    b = (b & ~0x7F800000u) | ((reg & 0x3F) << 23) | 0x80000000u;
    wr32(d, RFPGA0_XA_HSSI2, a & ~0x80000000u); delay_ms(1);
    wr32(d, rf_hssi2[path], b); delay_ms(1);
    wr32(d, RFPGA0_XA_HSSI2, a | 0x80000000u); delay_ms(1);
    int pi = (rd32(d, rf_hssi1[path]) >> 8) & 1;
    return rd32(d, pi ? rf_rbpi[path] : rf_rb[path]) & 0xFFFFF;
}
static int tbl_delay(uint32_t reg)
{
    switch (reg) {
    case 0xFE: delay_ms(50); return 1; case 0xFD: delay_ms(5); return 1;
    case 0xFC: delay_ms(1); return 1; case 0xFB: delay_us(50); return 1;
    case 0xFA: delay_us(5); return 1; case 0xF9: delay_us(1); return 1;
    default: return 0;
    }
}

/* ---- rings ---- */
#define NRX 32
#define RXBUF 2048
#define NTX 8
struct rxdesc { uint32_t w[8]; };                        /* RX descriptor: 32 bytes (8 words) */
struct desc { uint32_t w[16]; };                         /* TX descriptor: 64-byte stride, 40 bytes used */
static struct rxdesc rx_ring[NRX] __attribute__((aligned(256)));
static struct desc tx_ring[NTX] __attribute__((aligned(256)));   /* management / data */
static struct desc bcn_ring[2] __attribute__((aligned(256)));
static uint8_t rx_buf[NRX][RXBUF] __attribute__((aligned(32)));
static uint8_t tx_buf[NTX][RXBUF] __attribute__((aligned(32)));
static uint8_t bcn_buf[2][RXBUF] __attribute__((aligned(32)));
static uint32_t rx_next, tx_next, bcn_next;

static uint32_t sw(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
#define PHYS(p) ((uint32_t)(p) & 0x1FFFFFFFu)

static void rx_arm(uint32_t i)
{
    rx_ring[i].w[6] = sw(PHYS(rx_buf[i]));
    rx_ring[i].w[0] = sw((1u << 31) | (i == NRX - 1 ? (1u << 30) : 0) | RXBUF);
}

/* ---- fuse ---- */
static uint8_t efuse_raw[512];
static uint8_t efuse_map[128];

static void efuse_read_byte(struct wl_dev *d, uint16_t off, uint8_t *out)
{
    wr8(d, REG_EFUSE_CTRL + 1, off & 0xFF);
    wr8(d, REG_EFUSE_CTRL + 2, ((off >> 8) & 0x03) | (rd8(d, REG_EFUSE_CTRL + 2) & 0xFC));
    wr8(d, REG_EFUSE_CTRL + 3, rd8(d, REG_EFUSE_CTRL + 3) & 0x7F);   /* read command */
    uint32_t v = 0; int retry = 0;
    while (!((rd32(d, REG_EFUSE_CTRL) >> 24) & 0x80) && retry < 10000) { v = rd32(d, REG_EFUSE_CTRL); retry++; }
    delay_us(50);
    v = rd32(d, REG_EFUSE_CTRL);
    *out = (uint8_t)(v & 0xFF);
}

static void efuse_load(struct wl_dev *d)
{
    /* power up the efuse loader */
    wl_wr16(d, REG_SYS_ISO_CTRL, rd16(d, REG_SYS_ISO_CTRL) | (1u << 5));      /* PWC_EV12V */
    wl_wr16(d, REG_SYS_FUNC_EN, rd16(d, REG_SYS_FUNC_EN) | (1u << 8));        /* FEN_ELDR */
    wl_wr16(d, REG_SYS_CLKR, rd16(d, REG_SYS_CLKR) | (1u << 1) | (1u << 11)); /* LOADER_CLK | ANA8M */
    for (int i = 0; i < 512; i++) efuse_read_byte(d, (uint16_t)i, &efuse_raw[i]);

    /* unpack the physical efuse into the 128-byte logical map (8192C format) */
    memset(efuse_map, 0xFF, sizeof efuse_map);
    uint16_t a = 0;
    while (a < 512) {
        uint8_t hdr = efuse_raw[a++];
        if (hdr == 0xFF) break;
        uint16_t off; uint8_t wren;
        if ((hdr & 0x1F) == 0x0F) {
            uint8_t ext = efuse_raw[a++];
            off = ((ext & 0xF0) >> 1) | ((hdr >> 5) & 0x07);
            wren = ext & 0x0F;
        } else {
            off = (hdr >> 4) & 0x0F;
            wren = hdr & 0x0F;
        }
        for (int i = 0; i < 4; i++) {
            if (!(wren & (1 << i))) {
                uint16_t p = off * 8 + i * 2;
                if (p + 1u < sizeof efuse_map && a + 1 < 512) { efuse_map[p] = efuse_raw[a++]; efuse_map[p + 1] = efuse_raw[a++]; }
            }
        }
    }
    const uint8_t *m = efuse_map + 0x16;                                      /* EEPROM_MAC_ADDR */
    int ok = 0;
    for (int i = 0; i < 6; i++) if (m[i] != 0xFF && m[i] != 0x00) ok = 1;
    if (ok) { memcpy(d->mac, m, 6); d->fuse_ok = 1; }
    /* calibrated power, channel group of ch1 (group 0), path A, for reporting */
    for (int i = 0; i < 14; i++) {
        int g = i < 3 ? 0 : i < 9 ? 1 : 2;
        uint8_t c = efuse_map[0x5A + g], h = efuse_map[0x60 + g];     /* CCK, HT40_1S */
        d->pwr_cck[i] = (d->fuse_ok && c != 0xFF) ? c : 0x22;
        d->pwr_ht40_1s[i] = (d->fuse_ok && h != 0xFF) ? h : 0x22;
    }
}

/* ---- bring-up (as examples/25_wifi_scan) ---- */
static int power_on(struct wl_dev *d)
{
    wr8(d, REG_RSV_CTRL, 0x00); wr8(d, REG_SPS0_CTRL, 0x2B); wr8(d, REG_AFE_XTAL, 0x0F);
    delay_us(2);
    wr8(d, REG_APS_FSMCO + 1, rd8(d, REG_APS_FSMCO + 1) | 0x01);
    int n = 0; while ((rd8(d, REG_APS_FSMCO + 1) & 0x01) && n < 1000) { delay_us(50); n++; }
    if (n >= 1000) return -1;
    wl_wr16(d, REG_APS_FSMCO, 0x1012);
    wr8(d, REG_SYS_ISO_CTRL + 1, 0x82); delay_us(2);
    wl_wr16(d, REG_CR, 0x02FF);
    return 0;
}
static int llt_write(struct wl_dev *d, uint32_t a, uint32_t v)
{
    wr32(d, REG_LLT_INIT, (1u << 30) | ((a & 0xFF) << 8) | (v & 0xFF));
    for (int n = 0; n < 100; n++) if (((rd32(d, REG_LLT_INIT) >> 30) & 3) == 0) return 0;
    return -1;
}
static int init_mac(struct wl_dev *d)
{
    const uint32_t bndy = 0xF6, maxp = 0xFF;
    wr32(d, REG_RQPN, 0x80A32929u);
    wr32(d, REG_TRXFF_BNDY, 0x27FF0000u | bndy);
    wr8(d, REG_TDECTRL + 1, bndy);
    wr8(d, 0x424, bndy); wr8(d, 0x425, bndy); wr8(d, 0x45D, bndy);
    wr8(d, REG_PBP, 0x11); wr8(d, REG_RX_DRVINFO_SZ, 0x04);
    int err = 0;
    for (uint32_t i = 0; i < bndy - 1; i++) err |= llt_write(d, i, i + 1);
    err |= llt_write(d, bndy - 1, 0xFF);
    for (uint32_t i = bndy; i < maxp; i++) err |= llt_write(d, i, i + 1);
    err |= llt_write(d, maxp, bndy);
    if (err) return -1;
    wr32(d, REG_HISR, 0xFFFFFFFFu); wr8(d, REG_HISRE, 0xFF);
    wl_wr16(d, REG_TRXFF_BNDY + 2, 0x27FF);
    wl_wr16(d, REG_TRXDMA_CTRL, (uint16_t)((rd16(d, REG_TRXDMA_CTRL) & 0xF) | 0xF771));
    wr8(d, REG_FWHW_TXQ_CTRL + 1, 0x1F);

    memset(tx_ring, 0, sizeof tx_ring); memset(bcn_ring, 0, sizeof bcn_ring);
    /* chain each TX ring into a circular linked list (word 10 = next desc) */
    for (uint32_t i = 0; i < NTX; i++) tx_ring[i].w[10] = sw(PHYS(&tx_ring[(i + 1) % NTX]));
    bcn_ring[0].w[10] = sw(PHYS(&bcn_ring[1]));
    bcn_ring[1].w[10] = sw(PHYS(&bcn_ring[0]));
    wr32(d, REG_BEQ_DESA, PHYS(tx_ring));     /* best-effort queue holds our TX ring */
    wr32(d, REG_BKQ_DESA, PHYS(tx_ring)); wr32(d, REG_VOQ_DESA, PHYS(tx_ring));
    wr32(d, REG_VIQ_DESA, PHYS(tx_ring)); wr32(d, REG_MGQ_DESA, PHYS(tx_ring));
    wr32(d, REG_HQ_DESA, PHYS(tx_ring)); wr32(d, REG_BCNQ_DESA, PHYS(bcn_ring));
    for (uint32_t i = 0; i < NRX; i++) rx_arm(i);
    rx_next = 0; tx_next = 0;
    wr32(d, REG_RX_DESA, PHYS(rx_ring));
    wr8(d, REG_PCIE_CTRL_REG + 3, 0x77);
    wr32(d, REG_INT_MIG, 0);
    wr8(d, 0x600, rd8(d, 0x600) & ~0x40);
    for (int n = 0; n < 200 && (rd8(d, 0x600) & 0x80); n++) delay_us(10);
    wr32(d, REG_MCUTST_1, 0);
    return 0;
}
static int download_fw(struct wl_dev *d, const uint8_t *fw, uint32_t len)
{
    const uint8_t *p = fw + 32; len -= 32;
    if (rd8(d, REG_MCUFWDL) & 0x80) wr8(d, REG_MCUFWDL, 0x00);
    wr8(d, REG_SYS_FUNC_EN + 1, rd8(d, REG_SYS_FUNC_EN + 1) | 0x04);
    wr8(d, REG_MCUFWDL, rd8(d, REG_MCUFWDL) | 0x01);
    wr8(d, REG_MCUFWDL + 2, rd8(d, REG_MCUFWDL + 2) & 0xF7);
    for (uint32_t off = 0, page = 0; off < len; off += FW_PAGE, page++) {
        wr8(d, REG_MCUFWDL + 2, (uint8_t)((rd8(d, REG_MCUFWDL + 2) & 0xF8) | (page & 7)));
        uint32_t n = len - off < FW_PAGE ? len - off : FW_PAGE;
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) if (off + i + k < len) v |= (uint32_t)p[off + i + k] << (8 * k);
            wr32(d, FW_START + i, v);
        }
    }
    wr8(d, REG_MCUFWDL, rd8(d, REG_MCUFWDL) & 0xFE);
    wr8(d, REG_MCUFWDL + 1, 0x00);
    int n = 0; while (!(rd32(d, REG_MCUFWDL) & 0x04) && n < 1000) { delay_us(50); n++; }
    if (n >= 1000) return -1;
    wr32(d, REG_MCUFWDL, (rd32(d, REG_MCUFWDL) | 0x02) & ~0x40u);
    for (n = 0; n < 1000; n++) { if (rd32(d, REG_MCUFWDL) & 0x40) return 0; delay_ms(1); }
    return -2;
}
static void config_bb(struct wl_dev *d, const struct wl_tables *t)
{
    wl_wr16(d, REG_SYS_FUNC_EN, rd16(d, REG_SYS_FUNC_EN) | (1u << 13) | 0x3);
    wr8(d, REG_AFE_PLL, 0x83); wr8(d, REG_AFE_PLL + 1, 0xDB);
    wr8(d, REG_RF_CTRL, 0x07); wr8(d, REG_SYS_FUNC_EN, 0xE3); wr8(d, REG_AFE_XTAL + 1, 0x80);
    wr32(d, 0x4C, rd32(d, 0x4C) | (1u << 23));
    for (uint32_t i = 0; i < t->phy_n; i++) if (!tbl_delay(t->phy[i][0])) { wr32(d, t->phy[i][0], t->phy[i][1]); delay_us(1); }
    for (uint32_t i = 0; i < t->phy_fix_n; i++) { wr32(d, t->phy_fix[i][0], t->phy_fix[i][1]); delay_us(1); }
    for (uint32_t i = 0; i < t->agc_n; i++) { wr32(d, t->agc[i][0], t->agc[i][1]); delay_us(1); }
}
static void config_rf(struct wl_dev *d, const struct wl_tables *t)
{
    for (int path = 0; path < 2; path++) {
        uint32_t envmask = path ? 0x10u << 16 : 0x10u;
        uint32_t ife = path ? RFPGA0_XB_RFIFOE : RFPGA0_XA_RFIFOE;
        uint32_t saved = bb_get(d, RFPGA0_XAB_RFIFSW, envmask);
        bb_set(d, ife, 0x10u << 16, 1); delay_us(1);
        bb_set(d, ife, 0x10u, 1); delay_us(1);
        bb_set(d, rf_hssi2[path], 0x400, 0); delay_us(1);
        bb_set(d, rf_hssi2[path], 0x800, 0); delay_us(1);
        const uint32_t (*r)[2] = path ? t->radio_b : t->radio_a;
        uint32_t n = path ? t->radio_b_n : t->radio_a_n;
        for (uint32_t i = 0; i < n; i++) if (!tbl_delay(r[i][0])) rf_write(d, path, r[i][0], r[i][1]);
        bb_set(d, RFPGA0_XAB_RFIFSW, envmask, saved);
    }
}
static void lc_cal(struct wl_dev *d)
{
    uint8_t d03 = rd8(d, 0xD03);
    uint32_t mode[2] = { 0, 0 };
    if (d03 & 0x70) { wr8(d, 0xD03, d03 & 0x8F); for (int p = 0; p < 2; p++) { mode[p] = rf_read(d, p, 0); rf_write(d, p, 0, (mode[p] & 0x8FFFF) | 0x10000); } }
    else wr8(d, REG_TXPAUSE, 0xFF);
    rf_write(d, 0, 0x18, rf_read(d, 0, 0x18) | 0x08000);
    delay_ms(100);
    if (d03 & 0x70) { wr8(d, 0xD03, d03); for (int p = 0; p < 2; p++) rf_write(d, p, 0, mode[p]); }
    wr8(d, REG_TXPAUSE, 0xFF);
}
static void hw_configure(struct wl_dev *d)
{
    wr8(d, REG_INIRTS_RATE, 0x08);
    wr8(d, REG_BWOPMODE, 0x20);                              /* 20 MHz */
    wr32(d, REG_RRSR, 0x00000FFFu);                          /* all CCK + OFDM rates */
    wr8(d, REG_SLOT, 0x09); wr8(d, REG_AMPDU_MIN_SP, 0x00);
    wl_wr16(d, REG_FWHW_TXQ_CTRL, 0x1F80);
    wl_wr16(d, REG_RL, 0x0707);
    wr32(d, REG_BAR_MODE, 0x02012802u);
    wr8(d, REG_HWSEQ_CTRL, 0xFF);
    wr32(d, REG_DARFRC, 0x01000000u); wr32(d, REG_DARFRC + 4, 0x07060504u);
    wr32(d, REG_RARFRC, 0x01000000u); wr32(d, REG_RARFRC + 4, 0x07060504u);
    wr32(d, REG_AGGLEN_LMT, 0xB972A841u);
    wr8(d, REG_ATIMWND, 0x02); wr8(d, REG_BCN_MAX_ERR, 0xFF);
    wr8(d, REG_BCN_CTRL, 0x1F);
    wr8(d, REG_TBTT_PROHIB + 1, 0xFF);
    wr8(d, REG_PIFS, 0x1C); wr8(d, REG_AGGR_BREAK, 0x16);
    wl_wr16(d, REG_NAV_PROT, 0x0020);
    wr32(d, REG_FAST_EDCA, 0x086666u);
    wr8(d, REG_ACKTO, 0x40);
    wl_wr16(d, REG_SPEC_SIFS, 0x1010); wl_wr16(d, REG_MAC_SPEC_SIFS, 0x1010);
    wl_wr16(d, REG_SIFS_CTX, 0x1010); wl_wr16(d, REG_SIFS_TRX, 0x1010);
    wr32(d, REG_MAR, 0xFFFFFFFFu); wr32(d, REG_MAR + 4, 0xFFFFFFFFu);
    /* EDCA params (BE/BK/VI/VO) */
    wr32(d, 0x508, 0x005EA42Bu); wr32(d, 0x50C, 0x0000A44Fu);
    wr32(d, 0x504, 0x005EA324u); wr32(d, 0x500, 0x002FA226u);
    /* TSF tick = MAC clocks per microsecond; only the low 6 bits count.
     * Measured: 0x50 (-> 16) gave 2544 ticks/ms, 0x20 gave 1250, so the MAC
     * clock is 40 MHz here ("40MHz clock source" board) and 0x28 = 40 gives
     * an exact 1 us TSF, hence a true 102.4 ms beacon interval. */
    wr8(d, REG_USTIME_TSF, 0x28);
}

int wl_hw_init(struct wl_dev *d, const struct wl_tables *t)
{
    memset(d, 0, sizeof *d);
    d->mem = PCIE_MEM;
    static const uint8_t dflt[6] = { 0x02, 0x19, 0x6C, 0x00, 0x00, 0x02 };
    memcpy(d->mac, dflt, 6);
    efuse_load(d);
    if (power_on(d) < 0) return -1;
    wr8(d, REG_TXPAUSE, 0xFF);
    if (init_mac(d) < 0) return -2;
    if (download_fw(d, t->fw, t->fw_len) < 0) return -3;
    for (uint32_t i = 0; i < t->mac_n; i++) wr8(d, t->mac[i][0], (uint8_t)t->mac[i][1]);
    config_bb(d, t);
    config_rf(d, t);
    for (int p = 0; p < 2; p++) d->chnlval[p] = rf_read(d, p, 0x18);
    bb_set(d, RFPGA0_RFMOD, 1u << 24, 1);
    bb_set(d, RFPGA0_RFMOD, 1u << 25, 1);
    bb_set(d, RFPGA0_ANAPARM2, 1u << 10, 1);
    bb_set(d, RFPGA0_XB_RFIFOE, 0x60, 1);
    lc_cal(d);
    wr8(d, REG_BWOPMODE, rd8(d, REG_BWOPMODE) | 0x04);
    bb_set(d, RFPGA0_RFMOD, 1, 0); bb_set(d, RFPGA1_RFMOD, 1, 0);
    for (int p = 0; p < 2; p++) d->chnlval[p] = (d->chnlval[p] & ~0xC00u) | 0x400u;
    d->chnlval[1] = d->chnlval[0];
    d->cck_highpwr = (int)bb_get(d, RFPGA0_XA_HSSI2, 0x200);
    hw_configure(d);
    for (int i = 0; i < 6; i++) { wr8(d, REG_MACID + i, d->mac[i]); wr8(d, REG_BSSID + i, 0); }
    wl_wr16(d, REG_RXFLTMAP0, 0xFFFF); wl_wr16(d, REG_RXFLTMAP2, 0xFFFF);
    /* ADF: data frames (the control build received them even without it, via
     * RXFLTMAP2; kept on).  ACF (control frames) left off: ACKs only load the ring. */
    wr32(d, REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_ACRC32 | RCR_ADF | RCR_AMF | RCR_HTC_LOC |
                     RCR_APP_PHYST | RCR_APPFCS);
    wr8(d, MSR, rd8(d, MSR) & ~0x03);
    wr8(d, REG_TXPAUSE, 0xFF);                               /* receive only until wl_hw_tx_enable */
    d->chan = 1;
    return 0;
}

void wl_hw_set_channel(struct wl_dev *d, uint8_t ch)
{
    d->chan = ch;
    for (int p = 0; p < 2; p++) rf_write(d, p, 0x18, (d->chnlval[p] & ~0x3FFu) | ch);
}

static int rssi_of(const uint8_t *phy, uint32_t mcs, int hp)
{
    if (mcs < 4) {
        uint8_t agc = phy[5]; static const int base[4] = { 16, -12, -26, -46 };
        if (!hp) return base[(agc & 0xC0) >> 6] - (agc & 0x3E);
        return base[(agc & 0x60) >> 5] - ((agc & 0x1F) << 1);
    }
    return ((phy[4] >> 1) & 0x7F) - 110;
}
void wl_hw_rx_poll(struct wl_dev *d, wl_rx_fn cb, void *ctx)
{
    uint32_t isr = rd32(d, REG_HISR), isre = rd32(d, REG_HISRE);
    if (isr) wr32(d, REG_HISR, isr);
    if (isre) wr32(d, REG_HISRE, isre);
    for (int budget = 0; budget < NRX; budget++) {
        struct rxdesc *de = &rx_ring[rx_next];
        uint32_t w0 = sw(de->w[0]);
        if (w0 & (1u << 31)) return;
        uint32_t len = w0 & 0x3FFF, drv = (w0 >> 16) & 0xF, shift = (w0 >> 24) & 3, mcs = sw(de->w[3]) & 0x3F;
        if (!(w0 & (1u << 14)) && drv * 8 + shift + len <= RXBUF && len >= 10) {
            const uint8_t *buf = rx_buf[rx_next];
            const uint8_t *f = buf + drv * 8 + shift;
            int rssi = drv ? rssi_of(buf, mcs, d->cck_highpwr) : -100;
            if (len >= 4) cb(ctx, f, len - 4, d->chan, rssi);     /* drop FCS */
        }
        rx_arm(rx_next);
        rx_next = (rx_next + 1) % NRX;
    }
}

void wl_hw_tx_enable(struct wl_dev *d) { wr8(d, REG_TXPAUSE, 0x00); }

/* TX descriptor fields (rtl8192ce trx.h), little-endian words */
/* fill a TX descriptor; qsel 0x00 BE data, 0x12 management, 0x10 beacon */
static void fill_desc(struct desc *de, const uint8_t *buf, uint32_t len, uint8_t qsel, int bmc)
{
    uint32_t next = de->w[10];                               /* preserve the ring link */
    memset(de, 0, sizeof *de);
    de->w[10] = next;
    /* w0: pkt_size[15:0], offset=32 [23:16], bmc[24], last_seg[26], first_seg[27] */
    /* These are the exact descriptor values of the build a Windows PC joined
     * (examples/28 control run): rate_id 0xC, w4 = hwseq_en | use_rate | 0x1F<<16,
     * w5 = 0, no BMC.  The trx.h-derived layout (rate_id 7, fallback limits in
     * w5, BMC for multicast) is kept below for when it can be verified on air. */
    (void)bmc;
    de->w[0] = sw((len & 0xFFFF) | (0x20u << 16) | (1u << 26) | (1u << 27));
    /* w1 bits 23:22 = sec_type: 3 = AES-CCMP when the frame's Protected bit is set (chip encrypts) */
    uint32_t sec = (buf[1] & 0x40) ? (3u << 22) : 0;
    de->w[1] = sw(((uint32_t)qsel << 8) | (0xCu << 16) | sec);
    de->w[3] = sw((8u << 28));                               /* pkt_id 8 */
    de->w[4] = sw((1u << 7) | (1u << 8) | (0x1Fu << 16));
    de->w[5] = sw(0);                                        /* tx_rate = DESC_RATE1M (0) */
#if 0   /* trx.h layout, unverified on this board */
    de->w[0] = sw((len & 0xFFFF) | (0x20u << 16) | (bmc ? (1u << 24) : 0) | (1u << 26) | (1u << 27));
    de->w[1] = sw(((uint32_t)qsel << 8) | (7u << 16));
    de->w[4] = sw((1u << 7) | (1u << 8));
    de->w[5] = sw((0x1Fu << 8) | (0xFu << 13));
#endif
    de->w[7] = sw(len & 0xFFFF);                             /* tx buffer size */
    de->w[8] = sw(PHYS(buf));                                /* tx buffer address */
    de->w[0] = sw(sw(de->w[0]) | (1u << 31));                /* OWN last */
}

int wl_hw_beacon_update(struct wl_dev *d, const uint8_t *frame, uint32_t len)
{
    /* The beacon descriptor is persistent: the MAC re-sends it at every TBTT
     * and never clears OWN.  Like rtl_pci's beacon tasklet, refill it once per
     * interval, right after the beacon-DMA interrupt (HISR bit 13, BCNINT),
     * which is before the next TBTT. */
    struct desc *de = &bcn_ring[0];
    int first = !(sw(de->w[0]) & (1u << 31));
    uint32_t isr = rd32(d, REG_HISR);
    if (!first && !(isr & (1u << 13))) return 0;
    if (isr & (1u << 13)) wr32(d, REG_HISR, 1u << 13);
    memcpy(bcn_buf[0], frame, len);
    fill_desc(de, bcn_buf[0], len, 0x10, 1);
    (void)bcn_next;
    wl_wr16(d, REG_PCIE_CTRL_REG, 1u << 4);                  /* beacon queue poll */
    return 1;
}

uint32_t wl_hw_beacon_desc0(struct wl_dev *d) { (void)d; return sw(bcn_ring[0].w[0]); }

int wl_hw_tx(struct wl_dev *d, const uint8_t *frame, uint32_t len, int beacon, uint32_t timeout_ms)
{
    if (beacon) return wl_hw_beacon_update(d, frame, len) ? 0 : -1;
    uint32_t slot = tx_next;
    struct desc *de = &tx_ring[slot];
    uint8_t *buf = tx_buf[slot];
    if (sw(de->w[0]) & (1u << 31)) {                         /* still owned: wait */
        uint32_t t0 = micros();
        while ((sw(de->w[0]) & (1u << 31)) && elapsed_us(t0) < timeout_ms * 1000u) ;
        if (sw(de->w[0]) & (1u << 31)) return -1;
    }
    memcpy(buf, frame, len);
    int is_data = (frame[0] & 0x0C) == 0x08;
    fill_desc(de, buf, len, is_data ? 0x00 : 0x12, frame[4] & 1);
    wl_wr16(d, REG_PCIE_CTRL_REG, 1u << 1);                  /* BE queue poll (BIT(0) << queue 1) */
    tx_next = (tx_next + 1) % NTX;
    uint32_t t0 = micros();
    while ((sw(de->w[0]) & (1u << 31)) && elapsed_us(t0) < timeout_ms * 1000u) ;
    return (sw(de->w[0]) & (1u << 31)) ? -1 : 0;
}

void wl_hw_set_bssid(struct wl_dev *d, const uint8_t *bssid)
{
    for (int i = 0; i < 6; i++) wr8(d, REG_BSSID + i, bssid[i]);
    uint32_t rcr = rd32(d, REG_RCR);
    if (bssid) rcr |= RCR_CBSSID_DATA | RCR_CBSSID_BCN;
    wr32(d, REG_RCR, rcr);
}

void wl_hw_set_opmode(struct wl_dev *d, int type)
{
    uint8_t msr = rd8(d, MSR) & 0xFC;
    static const uint8_t mode[4] = { 0x00, 0x01, 0x02, 0x03 };   /* none/adhoc/sta/ap */
    if (type < 0 || type > 3) type = 0;
    if (type == 3 || type == 1) {                                /* AP/adhoc: beacon */
        wr8(d, REG_FWHW_TXQ_CTRL + 2, rd8(d, REG_FWHW_TXQ_CTRL + 2) | (1u << 6));
        wr8(d, REG_TBTT_PROHIB + 1, 0xFF);
        wr8(d, REG_TBTT_PROHIB + 2, rd8(d, REG_TBTT_PROHIB + 2) | 1u);
        /* rtl92ce: _disable_bcn_sub_func SETS bit 1, EN_BCN_FUNCTION bit 3 on, and
         * check_bssid clears DIS_TSF_UDT0 (bit 4) so the TSF/TBTT runs for beacons */
        wr8(d, REG_BCN_CTRL, (rd8(d, REG_BCN_CTRL) | (1u << 1) | (1u << 3)) & ~(1u << 4));
    }
    wr8(d, MSR, msr | mode[type]);
    wr8(d, REG_BCNTCFG + 1, type == 3 ? 0x00 : 0x66);
}

/* rtl92ce_set_beacon_related_registers + media status AP */
int wl_hw_ap_start(struct wl_dev *d, uint8_t chan)
{
    wl_hw_set_channel(d, chan);
    wl_wr16(d, REG_ATIMWND, 2);
    wl_wr16(d, REG_BCN_INTERVAL, 100);                      /* TU: TBTT every 102.4 ms */
    wl_wr16(d, REG_BCNTCFG, 0x660F);
    wr8(d, REG_RXTSF_CCK, 0x18);
    wr8(d, REG_RXTSF_OFDM, 0x18);
    wr8(d, 0x606, 0x30);
    wl_hw_set_bssid(d, d->mac);
    wl_hw_set_opmode(d, 3);
    wr8(d, 0x553, 0x03);                                     /* REG_DUAL_TSF_RST: restart the TSF, starts TBTT */
    return 0;
}
