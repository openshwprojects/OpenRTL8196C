/*
 * swcore.c - polled driver for the RTL8196C CPU-port NIC
 *
 * Mirrors the boot loader's swNic_poll.c: one RX pkthdr ring, one RX mbuf
 * ring, one TX pkthdr ring, 2048-byte clusters.  Everything lives in .bss,
 * which is in the uncached KSEG1 image, so no cache maintenance is needed.
 * Pointers handed to the hardware are KSEG1 addresses, exactly like the
 * vendor code does.
 */
#include "rtl8196c.h"
#include "swcore.h"
#include "timer.h"
#include "libc.h"

static uint32_t rx_ring[ETH_RX_RING]  __attribute__((aligned(16)));
static uint32_t mb_ring[ETH_RX_RING]  __attribute__((aligned(16)));
static uint32_t tx_ring[ETH_TX_RING]  __attribute__((aligned(16)));
static struct pkthdr rx_ph[ETH_RX_RING] __attribute__((aligned(32)));
static struct pkthdr tx_ph[ETH_TX_RING] __attribute__((aligned(32)));
static struct mbuf   rx_mb[ETH_RX_RING] __attribute__((aligned(16)));
static struct mbuf   tx_mb[ETH_TX_RING] __attribute__((aligned(16)));
static uint8_t rx_buf[ETH_RX_RING][ETH_CLUSTER] __attribute__((aligned(16)));
static uint8_t tx_buf[ETH_TX_RING][ETH_CLUSTER] __attribute__((aligned(16)));

static unsigned rx_idx, mb_idx, tx_idx, tx_done_idx;
static uint8_t  our_mac[6];

uint32_t eth_rx_count, eth_tx_count, eth_rx_port_count[8];

static inline struct pkthdr *desc_ph(uint32_t d)
{
    return (struct pkthdr *)(d & ~(DESC_OWNED_BIT | DESC_WRAP));
}

static void swtable_write(uint32_t type, uint32_t idx, const uint32_t entry[8], uint32_t cmd)
{
    REG32(SW_SWTCR0) |= (1u << 18);                 /* STOP_TLU */
    while ((REG32(SW_SWTCR0) & (1u << 19)) == 0)    /* wait TLU_READY */
        ;
    while (REG32(SW_SWTACR) & 1u)
        ;
    for (int i = 0; i < 8; i++)
        REG32(SW_TCR(i)) = entry[i];
    REG32(SW_SWTAA) = 0xBB000000u + (((type << 11) + idx) << 5);
    REG32(SW_SWTACR) = cmd;                         /* 0x03 = FORCE, 0x09 = ADD */
    while (REG32(SW_SWTACR) & 1u)
        ;
    REG32(SW_SWTCR0) &= ~(1u << 18);
}

static void phy_patch(int port, uint16_t page, uint16_t reg, uint16_t mask, uint16_t val)
{
    int p0 = (port == 999) ? 0 : port;
    int p1 = (port == 999) ? NUM_PHY_PORTS : (port + 1);
    for (int p = p0; p < p1; p++) {
        if (page >= 31) {
            eth_phy_write(p, 31, 7);
            eth_phy_write(p, 30, page);
        } else {
            eth_phy_write(p, 31, page);
        }
        uint16_t v = mask ? (eth_phy_read(p, reg) & mask) : 0;
        eth_phy_write(p, reg, v | val);
        eth_phy_write(p, 31, 0);
    }
}

static void phy_set_p1(int port)
{
    static const uint16_t r19[5] = { 0x5580, 0x5480, 0x54C0, 0x5440, 0x5400 };
    eth_phy_write(4, 31, 1);
    uint16_t v = eth_phy_read(4, 20);
    eth_phy_write(4, 20, (uint16_t)(v + (1u << port)));
    eth_phy_write(port, 31, 1);
    eth_phy_write(port, 19, r19[port]);
    eth_phy_write(4, 20, 0x0B20);
    eth_phy_write(port, 31, 0);
    eth_phy_write(4, 31, 0);
}

