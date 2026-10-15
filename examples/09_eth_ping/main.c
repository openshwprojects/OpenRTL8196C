/*
 * 09_eth_ping - the board answers ARP and ping.  Everything is in sdk/src/net.c;
 * this example just polls it and prints counters.
 */
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "net.h"
#include "swcore.h"
#include "cli.h"

int main(void)
{
    static const uint8_t mac[6] = BOARD_MAC, ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    char s_ip[16], s_mac[18];

    uart_init();
    timer_delay_init();
    net_init(mac, ip);
    format_ip(s_ip, ip);
    format_mac(s_mac, mac);
    printf("\n\nping responder: ip %s mac %s\n", s_ip, s_mac);

    uint32_t last = micros(), last_probe = 0;
    for (;;) {
        net_poll();
        if (every_ms(&last_probe, 5000)) {
            net_arp_request(peer);                 /* lets the PC learn our MAC without a broadcast from its side */
        }
        if (every_ms(&last, 2000)) {
            printf("rx=%u tx=%u arp_replies=%u arp_answers=%u pings=%u  by port:", net_stats.rx, net_stats.tx,
                   net_stats.arp_reply_sent, net_stats.arp_reply_rcvd, net_stats.icmp_echo);
            for (int p = 0; p < NUM_PHY_PORTS; p++)
                printf(" %u", eth_rx_port_count[p]);
            printf("\n");
        }
    }
}
