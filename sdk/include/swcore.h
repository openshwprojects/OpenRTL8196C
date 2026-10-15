/*
 * swcore.h - RTL8196C switch core and CPU-port NIC
 *
 * Register layout from Realtek's boot code for this exact chip
 * (rtl8196c-bootcode-1.0a: include/rtl8196x/asicregs.h, swNic_poll.c).
 * The boot loader has already reset the switch, configured the 5 PHY ports,
 * created VLAN 8 with all ports untagged members and enabled "unknown
 * unicast/multicast to CPU", so eth_init() only installs our own descriptor
 * rings on the CPU port and leaves the rest alone.
 */
#ifndef SWCORE_H
#define SWCORE_H

#include <stdint.h>

#define SWCORE_BASE         0xBB800000u
#define SW_MACCR_BASE       (SWCORE_BASE + 0x4000)
#define SW_MACCR            (SW_MACCR_BASE + 0x00)
#define SW_MDCIOCR          (SW_MACCR_BASE + 0x04)   /* MDC/MDIO command */
#define SW_MDCIOSR          (SW_MACCR_BASE + 0x08)   /* MDC/MDIO status */
#define   MDC_WRITE         (1u << 31)
#define   MDC_PHY_SHIFT     24
#define   MDC_REG_SHIFT     16
#define   MDC_BUSY          (1u << 31)
#define SW_PCRAM_BASE       (SWCORE_BASE + 0x4100)
#define SW_PCRP(n)          (SW_PCRAM_BASE + 0x04 + 4 * (n))   /* port config 0..5 */
#define   PCRP_ENABLE_PHYIF (1u << 0)                /* cleared by the loader's J command! */
#define   PCRP_MAXLEN_16K   (3u << 1)
#define   PCRP_MAC_SW_RESET (1u << 3)                /* 1 = normal, 0 = reset */
#define   PCRP_PHYID_SHIFT  26
#define SW_PSRP(n)          (SW_PCRAM_BASE + 0x28 + 4 * (n))   /* port status 0..5 */
#define   PS_NWAY           (1u << 7)
#define   PS_RXPAUSE        (1u << 6)
#define   PS_TXPAUSE        (1u << 5)
#define   PS_LINK           (1u << 4)
#define   PS_FULL_DUPLEX    (1u << 3)
#define   PS_SPEED_MASK     3u                       /* 0 = 10M, 1 = 100M, 2 = 1000M */
#define SW_SIRR             (SWCORE_BASE + 0x4204)   /* bit0 TRXRDY, bit2 FULL_RST */
#define SW_MSCR             (SWCORE_BASE + 0x4410)
#define SW_SWTCR0           (SWCORE_BASE + 0x4418)   /* bit18 STOP_TLU, bit19 TLU_READY */
#define SW_FFCR             (SWCORE_BASE + 0x4428)   /* bit1 unknown UC to CPU, bit0 unknown MC to CPU */
#define SW_QNUMCR           (SWCORE_BASE + 0x4754)   /* output queue number control */
#define SW_PVCR(n)          (SWCORE_BASE + 0x4A08 + 4 * (n))
#define SW_SWTACR           (SWCORE_BASE + 0x4D00)   /* table access control: bit0 busy, 3=force, 9=add */
#define SW_SWTAA            (SWCORE_BASE + 0x4D08)   /* table access address */
#define SW_TCR(n)           (SWCORE_BASE + 0x4D20 + 4 * (n)) /* table entry words 0..7 */
#define HW_STRAP            0xB8000008u

#define NUM_PHY_PORTS       5
#define ALL_PORT_MASK       0x1Fu

