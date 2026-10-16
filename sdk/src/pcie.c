/*
 * pcie.c - RTL8196C PCIe root port, see pcie.h
 *
 * Sequence disassembled from the stock LT-6408n kernel (PCIE_reset_procedure
 * at 0x80138f94, power-off right after it, BAR setup in rtl8192cd_init_one).
 */
#include "pcie.h"
#include "rtl8196c.h"
#include "timer.h"
#include "uart.h"

#define SYS_ID          0xB8000000u
#define CLK_MANAGE      0xB8000010u
#define CLK_PCIE0       (1u << 11)
#define CLK_PCIE0_PERST (1u << 12)       /* 1 = device out of reset */
#define SYS_PCIE_PHY0   0xB8000050u
#define RC_CFG          0xB8B00000u
#define RC_LTSSM        (RC_CFG + 0x728u)
#define PHY_MDIO        (RC_CFG + 0x1000u)
#define PHY_CTRL        (RC_CFG + 0x1008u)

static void mdio_write(uint32_t reg, uint32_t val)
{
    REG32(PHY_MDIO) = ((reg & 0x1Fu) << 8) | (val << 16) | 1u;
    delay_ms(1);
}

int pcie_init(int verbose)
{
    REG32(CLK_MANAGE) |= CLK_PCIE0;
    delay_ms(100);
    REG32(SYS_PCIE_PHY0) = 0x8;                                    /* PHY MDIO reset */
    REG32(SYS_PCIE_PHY0) = 0x9;
    REG32(SYS_PCIE_PHY0) = 0xB;                                    /* load done */
    REG32(PHY_CTRL) = 0x1;                                         /* PHY reset, LTSSM enable */
    REG32(PHY_CTRL) = 0x81;
    delay_ms(200);

    if (REG32(SYS_ID) == 0x80000001u) {
        static const uint16_t a[][2] = { { 0x0, 0x5027 }, { 0x2, 0x6D18 }, { 0x6, 0x8828 }, { 0x7, 0x30FF },
            { 0x8, 0x18D7 }, { 0xA, 0x00E9 }, { 0xB, 0x0511 }, { 0xD, 0x15B6 }, { 0xF, 0x0F0F }, { 0x5, 0x0BCB },
            { 0x6, 0x8128 } };
        for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) mdio_write(a[i][0], a[i][1]);
    } else {                                                       /* "96C - 40MHz Clock Source" */
        static const uint16_t b[][2] = { { 0x0, 0xD087 }, { 0x1, 0x0003 }, { 0x2, 0x4D18 }, { 0x5, 0x0BCB },
            { 0x6, 0xF148 }, { 0x7, 0x31FF }, { 0x8, 0x18D7 }, { 0x9, 0x539C }, { 0xA, 0x20EB }, { 0xB, 0x0511 },
            { 0xD, 0x1764 }, { 0xF, 0x0A00 }, { 0x19, 0xFCE0 }, { 0x1E, 0xC280 } };
        for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) mdio_write(b[i][0], b[i][1]);
    }

    REG32(CLK_MANAGE) &= ~CLK_PCIE0_PERST;                         /* device reset */
    delay_ms(300);
    REG32(PHY_CTRL) = 0x1;
    REG32(PHY_CTRL) = 0x81;
    delay_ms(300);
    REG32(CLK_MANAGE) |= CLK_PCIE0_PERST;
    delay_ms(100);

    uint32_t t0 = micros(), last = 0xFFFFFFFFu, st = 0;
    for (int i = 0; i < 100; i++) {
        st = REG32(RC_LTSSM) & 0x1Fu;
        if (verbose && st != last) printf("pcie: %5u ms LTSSM %02x\n", elapsed_us(t0) / 1000u, st);
        last = st;
        if (st == 0x11) break;                                     /* L0 */
        delay_ms(100);
    }
    if (st != 0x11) {
        REG32(CLK_MANAGE) &= ~CLK_PCIE0;
        return -1;
    }
    REG32(RC_CFG + 4) = 0x00100007u;                               /* root port: I/O, memory, bus master */
    return 0;
}

uint32_t pcie_map_bar2(void)
{
    pcie_cfg_write(0x18, PCIE_MEM & 0x1FFFFFFFu);                  /* BAR2 (64-bit memory BAR) */
    pcie_cfg_write(0x1C, 0);
    pcie_cfg_write(0x04, (pcie_cfg_read(0x04) & 0xFFFF0000u) | 7u);
    delay_ms(10);
    return PCIE_MEM;
}

void pcie_power_down(void)
{
    pcie_cfg_write(0x44, (pcie_cfg_read(0x44) & ~3u) | 3u);       /* D3hot */
    mdio_write(0x0F, 0x0708);
    REG32(CLK_MANAGE) &= ~CLK_PCIE0;
}
