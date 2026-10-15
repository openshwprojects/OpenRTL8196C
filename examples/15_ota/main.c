/*
 * 15_ota - Ethernet OTA (RAM jump + Flash Slot A/B + Safe Test Flash), DHCP,
 * HTTP status server, and TCP/UDP consoles.
 *
 *   TCP 9698  OTA server (python tools/ota.py --status | --ram | --test-flash | --flash)
 *   TCP 80    HTTP status server
 *   TCP 23    line console
 *   UDP 9696  datagram console
 *   TCP/UDP 7 echo
 */
#include "rtl8196c.h"
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "spiflash.h"
#include "flash_layout.h"
#include "net.h"
#include "dhcp.h"
#include "tcp.h"
#include "httpd.h"
#include "statuspage.h"
#include "console.h"
#include "ota.h"
#include "cli.h"
#include "libc.h"

#ifndef OTA_BUILD_TAG
#define OTA_BUILD_TAG "15_ota_v1"
#endif

int main(void)
{
    static const uint8_t def_ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    uint8_t mac[6];
    char s_mac[18], s_ip[16];

    uart_init();
    timer_delay_init();
    gpio_leds_init();

    int from_h601 = (spiflash_factory_mac(mac) == 0);
    net_init(mac, def_ip);

    console_set_tag(OTA_BUILD_TAG);
    httpd_init(BOARD_HTTP_PORT);
    statuspage_register();
    ota_init(BOARD_OTA_PORT);
    console_tcp_listen(BOARD_TCP_CONSOLE_PORT);
    echo_tcp_listen(7);
    console_udp_listen(BOARD_UDP_CONSOLE_PORT);
    echo_udp_listen(7);

    dhcp_start("OpenRTL8196C");
    net_arp_request(peer);


    format_mac(s_mac, mac);
    format_ip(s_ip, def_ip);
    printf("\n\n15_ota (%s): mac=%s (%s) static=%s ota=%u\n",
           OTA_BUILD_TAG, s_mac, from_h601 ? "H601" : "default", s_ip, BOARD_OTA_PORT);

    int dhcp_printed = 0;
    uint32_t last = micros(), last_arp = micros();
    for (;;) {
        net_poll();
        dhcp_poll();
        ota_poll();

        if (!dhcp_printed && dhcp_bound()) {
            const struct dhcp_lease *dl = dhcp_info();
            char lip[16], lgw[16];
            format_ip(lip, dl->ip);
            format_ip(lgw, dl->gw);
            printf("[dhcp] bound ip=%s gw=%s lease=%u s (alias %s stays active)\n",
                   lip, lgw, dl->lease_seconds, s_ip);
            dhcp_printed = 1;
        }
        if (every_ms(&last_arp, 10000))
            net_arp_request(peer);
        if (every_ms(&last, 10000)) {
            char cur_ip[16];
            format_ip(cur_ip, net_ip());
            printf("alive (%s): ip=%s rx=%u ota_sessions=%u ota_bytes=%u\n",
                   OTA_BUILD_TAG, cur_ip, net_stats.rx, ota_sessions, ota_bytes_rcvd);
        }
    }
}