/* CPU interface (NIC) */
#define CPU_IFACE_BASE      0xB8010000u
#define CPU_ICR             (CPU_IFACE_BASE + 0x00)
#define   ICR_TXCMD         (1u << 31)
#define   ICR_RXCMD         (1u << 30)
#define   ICR_BURST_32W     0u
#define   ICR_MBUF_2048     (4u << 24)
#define   ICR_TXFD          (1u << 23)               /* kick TX descriptor fetch */
#define CPU_RPDCR(n)        (CPU_IFACE_BASE + 0x04 + 4 * (n))  /* RX pkthdr ring 0..5 */
#define CPU_RMDCR0          (CPU_IFACE_BASE + 0x1C)  /* RX mbuf ring */
#define CPU_TPDCR0          (CPU_IFACE_BASE + 0x20)  /* TX pkthdr ring 0 */
#define CPU_TPDCR1          (CPU_IFACE_BASE + 0x24)  /* TX pkthdr ring 1 */
#define CPU_IIMR            (CPU_IFACE_BASE + 0x28)
#define CPU_IISR            (CPU_IFACE_BASE + 0x2C)
#define   IISR_RX_DONE_ALL      (0x3Fu << 3)
#define   IISR_TX_DONE_ALL      (0x3u << 9)
#define   IISR_MBUF_RUNOUT      (1u << 16)
#define   IISR_PKTHDR_RUNOUT    (0x3Fu << 17)

/* descriptor word = pointer | flags */
#define DESC_OWNED_BIT      1u
#define DESC_RISC_OWNED     0u
#define DESC_SWCORE_OWNED   1u
#define DESC_WRAP           2u

/* mbuf and pkthdr exactly as the ASIC expects them (big-endian bit-fields) */
struct mbuf {
    struct mbuf   *m_next;
    struct pkthdr *m_pkthdr;
    uint16_t       m_len;
    uint16_t       m_flags;
#define MBUF_USED   0x80
#define MBUF_EXT    0x10
#define MBUF_PKTHDR 0x08
#define MBUF_EOR    0x04
    uint8_t       *m_data;
    uint8_t       *m_extbuf;
    uint16_t       m_extsize;
    uint8_t        m_reserved[2];
};

struct pkthdr {
    struct mbuf   *ph_mbuf;
    uint16_t       ph_len;
    uint16_t       ph_reserved1   : 1;
    uint16_t       ph_queueId     : 3;
    uint16_t       ph_extPortList : 4;
    uint16_t       ph_reserved2   : 3;
    uint16_t       ph_hwFwd       : 1;
    uint16_t       ph_isOriginal  : 1;
    uint16_t       ph_l2Trans     : 1;
    uint16_t       ph_srcExtPortNum : 2;
    uint16_t       ph_type        : 3;
    uint16_t       ph_vlanTagged  : 1;
    uint16_t       ph_LLCTagged   : 1;
    uint16_t       ph_pppeTagged  : 1;
    uint16_t       ph_pppoeIdx    : 3;
    uint16_t       ph_linkID      : 7;
    uint16_t       ph_reason;
    uint16_t       ph_flags;
#define PKTHDR_USED   0x8000
#define PKT_INCOMING  0x1000
#define PKT_OUTGOING  0x0800
    uint8_t        ph_orgtos;
    uint8_t        ph_portlist;     /* RX: source port number, TX: destination port mask */
    uint16_t       ph_vlanId_resv : 1;
    uint16_t       ph_txPriority  : 3;
    uint16_t       ph_vlanId      : 12;
    uint16_t       ph_flags2;
    uint8_t        ph_pad[12];      /* the vendor says "exactly 32 bytes" */
};

/* ---- API ---- */
#define ETH_RX_RING   8
#define ETH_TX_RING   4
#define ETH_CLUSTER   2048

void     eth_init(const uint8_t mac[6]);
const uint8_t *eth_mac(void);

/* polled receive: returns 1 and fills data/len/port when a frame is waiting.
 * Call eth_recv_done() when finished with the buffer. */
int      eth_recv(uint8_t **data, uint32_t *len, int *port);
void     eth_recv_done(void);

/* queue a frame for the ports in portmask (bit n = PHY port n); copies data.
 * returns 0 on success, -1 when the TX ring is full. */
int      eth_send(const void *frame, uint32_t len, uint8_t portmask);
void     eth_tx_reclaim(void);

/* port status */
uint32_t eth_port_status(int port);          /* raw PSRP */
int      eth_link_up(int port);
uint16_t eth_phy_read(int phy, int reg);     /* MDIO */
void     eth_phy_write(int phy, int reg, uint16_t val);

/* statistics */
extern uint32_t eth_rx_count, eth_tx_count, eth_rx_port_count[8];

#endif
