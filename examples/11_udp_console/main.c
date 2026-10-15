/*
 * 11_udp_console - the shared console (sdk/src/console.c) over UDP, plus UDP echo on port 7.
 * Use tools/netcon.py from the PC.
 */
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "console.h"
#include "libc.h"

int main(void)
{
    static const uint8_t mac[6] = BOARD_MAC, ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    char s_ip[16];

    uart_init();
    timer_delay_init();
    gpio_leds_init();
    net_init(mac, ip);
    console_udp_listen(BOARD_UDP_CONSOLE_PORT);
    echo_udp_listen(7);
    net_arp_request(peer);

    format_ip(s_ip, ip);
    printf("\n\nudp console on %s port %u (echo on 7)\n", s_ip, BOARD_UDP_CONSOLE_PORT);

    uint32_t last = micros(), last_arp = micros();
    for (;;) {
        net_poll();
        if (every_ms(&last_arp, 10000)) { net_arp_request(peer); }
        if (every_ms(&last, 5000)) {
            printf("alive: rx %u udp %u cmds %u\n", net_stats.rx, net_stats.udp_rx, console_commands);
        }
    }
}
