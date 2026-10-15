/*
 * 08_eth_link - which switch port has a cable, and what arrives on it.
 *
 * Every second: link/speed/duplex of PHY ports 0..4 from the port status
 * registers, cross-checked with PHY register 1 (BMSR) over MDIO, plus the
 * number of frames the CPU port received per source port.  Every received
 * frame is summarised (port, length, MACs, ethertype) for the first 20.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "swcore.h"

static const uint8_t mac[6] = { 0x02, 0x19, 0x6c, 0x00, 0x00, 0x01 };

static void print_mac(const uint8_t *m)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\neth link monitor\n");
    printf("HW_STRAP=%08x SIRR=%08x FFCR=%08x MSCR=%08x CPUICR(loader)=%08x\n",
           REG32(HW_STRAP), REG32(SW_SIRR), REG32(SW_FFCR), REG32(SW_MSCR), REG32(CPU_ICR));
    for (int p = 0; p < NUM_PHY_PORTS; p++)
        printf("PCRP%d=%08x PSRP%d=%08x PHY%d id=%04x:%04x\n", p, REG32(SW_PCRP(p)),
               p, REG32(SW_PSRP(p)), p, eth_phy_read(p, 2), eth_phy_read(p, 3));

    eth_init(mac);
    printf("CPUICR=%08x rings installed, mac ", REG32(CPU_ICR));
    print_mac(mac);
    printf("\n");

    uint32_t last = micros();
    unsigned shown = 0;
    for (;;) {
        uint8_t *data; uint32_t len; int port;
        while (eth_recv(&data, &len, &port)) {
            if (shown < 20) {
                shown++;
                printf("rx port %d len %4u  dst ", port, len);
                print_mac(data);
                printf("  src ");
                print_mac(data + 6);
                printf("  type %02x%02x\n", data[12], data[13]);
            }
            eth_recv_done();
        }
        if (every_ms(&last, 1000)) {
            printf("link:");
            for (int p = 0; p < NUM_PHY_PORTS; p++) {
                uint32_t s = eth_port_status(p);
                uint16_t bmsr = eth_phy_read(p, 1);
                static const char *const spd[] = { "10", "100", "1000", "?" };
                printf(" p%d:%s", p, (s & PS_LINK) ? "UP" : "--");
                if (s & PS_LINK)
                    printf("/%s%s", spd[s & PS_SPEED_MASK], (s & PS_FULL_DUPLEX) ? "FD" : "HD");
                printf("(bmsr %04x)", bmsr);
            }
            printf("  rx=%u by port:", eth_rx_count);
            for (int p = 0; p < NUM_PHY_PORTS; p++)
                printf(" %u", eth_rx_port_count[p]);
            printf("\n");
        }
    }
}
