/*
 * 25_wifi_scan - Wi-Fi step 2: passive scan with the RTL8192CE.
 *
 * Listens on channels 1-13 and lists the networks heard (beacons and probe
 * responses): BSSID, SSID, channel, signal, security.  RECEIVE ONLY: the TX
 * queues are paused, the MAC transmitter is left off and no TX descriptor is
 * ever handed to the chip, so nothing is sent.
 *
 * Bring-up order follows Linux rtlwifi rtl92ce_hw_init: MAC power-on, LLT and
 * DMA rings, firmware, MAC table, baseband (PHY_REG 2T + AGC), RF paths A/B,
 * LC calibration, 20 MHz.  Tables and firmware come from the user's own flash
 * backup: python tools/extract_wifi_blobs.py backups/flash_<date>.bin
 * (generates wifi_blobs.inc here, git-ignored).
 *
 * Runs uncached (default load 0xA0500000): rings and buffers need no cache care.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "libc.h"
#include "wifi_blobs.inc"

/* ------------------------------------------------------------ registers */
#define REG_SYS_ISO_CTRL   0x0000
#define REG_SYS_FUNC_EN    0x0002
#define REG_APS_FSMCO      0x0004
#define REG_SPS0_CTRL      0x0011
#define REG_RF_CTRL        0x001F
#define REG_RSV_CTRL       0x001C
#define REG_AFE_XTAL_CTRL  0x0024
#define REG_AFE_PLL_CTRL   0x0028
#define REG_HISR           0x0124
#define REG_LEDCFG0        0x004C
#define REG_MCUFWDL        0x0080
#define REG_HISRE          0x012C
#define REG_SYS_CFG        0x00F0
#define REG_CR             0x0100
#define REG_MSR            0x0102
#define REG_PBP            0x0104
#define REG_TRXDMA_CTRL    0x010C
#define REG_TRXFF_BNDY     0x0114
#define REG_MCUTST_1       0x01C0
#define REG_LLT_INIT       0x01E0
#define REG_RQPN           0x0200
#define REG_TDECTRL        0x0208
#define REG_RQPN_NPQ       0x0214
#define REG_PCIE_CTRL_REG  0x0300
#define REG_INT_MIG        0x0304
#define REG_BCNQ_DESA      0x0308
#define REG_HQ_DESA        0x0310
#define REG_MGQ_DESA       0x0318
#define REG_VOQ_DESA       0x0320
#define REG_VIQ_DESA       0x0328
#define REG_BEQ_DESA       0x0330
#define REG_BKQ_DESA       0x0338
#define REG_RX_DESA        0x0340
#define REG_FWHW_TXQ_CTRL  0x0420
#define REG_TXPKTBUF_BCNQ_BDNY 0x0424
#define REG_TXPKTBUF_MGQ_BDNY  0x0425
#define REG_TXPAUSE        0x0522
#define REG_APSD_CTRL      0x0600
#define REG_BWOPMODE       0x0603
#define REG_TCR            0x0604
#define REG_RCR            0x0608
#define REG_RX_DRVINFO_SZ  0x060F
#define REG_RXFLTMAP0      0x06A0
#define REG_RXFLTMAP2      0x06A4
#define FW_START           0x1000
#define FW_PAGE            4096u

/* baseband */
#define RFPGA0_RFMOD           0x800
#define RFPGA0_XA_HSSIPARAM1   0x820
#define RFPGA0_XA_HSSIPARAM2   0x824
#define RFPGA0_XB_HSSIPARAM1   0x828
#define RFPGA0_XB_HSSIPARAM2   0x82C
#define RFPGA0_XA_LSSIPARAM    0x840
#define RFPGA0_XB_LSSIPARAM    0x844
#define RFPGA0_XA_RFIFOE       0x860
#define RFPGA0_XB_RFIFOE       0x864
#define RFPGA0_XAB_RFIFSW      0x870
#define RFPGA0_ANALOGPARAM2    0x884
#define RFPGA0_XA_LSSIREADBACK 0x8A0
#define RFPGA0_XB_LSSIREADBACK 0x8A4
#define TRANSCEIVEA_HSPI_RB    0x8B8
#define TRANSCEIVEB_HSPI_RB    0x8BC
#define RFPGA1_RFMOD           0x900

