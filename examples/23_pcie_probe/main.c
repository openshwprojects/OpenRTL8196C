/*
 * 23_pcie_probe - bring up the RTL8196C PCIe root port and identify what is
 * attached (on the LT-6408n: the Wi-Fi chip).  Read-only towards the device:
 * only its configuration space is read, then the port is powered down again.
 *
 * The sequence is the stock firmware's PCIE_reset_procedure (port 0),
 * disassembled from the LT-6408n kernel (flash backup, kernel 0x80138f94):
 *   1. CLK_MANAGE bit 11: PCIe clock on, 100 ms
 *   2. 0xB8000050 = 8, 9, 0xB: PHY MDIO reset, load done
 *   3. PHY control 0xB8B01008 = 1, 0x81: PHY reset, LTSSM enable
 *   4. PHY parameters over MDIO (0xB8B01000 = reg<<8 | val<<16 | 1), one of
 *      two sets chosen by 0xB8000000 == 0x80000001 (else "40MHz clock source")
 *   5. CLK_MANAGE bit 12 = PERST: low 300 ms, PHY reset again, high, 100 ms
 *   6. wait for LTSSM state 0x11 (L0) in 0xB8B00728, up to ~10 s
 *   7. root port command register 0xB8B00004 = 0x00100007
 * Device configuration space appears at 0xB8B10000 once the link is up.
 * For a Realtek device, BAR2 is then set like the stock driver does (chip
 * registers at 0xB9000000) and SYS_CFG / HPON_FSM are decoded the way the
 * stock check_chipID does (kernel 0x8015ba00): RTL8192C / RTL8188C / RTL8188R.
 * The "as read" decode is the right one: memory reads arrive in normal order.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"

#define SYS_ID          0xB8000000u
#define CLK_MANAGE      0xB8000010u
#define CLK_PCIE0       (1u << 11)
#define CLK_PCIE0_PERST (1u << 12)       /* 1 = device out of reset */
#define SYS_PCIE_PHY0   0xB8000050u
#define RC_CFG          0xB8B00000u      /* root port configuration space */
#define RC_LTSSM        (RC_CFG + 0x728u)
#define PHY_MDIO        (RC_CFG + 0x1000u)
#define PHY_CTRL        (RC_CFG + 0x1008u)
#define EP_CFG          0xB8B10000u      /* bus 1 device 0 configuration space */

static void mdio_write(uint32_t reg, uint32_t val)
{
    REG32(PHY_MDIO) = ((reg & 0x1Fu) << 8) | (val << 16) | 1u;
    delay_ms(1);
}

static const char *chip_name(uint16_t ven, uint16_t dev)
{
    if (ven != 0x10EC) return "not Realtek";
    /* IDs as in Linux drivers/net/wireless/realtek/rtlwifi */
    switch (dev) {
    case 0x8176: case 0x8177: case 0x8178: case 0x8191:
        return "RTL8192CE family: RTL8188CE / RTL8188RE / RTL8192CE (802.11n 2.4 GHz)";
    case 0x8171: case 0x8172: case 0x8173: case 0x8174: case 0x8192:
        return "RTL8192SE family";
    case 0x8179: return "RTL8188EE";
    case 0x818B: return "RTL8192EE";
    case 0x8812: return "RTL8812AE";
    default: return "unknown Realtek device";
    }
}

static const char *class_name(uint32_t cls)
{
    switch (cls >> 8) {
    case 0x0280: return "network controller (other: wireless)";
    case 0x0200: return "Ethernet controller";
    case 0x0604: return "PCI bridge";
    default: return "?";
    }
}

static void dump_cfg(const char *what, uint32_t base, int words)
{
    printf("%s configuration space at %08x:\n", what, base);
    for (int i = 0; i < words; i += 4)
        printf("  %02x: %08x %08x %08x %08x\n", i * 4, REG32(base + i * 4), REG32(base + i * 4 + 4),
               REG32(base + i * 4 + 8), REG32(base + i * 4 + 12));
}

static uint32_t swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

static void decode_variant(const char *order, uint32_t sys_cfg, uint32_t hpon)
{
    /* the stock rtl8192cd check_chipID (kernel 0x8015ba00) */
    const char *name;
    if (sys_cfg & (1u << 27)) name = "RTL8192C (2T2R: two transmit + two receive paths)";
    else if (((hpon >> 22) & 3u) == 1) name = "RTL8188R (1T1R, high-power version, 'RTL8188RE')";
    else name = "RTL8188C (1T1R: one transmit + one receive path)";
    printf("  %s: SYS_CFG %08x, HPON_FSM %08x -> %s%s, cut %x\n", order, sys_cfg, hpon, name,
           (sys_cfg & (1u << 23)) ? " (test chip)" : "", (sys_cfg >> 12) & 0xFu);
}

/* map BAR2 like the stock driver (memory window 0xB9000000 = PCIe 0x19000000)
 * and read the chip's SYS_CFG (0xF0) and HPON_FSM (0xEC) registers */
static void identify_wifi(void)
{
    const uint32_t mem = 0xB9000000u;
    REG32(EP_CFG + 0x18) = mem & 0x1FFFFFFFu;                  /* BAR2 */
    REG32(EP_CFG + 0x1C) = 0;                                  /* BAR2 upper half (64-bit BAR) */
    REG32(EP_CFG + 0x04) = (REG32(EP_CFG + 0x04) & 0xFFFF0000u) | 7u;   /* I/O, memory, bus master */
    delay_ms(10);
    printf("BAR2 now %08x, command %04x; chip registers at %08x:\n", REG32(EP_CFG + 0x18),
           REG32(EP_CFG + 4) & 0xFFFF, mem);
    for (uint32_t r = 0; r < 0x10; r += 4) printf("  %02x: %08x", r, REG32(mem + r));
    printf("\n");
    uint32_t cfg = REG32(mem + 0xF0), hpon = REG32(mem + 0xEC);
    printf("raw words: 0xF0 = %08x, 0xEC = %08x; decoded both ways:\n", cfg, hpon);
    decode_variant("as read     ", cfg, hpon);
    decode_variant("byte-swapped", swap32(cfg), swap32(hpon));
}