static void swcore_set_vlan_netif(const uint8_t mac[6])
{
    /* Table 4 (NETINTERFACE), index 0: valid=1, vid=8, mtu=1500, mac */
    uint32_t m01 = ((uint32_t)mac[0] << 8) | mac[1];
    uint32_t m23 = ((uint32_t)mac[2] << 8) | mac[3];
    uint32_t m45 = ((uint32_t)mac[4] << 8) | mac[5];
    uint32_t netif_entry[8] = { 0 };
    netif_entry[0] = 1u | (8u << 1) | ((((m23 << 16) | m45) & 0x0007FFFFu) << 13);
    netif_entry[1] = ((m01 << 13) | (m23 >> 3)) & 0x0FFFFFFFu;
    netif_entry[2] = 0x9C000000u;
    netif_entry[3] = 1500u >> 3;
    swtable_write(4, 0, netif_entry, 0x03u);

    /* Table 6 (VLAN), index 8: memberPort=0x3f, untagSet=0x3f, fid=0 */
    uint32_t vlan_entry[8] = { 0x00007E3Fu, 0, 0, 0, 0, 0, 0, 0 };
    swtable_write(6, 8, vlan_entry, 0x03u);
}

/*
 * Full switch ASIC and PHY bring-up (mirrors the Realtek v1.1f bootloader's
 * swCore_init + eth_startup).  When the root bootloader boots directly from
 * SPI flash without entering its interactive <RealTek> prompt, it skips
 * eth_startup() completely, so we must run this ourselves on cold boot.
 */
static void swcore_hw_init(const uint8_t mac[6])
{
    uint32_t saved_pinmux = REG32(PIN_MUX_SEL);

    /* Full switch core reset */
    REG32(SW_SIRR) |= 0x04u;
    delay_ms(50);
    REG32(SW_SIRR) |= 0x01u;

    /* Internal 5-port 10/100 PHY analog calibration (RTL8196C rev B = 0x80000002) */
    if (REG32(SYS_BASE) == 0x80000002u) {
        for (int p = 0; p < NUM_PHY_PORTS; p++)
            REG32(SW_PCRP(p)) |= 0x02000000u;
        phy_patch(999, 1, 17, 0xE3FF, 0x1C00);
        phy_patch(999, 4, 24, 0xFF00, 0x00F3);
        phy_patch(999, 4, 16, 0xFFF7, 0x0008);
        phy_patch(999, 1, 19, 0xC7FF, 0x1000);
        phy_patch(999, 1, 23, 0xFE3F, 0x0100);
        phy_patch(999, 1, 18, 0xFFC7, 0x0030);
        REG32(SW_MACCR) = (REG32(SW_MACCR) & ~0x0Fu) | 0x05u;
        phy_patch(999, 0, 21, 0xFF00, 0x0032);
        phy_patch(999, 0, 22, 0xFF8F, 0x0050);
        phy_patch(999, 0,  0, 0xFDFF, 0x0200);
        phy_patch(999, 1, 17, 0xFFF9, 0x0006);
        phy_patch(999, 1, 18, 0x0000, 0x9004);
        phy_patch(999, 0, 21, 0xBFFF, 0x4000);
        for (int p = 0; p < NUM_PHY_PORTS; p++)
            REG32(SW_PCRP(p)) &= ~0x02000000u;
        for (int p = 0; p < NUM_PHY_PORTS; p++)
            phy_set_p1(p);
    }

    /* Clear all ASIC lookup tables */
    static const struct { uint8_t type; uint16_t count; } tbl_sizes[] = {
        { 0, 1024 }, { 1, 512 }, { 2, 8 }, { 3, 64 }, { 4, 8 }, { 6, 4096 },
        { 5, 16 }, { 9, 1024 }, { 11, 8 }, { 12, 128 }, { 13, 32 }, { 14, 32 }
    };
    uint32_t zero_entry[8] = { 0 };
    for (unsigned t = 0; t < sizeof(tbl_sizes) / sizeof(tbl_sizes[0]); t++) {
        for (uint32_t i = 0; i < tbl_sizes[t].count; i++)
            swtable_write(tbl_sizes[t].type, i, zero_entry, 0x03u);
    }

    /* Port config, default PVID 8, L2 table lookup enable, 1 output queue/port */
    for (int p = 0; p < NUM_PHY_PORTS; p++) {
        REG32(SW_PCRP(p)) &= ~0x00400008u;
        REG32(SW_PCRP(p)) |= ((uint32_t)p << 26) | 0x0Fu;
    }
    for (int i = 0; i < 4; i++)
        REG32(SW_PVCR(i)) = 0x00080008u;
    REG32(SW_MSCR)   = 1u;
    REG32(SW_QNUMCR) = 0x1249u;
    REG32(SW_SIRR)  |= 1u;

    for (int p = 1; p < NUM_PHY_PORTS; p++) {
        uint16_t bmcr = eth_phy_read(p, 0);
        eth_phy_write(p, 0, bmcr | 0x0200u);
    }

    /* Do NOT add our_mac to L2 Table 0 with LAN portmask; leaving our_mac out
     * of Table 0 ensures SW_FFCR=3 traps all unicast frames for our_mac to CPU. */
    REG32(SW_FFCR) = 3u;
    swcore_set_vlan_netif(mac);
    REG32(SW_MDCIOCR) = 0x96181441u;
    REG32(PIN_MUX_SEL) = saved_pinmux;
}