/* RCR bits */
#define RCR_AAP            (1u << 0)
#define RCR_APM            (1u << 1)
#define RCR_AM             (1u << 2)
#define RCR_AB             (1u << 3)
#define RCR_ACRC32         (1u << 8)
#define RCR_AMF            (1u << 13)
#define RCR_HTC_LOC_CTRL   (1u << 14)
#define RCR_APP_PHYST_RXFF (1u << 28)
#define RCR_APPFCS         (1u << 31)

/* --------------------------------------------------------- access helpers */
static uint32_t mem;
static uint32_t rd32(uint32_t r) { return REG32(mem + r); }
static void wr32(uint32_t r, uint32_t v) { REG32(mem + r) = v; }
static uint8_t rd8(uint32_t r) { return *(volatile uint8_t *)(mem + r); }
static void wr8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(mem + r) = v; }
static uint16_t rd16(uint32_t r) { return *(volatile uint16_t *)(mem + r); }
static void wr16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(mem + r) = v; }

static int lowbit(uint32_t m) { int s = 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static void bb_set(uint32_t r, uint32_t mask, uint32_t v)
{
    wr32(r, (rd32(r) & ~mask) | ((v << lowbit(mask)) & mask));
}
static uint32_t bb_get(uint32_t r, uint32_t mask) { return (rd32(r) & mask) >> lowbit(mask); }

/* RF registers through the 3-wire serial interface (LSSI), 20-bit values */
static const uint32_t rf_lssi[2] = { RFPGA0_XA_LSSIPARAM, RFPGA0_XB_LSSIPARAM };
static const uint32_t rf_hssi1[2] = { RFPGA0_XA_HSSIPARAM1, RFPGA0_XB_HSSIPARAM1 };
static const uint32_t rf_hssi2[2] = { RFPGA0_XA_HSSIPARAM2, RFPGA0_XB_HSSIPARAM2 };
static const uint32_t rf_rb[2] = { RFPGA0_XA_LSSIREADBACK, RFPGA0_XB_LSSIREADBACK };
static const uint32_t rf_rbpi[2] = { TRANSCEIVEA_HSPI_RB, TRANSCEIVEB_HSPI_RB };

static void rf_write(int path, uint32_t reg, uint32_t val)
{
    wr32(rf_lssi[path], ((reg & 0x3Fu) << 20) | (val & 0xFFFFFu));
    delay_us(1);
}

static uint32_t rf_read(int path, uint32_t reg)
{
    uint32_t a = rd32(RFPGA0_XA_HSSIPARAM2);
    uint32_t b = path ? rd32(rf_hssi2[path]) : a;
    b = (b & ~0x7F800000u) | ((reg & 0x3Fu) << 23) | 0x80000000u;
    wr32(RFPGA0_XA_HSSIPARAM2, a & ~0x80000000u);
    delay_ms(1);
    wr32(rf_hssi2[path], b);
    delay_ms(1);
    wr32(RFPGA0_XA_HSSIPARAM2, a | 0x80000000u);
    delay_ms(1);
    int pi = (rd32(rf_hssi1[path]) >> 8) & 1;
    return rd32(pi ? rf_rbpi[path] : rf_rb[path]) & 0xFFFFFu;
}

/* pseudo registers 0xFE..0xF9 in the tables are delays */
static int table_delay(uint32_t reg)
{
    switch (reg) {
    case 0xFE: delay_ms(50); return 1;
    case 0xFD: delay_ms(5); return 1;
    case 0xFC: delay_ms(1); return 1;
    case 0xFB: delay_us(50); return 1;
    case 0xFA: delay_us(5); return 1;
    case 0xF9: delay_us(1); return 1;
    default: return 0;
    }
}

/* ----------------------------------------------------------------- rings */
#ifndef NRX
#define NRX     32
#endif
#define RXBUF   2048
struct desc { uint32_t w[8]; };                    /* 32 bytes, little-endian words */
static struct desc rx_ring[NRX] __attribute__((aligned(256)));
static struct desc tx_dummy[2] __attribute__((aligned(256)));   /* empty TX rings: never handed to the chip */
static uint8_t rx_buf[NRX][RXBUF] __attribute__((aligned(32)));
static uint32_t rx_next;

static uint32_t sw(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
#define PHYS(p) ((uint32_t)(p) & 0x1FFFFFFFu)

static void rx_arm(uint32_t i)
{
    rx_ring[i].w[6] = sw(PHYS(rx_buf[i]));
    rx_ring[i].w[0] = sw((1u << 31) | (i == NRX - 1 ? (1u << 30) : 0) | RXBUF);   /* OWN, EOR, size */
}

/* ------------------------------------------------------------- bring-up */
static int power_on(void)
{
    wr8(REG_RSV_CTRL, 0x00);
    wr8(REG_SPS0_CTRL, 0x2B);
    wr8(REG_AFE_XTAL_CTRL, 0x0F);
    delay_us(2);
    wr8(REG_APS_FSMCO + 1, rd8(REG_APS_FSMCO + 1) | 0x01);
    int n = 0;
    while ((rd8(REG_APS_FSMCO + 1) & 0x01) && n < 1000) { delay_us(50); n++; }
    if (n >= 1000) return -1;
    wr16(REG_APS_FSMCO, 0x1012);
    wr8(REG_SYS_ISO_CTRL + 1, 0x82);
    delay_us(2);
    wr16(REG_CR, 0x02FF);
    return 0;
}

static int llt_write(uint32_t addr, uint32_t data)
{
    wr32(REG_LLT_INIT, (1u << 30) | ((addr & 0xFF) << 8) | (data & 0xFF));
    for (int n = 0; n < 100; n++) if (((rd32(REG_LLT_INIT) >> 30) & 3) == 0) return 0;
    return -1;
}

static int init_mac(void)
{
    const uint32_t bndy = 0xF6, max_page = 0xFF;
    /* packet buffer: the values in the stock MACPHY_REG table's header */
    wr32(REG_RQPN, 0x80A32929u);
    wr32(REG_TRXFF_BNDY, 0x27FF0000u | bndy);
    wr8(REG_TDECTRL + 1, (uint8_t)bndy);
    wr8(REG_TXPKTBUF_BCNQ_BDNY, (uint8_t)bndy);
    wr8(REG_TXPKTBUF_MGQ_BDNY, (uint8_t)bndy);
    wr8(0x45D, (uint8_t)bndy);
    wr8(REG_PBP, 0x11);
    wr8(REG_RX_DRVINFO_SZ, 0x04);                                   /* 32 bytes of PHY status per frame */
    int err = 0;
    for (uint32_t i = 0; i < bndy - 1; i++) err |= llt_write(i, i + 1);
    err |= llt_write(bndy - 1, 0xFF);
    for (uint32_t i = bndy; i < max_page; i++) err |= llt_write(i, i + 1);
    err |= llt_write(max_page, bndy);
    if (err) { printf("LLT init failed\n"); return -1; }

    wr32(REG_HISR, 0xFFFFFFFFu);
    wr8(REG_HISRE, 0xFF);
    wr16(REG_TRXFF_BNDY + 2, 0x27FF);
    wr16(REG_TRXDMA_CTRL, (uint16_t)((rd16(REG_TRXDMA_CTRL) & 0xF) | 0xF771));
    wr8(REG_FWHW_TXQ_CTRL + 1, 0x1F);

    memset(tx_dummy, 0, sizeof tx_dummy);
    uint32_t t = PHYS(tx_dummy);
    wr32(REG_BCNQ_DESA, t); wr32(REG_MGQ_DESA, t); wr32(REG_VOQ_DESA, t); wr32(REG_VIQ_DESA, t);
    wr32(REG_BEQ_DESA, t); wr32(REG_BKQ_DESA, t); wr32(REG_HQ_DESA, t);
    for (uint32_t i = 0; i < NRX; i++) rx_arm(i);
    rx_next = 0;
    wr32(REG_RX_DESA, PHYS(rx_ring));
    wr8(REG_PCIE_CTRL_REG + 3, 0x77);
    wr32(REG_INT_MIG, 0);
    wr8(REG_APSD_CTRL, rd8(REG_APSD_CTRL) & ~0x40);
    for (int n = 0; n < 200 && (rd8(REG_APSD_CTRL) & 0x80); n++) delay_us(10);
    wr32(REG_MCUTST_1, 0);
    return 0;
}

static int download_fw(void)
{
    const uint8_t *p = wifi_fw + 32;                                /* skip the 32-byte header */
    uint32_t len = WIFI_FW_LEN - 32;
    if (rd8(REG_MCUFWDL) & 0x80) wr8(REG_MCUFWDL, 0x00);
    wr8(REG_SYS_FUNC_EN + 1, rd8(REG_SYS_FUNC_EN + 1) | 0x04);
    wr8(REG_MCUFWDL, rd8(REG_MCUFWDL) | 0x01);
    wr8(REG_MCUFWDL + 2, rd8(REG_MCUFWDL + 2) & 0xF7);
    for (uint32_t off = 0, page = 0; off < len; off += FW_PAGE, page++) {
        wr8(REG_MCUFWDL + 2, (uint8_t)((rd8(REG_MCUFWDL + 2) & 0xF8) | (page & 7)));
        uint32_t n = len - off < FW_PAGE ? len - off : FW_PAGE;
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) if (off + i + k < len) v |= (uint32_t)p[off + i + k] << (8 * k);
            wr32(FW_START + i, v);
        }
    }
    wr8(REG_MCUFWDL, rd8(REG_MCUFWDL) & 0xFE);
    wr8(REG_MCUFWDL + 1, 0x00);
    int n = 0;
    while (!(rd32(REG_MCUFWDL) & 0x04) && n < 1000) { delay_us(50); n++; }
    if (n >= 1000) return -1;
    wr32(REG_MCUFWDL, (rd32(REG_MCUFWDL) | 0x02) & ~0x40u);
    for (n = 0; n < 1000; n++) { if (rd32(REG_MCUFWDL) & 0x40) return 0; delay_ms(1); }
    return -2;
}

