/*
 * 14_flash_backup - stream the whole SPI flash to the PC over TCP.
 *
 * Connect to port 9697; the board sends an 16-byte header
 * ("RTLF" + jedec id + size + reserved, big-endian) followed by the raw
 * flash contents read through the memory-mapped window.  Nothing is
 * written.  PC side: python tools/flashdump.py
 */
#include "rtl8196c.h"
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "net.h"
#include "tcp.h"
#include "spiflash.h"
#include "cli.h"
#include "libc.h"

#define DUMP_PORT 9697
#define CHUNK     1024

static uint32_t offsets[TCP_MAX_CONN];         /* next byte to send per connection, ~0 = idle */
static uint32_t dumps_started, dumps_done;

static void dump_event(tcp_conn_t *c, int ev)
{
    int i = tcp_index(c);
    if (ev == TCP_EV_ACCEPT) {
        uint8_t hdr[16] = { 'R', 'T', 'L', 'F' };
        wr32(hdr + 4, 0xC22017);                 /* MX25L6405 per the kernel log; PIO RDID disturbs the window, so not read here */
        wr32(hdr + 8, SPIFLASH_SIZE);
        tcp_send(c, hdr, sizeof hdr);
        offsets[i] = 0;
        dumps_started++;
        printf("dump started (conn %d)\n", i);
    } else if (ev == TCP_EV_CLOSED || ev == TCP_EV_PEER_FIN) {
        if (offsets[i] != ~0u && offsets[i] < SPIFLASH_SIZE) printf("dump aborted at %08x\n", offsets[i]);
        offsets[i] = ~0u;
        if (ev == TCP_EV_PEER_FIN) tcp_close(c);
    }
}

static void dump_data(tcp_conn_t *c, const uint8_t *d, uint32_t len) { (void)c; (void)d; (void)len; }

/* push more flash into every open dump connection while its send buffer has room */
static void dump_pump(void)
{
    extern tcp_conn_t *tcp_conn_by_index(int i);
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (offsets[i] == ~0u) continue;
        tcp_conn_t *c = tcp_conn_by_index(i);
        while (offsets[i] < SPIFLASH_SIZE && tcp_send_space(c) >= CHUNK) {
            int n = tcp_send(c, (const void *)(SPIFLASH_MMAP + offsets[i]), CHUNK);
            if (n <= 0) break;
            offsets[i] += (uint32_t)n;
        }
        if (offsets[i] >= SPIFLASH_SIZE) {
            printf("dump complete (conn %d)\n", i);
            dumps_done++;
            offsets[i] = ~0u;
            tcp_close(c);
        }
    }
}

int main(void)
{
    static const uint8_t mac[6] = BOARD_MAC, ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    char s_ip[16];

    uart_init();
    timer_delay_init();
    for (int i = 0; i < TCP_MAX_CONN; i++) offsets[i] = ~0u;
    net_init(mac, ip);
    tcp_listen(DUMP_PORT, dump_data, dump_event);
    net_arp_request(peer);
    format_ip(s_ip, ip);

    printf("\n\nflash backup server on %s:%u\n", s_ip, DUMP_PORT);
    printf("SFCR=%08x SFCSR=%08x (memory-mapped reads only, no PIO)\n", REG32(SF_CR), REG32(SF_CSR));
    printf("flash[0]=%08x flash[10000]=%08x flash[7ffff0]=%08x\n",
           REG32(SPIFLASH_MMAP), REG32(SPIFLASH_MMAP + 0x10000), REG32(SPIFLASH_MMAP + 0x7FFFF0));

    uint32_t last = micros(), last_arp = micros();
    for (;;) {
        net_poll();
        dump_pump();
        if (every_ms(&last_arp, 10000)) net_arp_request(peer);
        if (every_ms(&last, 10000))
            printf("alive: dumps started %u done %u, tcp syn %u retx %u\n", dumps_started, dumps_done,
                   tcp_stats.syn, tcp_stats.retransmits);
    }
}
