/*
 * 12_http_server - the status pages (sdk/src/statuspage.c) on the httpd.
 * Open http://<board ip>/ in a browser.
 */
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "httpd.h"
#include "statuspage.h"
#include "cli.h"

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
    net_arp_request(peer);
    format_ip(s_ip, ip);
    printf("\n\nhttp server on http://%s/\n", s_ip);

    uint32_t last = micros(), last_arp = micros();
    for (;;) {
        net_poll();
        if (every_ms(&last_arp, 10000)) { net_arp_request(peer); }
        if (every_ms(&last, 10000)) {
            printf("alive: rx %u requests %u tcp syn %u closed %u retx %u timeouts %u dropped %u\n",
                   net_stats.rx, httpd_requests, tcp_stats.syn, tcp_stats.closed, tcp_stats.retransmits,
                   tcp_stats.timeouts, tcp_stats.dropped);
        }
    }
}