static void config_bb(void)
{
    /* rtl8192ce phy.c rtl92c_phy_bb_config */
    wr16(REG_SYS_FUNC_EN, rd16(REG_SYS_FUNC_EN) | (1u << 13) | 0x3);
    wr8(REG_AFE_PLL_CTRL, 0x83);                                    /* AFE PLL: ADC/DAC clocks (320 MHz) */
    wr8(REG_AFE_PLL_CTRL + 1, 0xDB);
    wr8(REG_RF_CTRL, 0x07);                                         /* RF_EN | RF_RSTB | RF_SDMRSTB */
    wr8(REG_SYS_FUNC_EN, 0xE3);                                     /* PPLL, PCIEA, DIO_PCIE, BB_GLB_RSTn, BBRSTB */
    wr8(REG_AFE_XTAL_CTRL + 1, 0x80);
    wr32(REG_LEDCFG0, rd32(REG_LEDCFG0) | (1u << 23));             /* antenna select pins */
#ifndef USE_2T
#define USE_2T 1                       /* the chip is a 2T2R RTL8192CE */
#endif
    const uint32_t (*phy)[2] = USE_2T ? wifi_phy_2t : wifi_phy_1t;
    uint32_t nphy = USE_2T ? WIFI_PHY_2T_N : WIFI_PHY_1T_N;
    for (uint32_t i = 0; i < nphy; i++)
        if (!table_delay(phy[i][0])) { wr32(phy[i][0], phy[i][1]); delay_us(1); }
    for (uint32_t i = 0; i < WIFI_PHY_FIX_N; i++) { wr32(wifi_phy_fix[i][0], wifi_phy_fix[i][1]); delay_us(1); }
    for (uint32_t i = 0; i < WIFI_AGC_N; i++) { wr32(wifi_agc[i][0], wifi_agc[i][1]); delay_us(1); }
}

