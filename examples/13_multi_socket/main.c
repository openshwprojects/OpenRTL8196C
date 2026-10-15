/*
 * 13_multi_socket - several services at once on the same stack:
 *   TCP 80   web pages (statuspage on httpd)
 *   TCP 23   line console (telnet-style; "python tools/netcon.py --tcp")
 *   TCP 7    echo
 *   UDP 9696 console, UDP 7 echo
 * Up to TCP_MAX_CONN (8) connections may be open simultaneously.
 */
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "tcp.h"
#include "httpd.h"
#include "statuspage.h"
#include "console.h"
#include "ota.h"
#include "libc.h"

int main(void)
{
    static const uint8_t mac[6] = BOARD_MAC, ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    char s_ip[16];

    uart_init();
    timer_delay_init();
    gpio_leds_init();
    net_init(mac, ip);

    httpd_init(BOARD_HTTP_PORT);
    statuspage_register();
    ota_init(BOARD_OTA_PORT);
    console_tcp_listen(BOARD_TCP_CONSOLE_PORT);
    echo_tcp_listen(7);
    console_udp_listen(BOARD_UDP_CONSOLE_PORT);
    echo_udp_listen(7);
    net_arp_request(peer);

    format_ip(s_ip, ip);
    printf("\n\nmulti socket: http://%s/  tcp console %s:%u  echo tcp/udp 7  udp console %u  ota %u\n",
           s_ip, s_ip, BOARD_TCP_CONSOLE_PORT, BOARD_UDP_CONSOLE_PORT, BOARD_OTA_PORT);

    uint32_t last = micros(), last_arp = micros();
    for (;;) {
        net_poll();
        ota_poll();
        if (every_ms(&last_arp, 10000)) { net_arp_request(peer); }
        if (every_ms(&last, 10000)) {
            printf("alive: rx %u http %u cmds %u udp %u tcp(syn %u accepted %u closed %u retx %u)\n",
                   net_stats.rx, httpd_requests, console_commands, net_stats.udp_rx,
                   tcp_stats.syn, tcp_stats.accepted, tcp_stats.closed, tcp_stats.retransmits);
        }
    }
}