void eth_init(const uint8_t mac[6])
{
    memcpy(our_mac, mac, 6);

    /* stop the CPU port while we swap the rings (the loader left its own) */
    REG32(CPU_ICR) = 0;
    REG32(CPU_IIMR) = 0;
    REG32(CPU_IISR) = 0xFFFFFFFFu;

    /* If cold-booting directly from SPI flash (without entering the <RealTek>
     * console), the switch ASIC was never initialized by the root loader. */
    if ((REG32(SW_FFCR) & 3u) != 3u)
        swcore_hw_init(mac);

    /* Clear ring base registers to 0 first so writing the same ring address
     * on a soft-reboot forces the NIC hardware to reset its internal index to 0 */
    REG32(CPU_TPDCR0) = 0;
    REG32(CPU_TPDCR1) = 0;
    for (int i = 0; i < 6; i++)
        REG32(CPU_RPDCR(i)) = 0;
    REG32(CPU_RMDCR0) = 0;

    for (int i = 0; i < ETH_TX_RING; i++) {
        memset(&tx_ph[i], 0, sizeof tx_ph[i]);
        memset(&tx_mb[i], 0, sizeof tx_mb[i]);
        tx_ph[i].ph_mbuf = &tx_mb[i];
        tx_ph[i].ph_flags = PKTHDR_USED | PKT_OUTGOING;
        tx_ph[i].ph_type = 0;                       /* ethernet */
        tx_mb[i].m_pkthdr = &tx_ph[i];
        tx_mb[i].m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
        tx_ring[i] = (uint32_t)&tx_ph[i] | DESC_RISC_OWNED;
    }
    tx_ring[ETH_TX_RING - 1] |= DESC_WRAP;

    for (int i = 0; i < ETH_RX_RING; i++) {
        memset(&rx_ph[i], 0, sizeof rx_ph[i]);
        memset(&rx_mb[i], 0, sizeof rx_mb[i]);
        rx_ph[i].ph_mbuf = &rx_mb[i];
        rx_ph[i].ph_flags = PKTHDR_USED | PKT_INCOMING;
        rx_mb[i].m_pkthdr = &rx_ph[i];
        rx_mb[i].m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
        rx_mb[i].m_extsize = ETH_CLUSTER;
        rx_mb[i].m_data = rx_mb[i].m_extbuf = &rx_buf[i][2];   /* IP header 4-byte aligned */
        rx_ring[i] = (uint32_t)&rx_ph[i] | DESC_SWCORE_OWNED;
        mb_ring[i] = (uint32_t)&rx_mb[i] | DESC_SWCORE_OWNED;
    }
    rx_ring[ETH_RX_RING - 1] |= DESC_WRAP;
    mb_ring[ETH_RX_RING - 1] |= DESC_WRAP;

    rx_idx = mb_idx = tx_idx = tx_done_idx = 0;

    REG32(CPU_TPDCR0) = (uint32_t)tx_ring;
    REG32(CPU_TPDCR1) = (uint32_t)tx_ring;          /* unused ring 1: point it somewhere valid */
    for (int i = 0; i < 6; i++)
        REG32(CPU_RPDCR(i)) = (uint32_t)rx_ring;    /* only ring 0 is used; others alias it */
    REG32(CPU_RMDCR0) = (uint32_t)mb_ring;

    REG32(CPU_ICR) = ICR_TXCMD | ICR_RXCMD | ICR_BURST_32W | ICR_MBUF_2048;
    REG32(SW_MDCIOCR) = 0x96181441u;

    /* The loader's "J" command (and ota_ram_jump) clears EnablePHYIf on every
     * port before jumping to prevent DMA into .bss during boot. Turn the PHY
     * interfaces back on now that the rings are ready. */
    for (int p = 0; p < NUM_PHY_PORTS; p++)
        REG32(SW_PCRP(p)) |= PCRP_ENABLE_PHYIF;
}