static void config_rf(void)
{
    for (int path = 0; path < 2; path++) {
        uint32_t envmask = path ? 0x10u << 16 : 0x10u;              /* BRFSI_RFENV */
        uint32_t ife = path ? RFPGA0_XB_RFIFOE : RFPGA0_XA_RFIFOE;
        uint32_t saved = bb_get(RFPGA0_XAB_RFIFSW, envmask);
        bb_set(ife, 0x10u << 16, 1);
        delay_us(1);
        bb_set(ife, 0x10u, 1);
        delay_us(1);
        bb_set(rf_hssi2[path], 0x400, 0);                           /* 3-wire address length */
        delay_us(1);
        bb_set(rf_hssi2[path], 0x800, 0);                           /* 3-wire data length */
        delay_us(1);
        const uint32_t (*t)[2] = path ? wifi_radio_b : wifi_radio_a;
        uint32_t n = path ? WIFI_RADIO_B_N : WIFI_RADIO_A_N;
        for (uint32_t i = 0; i < n; i++)
            if (!table_delay(t[i][0])) rf_write(path, t[i][0], t[i][1]);
        bb_set(RFPGA0_XAB_RFIFSW, envmask, saved);
    }
}

static uint32_t chnlval[2];

static void lc_calibrate(void)
{
    uint8_t d03 = rd8(0xD03);
    uint32_t mode[2] = { 0, 0 };
    if (d03 & 0x70) {
        wr8(0xD03, d03 & 0x8F);
        for (int p = 0; p < 2; p++) { mode[p] = rf_read(p, 0x00); rf_write(p, 0x00, (mode[p] & 0x8FFFF) | 0x10000); }
    } else {
        wr8(REG_TXPAUSE, 0xFF);
    }
    uint32_t v = rf_read(0, 0x18);
    rf_write(0, 0x18, v | 0x08000);                                 /* start LC calibration */
    delay_ms(100);
    if (d03 & 0x70) {
        wr8(0xD03, d03);
        for (int p = 0; p < 2; p++) rf_write(p, 0x00, mode[p]);
    }
    wr8(REG_TXPAUSE, 0xFF);                                         /* keep every TX queue paused */
}

