/*
 * 24_wifi_fw - first Wi-Fi step: wake the RTL8192CE and start its firmware.
 *
 *  1. PCIe up (sdk/src/pcie.c), chip registers mapped at 0xB9000000
 *  2. byte-lane test: 8/16-bit register reads compared with the 32-bit one,
 *     to learn how narrow accesses map through the PCIe bridge
 *  3. MAC power-on (APS_FSMCO state machine), isolation off, clocks on
 *  4. firmware download into the chip's 8051 (4 KB pages at 0x1000), then
 *     wait for the checksum report and WINTINI_RDY (firmware running)
 *  5. PCIe powered down again
 * The radio is never configured, so nothing is transmitted.
 *
 * The firmware is Realtek's, taken from the stock flash backup by
 * tools/extract_wifi_blobs.py into sdk/include/wifi_blobs.inc.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "libc.h"
#include "wifi_blobs.inc"

/* RTL8192C registers (names as in Linux rtlwifi rtl8192c reg.h) */
#define REG_SYS_ISO_CTRL   0x0000
#define REG_SYS_FUNC_EN    0x0002
#define REG_APS_FSMCO      0x0004
#define REG_SYS_CLKR       0x0008
#define REG_9346CR         0x000A
#define REG_SPS0_CTRL      0x0011
#define REG_RSV_CTRL       0x001C
#define REG_AFE_XTAL_CTRL  0x0024
#define REG_MCUFWDL        0x0080
#define REG_SYS_CFG        0x00F0
#define REG_CR             0x0100
#define FW_START           0x1000
#define FW_PAGE            4096u

#define MCUFWDL_EN         (1u << 0)
#define MCUFWDL_RDY        (1u << 1)
#define FWDL_CHKSUM_RPT    (1u << 2)
#define WINTINI_RDY        (1u << 6)

static uint32_t mem;
static uint32_t x8, x16;                       /* address XOR for 8/16-bit accesses (0, or 3/2 if lanes swap) */

static uint32_t rd32(uint32_t r) { return REG32(mem + r); }
static void wr32(uint32_t r, uint32_t v) { REG32(mem + r) = v; }
static uint8_t rd8(uint32_t r) { return *(volatile uint8_t *)(mem + (r ^ x8)); }
static void wr8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(mem + (r ^ x8)) = v; }
static uint16_t rd16(uint32_t r) { return *(volatile uint16_t *)(mem + (r ^ x16)); }
static void wr16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(mem + (r ^ x16)) = v; }

static int lane_test(void)
{
    uint32_t d = rd32(REG_SYS_CLKR);
    uint8_t b[4];
    for (int k = 0; k < 4; k++) b[k] = *(volatile uint8_t *)(mem + REG_SYS_CLKR + k);
    uint16_t h0 = *(volatile uint16_t *)(mem + REG_SYS_CLKR), h2 = *(volatile uint16_t *)(mem + REG_SYS_CLKR + 2);
    printf("lanes: 32-bit @08 = %08x; bytes @08..0b = %02x %02x %02x %02x; halves @08,@0a = %04x %04x\n", d, b[0],
           b[1], b[2], b[3], h0, h2);
    int direct = 1, swapped = 1;
    for (int k = 0; k < 4; k++) {
        if (b[k] != (uint8_t)(d >> (8 * k))) direct = 0;
        if (b[k] != (uint8_t)(d >> (8 * (3 - k)))) swapped = 0;
    }
    if (direct && !swapped) { x8 = 0; x16 = 0; printf("lanes: little-endian addressing, no XOR\n"); }
    else if (swapped && !direct) { x8 = 3; x16 = 2; printf("lanes: byte address XOR 3, halfword XOR 2\n"); }
    else { printf("lanes: cannot tell (direct %d swapped %d)\n", direct, swapped); return -1; }
    uint16_t want0 = (uint16_t)d, got0 = rd16(REG_SYS_CLKR);
    printf("lanes: check 16-bit @08 = %04x (want %04x), 8-bit @0a = %02x (want %02x)\n", got0, want0,
           rd8(REG_9346CR), (uint8_t)(d >> 16));
    return got0 == want0 && rd8(REG_9346CR) == (uint8_t)(d >> 16) ? 0 : -1;
}

static void show(const char *when)
{
    printf("%-22s ISO %04x FUNC_EN %04x APS_FSMCO %08x CLKR %04x MCUFWDL %08x CR %04x\n", when,
           rd16(REG_SYS_ISO_CTRL), rd16(REG_SYS_FUNC_EN), rd32(REG_APS_FSMCO), rd16(REG_SYS_CLKR), rd32(REG_MCUFWDL),
           rd16(REG_CR));
}