const uint8_t *eth_mac(void)
{
    return our_mac;
}

int eth_recv(uint8_t **data, uint32_t *len, int *port)
{
    uint32_t d = rx_ring[rx_idx];
    if ((d & DESC_OWNED_BIT) != DESC_RISC_OWNED)
        return 0;
    struct pkthdr *ph = desc_ph(d);
    *data = ph->ph_mbuf->m_data;
    *len  = ph->ph_len >= 4 ? ph->ph_len - 4 : 0;   /* strip CRC */
    *port = ph->ph_portlist;
    eth_rx_count++;
    eth_rx_port_count[ph->ph_portlist & 7]++;
    return 1;
}

void eth_recv_done(void)
{
    rx_ring[rx_idx] |= DESC_SWCORE_OWNED;
    mb_ring[mb_idx] |= DESC_SWCORE_OWNED;
    rx_idx = (rx_idx + 1) % ETH_RX_RING;
    mb_idx = (mb_idx + 1) % ETH_RX_RING;
    if (REG32(CPU_IISR) & (IISR_PKTHDR_RUNOUT | IISR_MBUF_RUNOUT))
        REG32(CPU_IISR) = IISR_PKTHDR_RUNOUT | IISR_MBUF_RUNOUT;
}

void eth_tx_reclaim(void)
{
    while (tx_done_idx != tx_idx &&
           (tx_ring[tx_done_idx] & DESC_OWNED_BIT) == DESC_RISC_OWNED)
        tx_done_idx = (tx_done_idx + 1) % ETH_TX_RING;
}

int eth_send(const void *frame, uint32_t len, uint8_t portmask)
{
    eth_tx_reclaim();
    unsigned next = (tx_idx + 1) % ETH_TX_RING;
    if (next == tx_done_idx) {
        uint32_t t0 = micros();
        while (next == tx_done_idx && (uint32_t)(micros() - t0) < 2000u)
            eth_tx_reclaim();
        if (next == tx_done_idx)
            return -1;                              /* ring full */
    }
    if ((tx_ring[tx_idx] & DESC_OWNED_BIT) != DESC_RISC_OWNED)
        return -1;
    if (len > ETH_CLUSTER - 4)
        len = ETH_CLUSTER - 4;

    struct pkthdr *ph = desc_ph(tx_ring[tx_idx]);
    uint8_t *buf = tx_buf[tx_idx];
    memcpy(buf, frame, len);
    uint32_t total = len < 60 ? 64 : len + 4;       /* vendor rule: CRC is added by hardware */
    if (len < 60)
        memset(buf + len, 0, 60 - len);

    ph->ph_len = (uint16_t)total;
    ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
    ph->ph_type = 0;
    ph->ph_portlist = portmask;
    ph->ph_vlanId = 0;
    ph->ph_txPriority = 0;
    ph->ph_mbuf->m_len = (uint16_t)total;
    ph->ph_mbuf->m_extsize = (uint16_t)total;
    ph->ph_mbuf->m_data = ph->ph_mbuf->m_extbuf = buf;

    tx_ring[tx_idx] |= DESC_SWCORE_OWNED;
    REG32(CPU_ICR) |= ICR_TXFD;
    tx_idx = next;
    eth_tx_count++;
    return 0;
}

uint32_t eth_port_status(int port)
{
    (void)REG32(SW_PSRP(port));                     /* vendor code reads twice */
    return REG32(SW_PSRP(port));
}

int eth_link_up(int port)
{
    return (eth_port_status(port) & PS_LINK) != 0;
}

uint16_t eth_phy_read(int phy, int reg)
{
    REG32(SW_MDCIOCR) = ((uint32_t)phy << MDC_PHY_SHIFT) | ((uint32_t)reg << MDC_REG_SHIFT);
    while (REG32(SW_MDCIOSR) & MDC_BUSY)
        ;
    return (uint16_t)REG32(SW_MDCIOSR);
}

void eth_phy_write(int phy, int reg, uint16_t val)
{
    REG32(SW_MDCIOCR) = MDC_WRITE | ((uint32_t)phy << MDC_PHY_SHIFT) |
                        ((uint32_t)reg << MDC_REG_SHIFT) | val;
    while (REG32(SW_MDCIOSR) & MDC_BUSY)
        ;
}