static void power_down(void)
{
    /* the stock kernel's power-off: device to D3hot, PHY parameter, clock off */
    REG32(EP_CFG + 0x44) = (REG32(EP_CFG + 0x44) & ~3u) | 3u;
    mdio_write(0x0F, 0x0708);
    REG32(CLK_MANAGE) &= ~CLK_PCIE0;
}

int main(void)
{
    uart_init();
    timer_delay_init();
    uint32_t id = REG32(SYS_ID);
    printf("\n\npcie probe: system id %08x, CLK_MANAGE %08x\n", id, REG32(CLK_MANAGE));

    REG32(CLK_MANAGE) |= CLK_PCIE0;
    delay_ms(100);
    REG32(SYS_PCIE_PHY0) = 0x8;
    REG32(SYS_PCIE_PHY0) = 0x9;
    REG32(SYS_PCIE_PHY0) = 0xB;
    REG32(PHY_CTRL) = 0x1;
    REG32(PHY_CTRL) = 0x81;
    delay_ms(200);

    if (id == 0x80000001u) {
        printf("PHY parameters: set A (system id 80000001)\n");
        static const uint16_t a[][2] = { { 0x0, 0x5027 }, { 0x2, 0x6D18 }, { 0x6, 0x8828 }, { 0x7, 0x30FF },
            { 0x8, 0x18D7 }, { 0xA, 0x00E9 }, { 0xB, 0x0511 }, { 0xD, 0x15B6 }, { 0xF, 0x0F0F }, { 0x5, 0x0BCB },
            { 0x6, 0x8128 } };
        for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) mdio_write(a[i][0], a[i][1]);
    } else {
        printf("PHY parameters: set B (\"96C - 40MHz Clock Source\")\n");
        static const uint16_t b[][2] = { { 0x0, 0xD087 }, { 0x1, 0x0003 }, { 0x2, 0x4D18 }, { 0x5, 0x0BCB },
            { 0x6, 0xF148 }, { 0x7, 0x31FF }, { 0x8, 0x18D7 }, { 0x9, 0x539C }, { 0xA, 0x20EB }, { 0xB, 0x0511 },
            { 0xD, 0x1764 }, { 0xF, 0x0A00 }, { 0x19, 0xFCE0 }, { 0x1E, 0xC280 } };
        for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) mdio_write(b[i][0], b[i][1]);
    }

    REG32(CLK_MANAGE) &= ~CLK_PCIE0_PERST;                    /* hold the device in reset */
    delay_ms(300);
    REG32(PHY_CTRL) = 0x1;
    REG32(PHY_CTRL) = 0x81;
    delay_ms(300);
    REG32(CLK_MANAGE) |= CLK_PCIE0_PERST;                     /* release it */
    delay_ms(100);

    uint32_t t0 = micros(), last = 0xFFFFFFFFu, st = 0;
    for (int i = 0; i < 100; i++) {
        st = REG32(RC_LTSSM) & 0x1Fu;
        if (st != last) { printf("  %5u ms: LTSSM state %02x%s\n", elapsed_us(t0) / 1000u, st, st == 0x11 ? " (L0, link up)" : ""); last = st; }
        if (st == 0x11) break;
        delay_ms(100);
    }
    dump_cfg("root port", RC_CFG, 16);
    if (st != 0x11) {
        printf("no link: nothing answers on the PCIe port (state %02x, raw %08x)\n", st, REG32(RC_LTSSM));
        REG32(CLK_MANAGE) &= ~CLK_PCIE0;
        printf("done\n");
        return 0;
    }
    REG32(RC_CFG + 4) = 0x00100007u;                          /* root port: I/O, memory, bus master */

    uint32_t w0 = REG32(EP_CFG), w2 = REG32(EP_CFG + 8), w11 = REG32(EP_CFG + 0x2C);
    uint16_t ven = (uint16_t)w0, dev = (uint16_t)(w0 >> 16);
    if (ven == 0xEC10) printf("note: configuration words read byte-swapped\n");
    dump_cfg("device", EP_CFG, 16);
    printf("device: vendor %04x device %04x revision %02x class %06x (%s)\n", ven, dev, w2 & 0xFF, w2 >> 8,
           class_name(w2 >> 8));
    printf("        subsystem %04x:%04x -> %s\n", w11 & 0xFFFF, w11 >> 16, chip_name(ven, dev));
    for (int i = 0; i < 6; i++) {
        uint32_t bar = REG32(EP_CFG + 0x10 + 4 * i);
        if (bar) printf("        BAR%d %08x (%s)\n", i, bar, (bar & 1) ? "I/O" : "memory");
    }
    uint32_t cap = REG32(EP_CFG + 0x34) & 0xFC;
    printf("        capabilities:");
    for (int n = 0; cap && n < 16; n++) {
        uint32_t w = REG32(EP_CFG + cap);
        printf(" %02x@%02x", w & 0xFF, cap);
        cap = (w >> 8) & 0xFC;
    }
    printf("  (01 power management, 05 MSI, 10 PCI Express)\n");

    if (ven == 0x10EC) identify_wifi();
    power_down();
    printf("PCIe port powered down again. done\n");
    return 0;
}