static void set_channel(uint32_t ch)
{
    for (int p = 0; p < 2; p++) rf_write(p, 0x18, (chnlval[p] & ~0x3FFu) | ch);
}

/* ------------------------------------------------------------ scanning */
struct bss {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel, sec;          /* sec: 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2 */
    int      rssi;
    uint32_t count;
};
static struct bss bss[64];
static uint32_t nbss;
static struct { uint32_t frames, crc_err, beacons, other, too_long; } st;

static int rssi_of(const uint8_t *phy, uint32_t mcs, int cck_highpwr)
{
    if (mcs < 4) {                                                  /* CCK rates */
        uint8_t agc = phy[5];
        static const int base[4] = { 16, -12, -26, -46 };
        if (!cck_highpwr) return base[(agc & 0xC0) >> 6] - (agc & 0x3E);
        return base[(agc & 0x60) >> 5] - ((agc & 0x1F) << 1);
    }
    return ((phy[4] >> 1) & 0x7F) - 110;                            /* OFDM: pwdb_all */
}

static void parse_beacon(const uint8_t *f, uint32_t len, uint32_t ch, int rssi)
{
    if (len < 36 + 4) return;
    len -= 4;                                                       /* FCS */
    const uint8_t *bssid = f + 16;
    uint16_t cap = (uint16_t)(f[34] | (f[35] << 8));
    char ssid[33] = "";
    uint8_t chan = (uint8_t)ch, rsn = 0, wpa = 0;
    for (uint32_t o = 36; o + 2 <= len;) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > len) break;
        const uint8_t *v = f + o + 2;
        if (id == 0 && l <= 32) {
            for (uint32_t k = 0; k < l; k++) ssid[k] = (v[k] >= 32 && v[k] < 127) ? (char)v[k] : '?';
            ssid[l] = 0;
        }
        if (id == 3 && l >= 1) chan = v[0];
        if (id == 48) rsn = 1;
        if (id == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) wpa = 1;
        o += 2u + l;
    }
    uint8_t sec = (cap & 0x10) ? (rsn && wpa ? 4 : rsn ? 3 : wpa ? 2 : 1) : 0;
    struct bss *b = 0;
    for (uint32_t i = 0; i < nbss; i++) if (!memcmp(bss[i].bssid, bssid, 6)) b = &bss[i];
    if (!b) {
        if (nbss >= sizeof bss / sizeof bss[0]) return;
        b = &bss[nbss++];
        memset(b, 0, sizeof *b);
        memcpy(b->bssid, bssid, 6);
        b->rssi = -200;
    }
    memcpy(b->ssid, ssid, sizeof ssid);
    b->channel = chan;
    b->sec = sec;
    if (rssi > b->rssi) b->rssi = rssi;
    b->count++;
}