/* rtl92ce _rtl92ce_init_mac, power-on part */
static int power_on(void)
{
    wr8(REG_RSV_CTRL, 0x00);
    wr8(REG_SPS0_CTRL, 0x2B);
    wr8(REG_AFE_XTAL_CTRL, 0x0F);
    delay_us(2);
    wr8(REG_APS_FSMCO + 1, rd8(REG_APS_FSMCO + 1) | 0x01);       /* APFM_ONMAC: start the power-on state machine */
    int n = 0;
    while ((rd8(REG_APS_FSMCO + 1) & 0x01) && n < 1000) { delay_us(50); n++; }   /* clears when done */
    printf("power-on state machine finished after %d polls%s\n", n, n >= 1000 ? " (TIMEOUT)" : "");
    wr16(REG_APS_FSMCO, 0x1012);
    wr8(REG_SYS_ISO_CTRL + 1, 0x82);
    delay_us(2);
    wr16(REG_CR, 0x02FF);                                         /* MAC function blocks on */
    return n < 1000 ? 0 : -1;
}

static int download_fw(void)
{
    const uint8_t *p = wifi_fw;
    uint32_t len = WIFI_FW_LEN;
    uint16_t sig = (uint16_t)(p[0] | (p[1] << 8));
    printf("firmware: signature %04x, version %u.%u, built %02u-%02u %02u:%02u, %u bytes\n", sig,
           p[4] | (p[5] << 8), p[6], p[8], p[9], p[10], p[11], len);
    if ((sig & 0xFFF0) == 0x92C0 || (sig & 0xFFF0) == 0x88C0) { p += 32; len -= 32; }   /* skip the header */

    if (rd8(REG_MCUFWDL) & 0x80) {                                /* RAM code already running: reset the 8051 */
        wr8(REG_MCUFWDL, 0x00);
        printf("firmware was running, MCU reset\n");
    }
    wr8(REG_SYS_FUNC_EN + 1, rd8(REG_SYS_FUNC_EN + 1) | 0x04);    /* FEN_CPUEN */
    wr8(REG_MCUFWDL, rd8(REG_MCUFWDL) | MCUFWDL_EN);
    wr8(REG_MCUFWDL + 2, rd8(REG_MCUFWDL + 2) & 0xF7);

    uint32_t t0 = micros();
    for (uint32_t off = 0, page = 0; off < len; off += FW_PAGE, page++) {
        wr8(REG_MCUFWDL + 2, (uint8_t)((rd8(REG_MCUFWDL + 2) & 0xF8) | (page & 7)));
        uint32_t n = len - off < FW_PAGE ? len - off : FW_PAGE;
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t v = 0;                                       /* little-endian word from the image, zero-padded */
            for (int k = 0; k < 4; k++) if (off + i + k < len) v |= (uint32_t)p[off + i + k] << (8 * k);
            wr32(FW_START + i, v);
        }
    }
    printf("firmware: %u bytes written in %u ms\n", len, elapsed_us(t0) / 1000u);
    wr8(REG_MCUFWDL, rd8(REG_MCUFWDL) & 0xFE);                    /* download done */
    wr8(REG_MCUFWDL + 1, 0x00);

    int n = 0;
    while (!(rd32(REG_MCUFWDL) & FWDL_CHKSUM_RPT) && n < 1000) { delay_us(50); n++; }
    printf("firmware: checksum report %s (MCUFWDL %08x)\n", n < 1000 ? "OK" : "MISSING", rd32(REG_MCUFWDL));
    if (n >= 1000) return -1;
    wr32(REG_MCUFWDL, (rd32(REG_MCUFWDL) | MCUFWDL_RDY) & ~WINTINI_RDY);
    t0 = micros();
    for (n = 0; n < 1000; n++) {
        if (rd32(REG_MCUFWDL) & WINTINI_RDY) break;
        delay_ms(1);
    }
    printf("firmware: %s after %u ms (MCUFWDL %08x)\n", n < 1000 ? "RUNNING (WINTINI_RDY)" : "did not start",
           elapsed_us(t0) / 1000u, rd32(REG_MCUFWDL));
    return n < 1000 ? 0 : -1;
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi step 1: power on the RTL8192CE and start its firmware\n");
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    uint32_t id = pcie_cfg_read(0);
    printf("pcie: link up, device %04x:%04x\n", id & 0xFFFF, id >> 16);
    mem = pcie_map_bar2();
    printf("chip: SYS_CFG %08x\n", rd32(REG_SYS_CFG));
    if (lane_test() < 0) { pcie_power_down(); printf("done\n"); return 1; }
    show("before power-on:");
    int rc = power_on();
    show("after power-on:");
    if (rc == 0) {
        rc = download_fw();
        show("after firmware:");
    }
    printf("RESULT: %s\n", rc == 0 ? "firmware running" : "FAILED");
    pcie_power_down();
    printf("PCIe powered down. done\n");
    return 0;
}