/* RX descriptor word 0: length 0-13, CRC error 14, drv_info size 16-19 (units of 8 bytes: the PHY
 * status in front of the frame), shift 24-25, OWN 31; word 3 bits 0-5: rate (0-3 = CCK) */
static void rx_poll(uint32_t ch, int cck_highpwr)
{
    uint32_t isr = rd32(REG_HISR), isre = rd32(REG_HISRE);         /* acknowledge like an interrupt handler */
    if (isr) wr32(REG_HISR, isr);
    if (isre) wr32(REG_HISRE, isre);
    for (int budget = 0; budget < NRX; budget++) {
        struct desc *d = &rx_ring[rx_next];
        uint32_t w0 = sw(d->w[0]);
        if (w0 & (1u << 31)) return;                                /* still owned by the chip */
        uint32_t len = w0 & 0x3FFF, drv = (w0 >> 16) & 0xF, shift = (w0 >> 24) & 3, mcs = sw(d->w[3]) & 0x3F;
        st.frames++;
        if (w0 & (1u << 14)) st.crc_err++;
        else if (drv * 8 + shift + len > RXBUF) st.too_long++;
        else {
            const uint8_t *buf = rx_buf[rx_next];
            const uint8_t *f = buf + drv * 8 + shift;
            int rssi = drv ? rssi_of(buf, mcs, cck_highpwr) : -100;  /* PHY status = the drv_info bytes */
            if (len >= 24 && (f[0] == 0x80 || f[0] == 0x50)) { st.beacons++; parse_beacon(f, len, ch, rssi); }
            else st.other++;
        }
        rx_arm(rx_next);
        rx_next = (rx_next + 1) % NRX;
    }
}

static const char *sec_name(uint8_t s)
{
    static const char *const n[] = { "open", "WEP", "WPA", "WPA2", "WPA/WPA2" };
    return s < 5 ? n[s] : "?";
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi step 2: passive scan (receive only)\n");
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    mem = pcie_map_bar2();
    printf("chip %04x:%04x, SYS_CFG %08x\n", pcie_cfg_read(0) & 0xFFFF, pcie_cfg_read(0) >> 16, rd32(REG_SYS_CFG));
#ifndef STOCK_PCIE_TUNING
#define STOCK_PCIE_TUNING 1
#endif
    if (STOCK_PCIE_TUNING && REG32(0xB8000000) == 0x80000002u) {
        /* stock rtl8192cd_init_one (kernel 0x80004a58) on 40 MHz RTL8196C boards: root-port PHY MDIO
         * reg 15 = 0x0F0F, then the Wi-Fi chip's own PCIe PHY through its MDIO window (0x354 data,
         * 0x358 = 0x20 | reg) */
        REG32(0xB8B01000) = 0x0F0F0F01u;
        delay_ms(5);
        (void)REG32(0xB8B01000);
        static const uint16_t ephy[3][2] = { { 4, 0xC940 }, { 5, 0x4270 }, { 3, 0x019E } };
        for (int i = 0; i < 3; i++) { wr32(0x354, ephy[i][1]); wr32(0x358, 0x20u | ephy[i][0]); delay_ms(5); }
        REG8(0xB8B00078) = (uint8_t)(pcie_cfg_read(0x78) & 0x1F);  /* root port device control, as stock */
        printf("PCIe: stock 40 MHz tuning applied (root port PHY 15, Wi-Fi EPHY 3/4/5); RC 78 %08x EP 78 %08x\n",
               REG32(0xB8B00078), pcie_cfg_read(0x78));
    }

    if (power_on() < 0) { printf("power-on failed\n"); goto out; }
    if (init_mac() < 0) goto out;
    wr8(REG_TXPAUSE, 0xFF);                                         /* pause every TX queue */
#ifndef SKIP_FW
#define SKIP_FW 0
#endif
    if (!SKIP_FW) {
        int rc = download_fw();
        printf("firmware: %s (MCUFWDL %08x)\n", rc == 0 ? "running" : "FAILED", rd32(REG_MCUFWDL));
        if (rc < 0) goto out;
    } else {
        printf("firmware: NOT loaded (diagnostic build)\n");
    }

    for (uint32_t i = 0; i < WIFI_MAC_N; i++) wr8(wifi_mac[i][0], (uint8_t)wifi_mac[i][1]);
    config_bb();
    config_rf();
    for (int p = 0; p < 2; p++) chnlval[p] = rf_read(p, 0x18);
    printf("RF: path A reg00 %05x reg18 %05x, path B reg00 %05x reg18 %05x\n", rf_read(0, 0), chnlval[0],
           rf_read(1, 0), chnlval[1]);
    bb_set(RFPGA0_RFMOD, 1u << 24, 1);                              /* CCK on */
    bb_set(RFPGA0_RFMOD, 1u << 25, 1);                              /* OFDM on */
    bb_set(RFPGA0_ANALOGPARAM2, 1u << 10, 1);
    bb_set(RFPGA0_XB_RFIFOE, 0x60, 1);                              /* 2T: main antenna path */
    lc_calibrate();

    /* 20 MHz */
    wr8(REG_BWOPMODE, rd8(REG_BWOPMODE) | 0x04);
    bb_set(RFPGA0_RFMOD, 1, 0);
    bb_set(RFPGA1_RFMOD, 1, 0);
    for (int p = 0; p < 2; p++) chnlval[p] = (chnlval[p] & ~0xC00u) | 0x400u;
    int cck_highpwr = (int)bb_get(RFPGA0_XA_HSSIPARAM2, 0x200);
    printf("BB: 800 %08x 804 %08x c04 %08x d04 %08x, CCK high power %d\n", rd32(0x800), rd32(0x804), rd32(0xC04),
           rd32(0xD04), cck_highpwr);

    /* receive everything, beacons from every network.  The MAC transmitter must stay enabled: with
     * CR.MACTXEN off the MAC receives nothing at all (measured).  Nothing is queued for sending: every
     * TX queue is paused and no TX descriptor exists; the MAC could only ACK a frame addressed to its
     * own address, a random locally administered one that no device knows. */
    static const uint8_t own_mac[6] = { 0x02, 0x19, 0x6C, 0x00, 0x00, 0x02 };
    for (int i = 0; i < 6; i++) { wr8(0x610 + i, own_mac[i]); wr8(0x618 + i, 0); }
    wr8(REG_TXPAUSE, 0xFF);
    wr16(REG_RXFLTMAP0, 0xFFFF);
    wr16(REG_RXFLTMAP2, 0xFFFF);
    wr32(REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_ACRC32 | RCR_AMF | RCR_HTC_LOC_CTRL |
                  RCR_APP_PHYST_RXFF | RCR_APPFCS);
    wr8(REG_MSR, rd8(REG_MSR) & ~0x03);                             /* no link */
    chnlval[1] = chnlval[0];               /* path B's RF read-back is unreliable; both paths share the value */
    printf("RX: CR %04x RCR %08x TXPAUSE %02x\n", rd16(REG_CR), rd32(REG_RCR), rd8(REG_TXPAUSE));


    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t ch = 1; ch <= 13; ch++) {
            set_channel(ch);
            uint32_t f0 = st.frames, b0 = st.beacons, c0 = st.crc_err, t0 = micros();
            while (elapsed_us(t0) < 400000u) rx_poll(ch, cck_highpwr);
            printf("  pass %d ch %2u: %3u frames, %3u beacons, %3u CRC errors\n", pass + 1, ch, st.frames - f0,
                   st.beacons - b0, st.crc_err - c0);
        }
    }
    printf("total: %u frames, %u beacons/probe responses, %u other, %u CRC errors, %u too long\n", st.frames,
           st.beacons, st.other, st.crc_err, st.too_long);
    printf("%u networks:\n", nbss);
    printf("  BSSID              ch  signal  security  beacons  SSID\n");
    for (uint32_t i = 0; i < nbss; i++) {
        const struct bss *b = &bss[i];
        printf("  %02x:%02x:%02x:%02x:%02x:%02x  %2u  %4d dBm  %-8s  %5u    %s\n", b->bssid[0], b->bssid[1],
               b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], b->channel, b->rssi, sec_name(b->sec), b->count,
               b->ssid[0] ? b->ssid : "(hidden)");
    }
out:
    pcie_power_down();
    printf("PCIe powered down. done\n");
    return 0;
}
