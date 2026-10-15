#include "rtl8196c.h"
#include "console.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "dhcp.h"
#include "tcp.h"
#include "swcore.h"
#include "spiflash.h"
#include "flash_layout.h"
#include "libc.h"

uint32_t console_led_mask;
uint32_t console_commands;
static const char *s_console_tag;

void console_set_tag(const char *tag)
{
    s_console_tag = tag;
}

int console_exec(char *line, struct sb *out)
{
    char *argv[8];
    int argc = split_args(line, argv, 8);
    console_commands++;
    if (argc == 0) { sb_put(out, "empty\n"); return CONSOLE_OK; }
    const char *c = argv[0];

    if (strcmp(c, "help") == 0) {
        sb_put(out, "commands: hi uptime ip link stats flash confirm peek <addr> [n] poke <addr> <val> led <mask> reboot\n");
    } else if (strcmp(c, "tag") == 0 && s_console_tag) {
        sb_putf(out, "tag %s\n", s_console_tag);
    } else if (strcmp(c, "hi") == 0) {
        char mac[18];
        format_mac(mac, eth_mac());
        sb_putf(out, "OpenRTL8196C console, mac %s, chip rev %08x, commands so far %u\n",
                mac, REG32(0xB8000000), console_commands);
    } else if (strcmp(c, "uptime") == 0) {
        uint32_t us = micros();
        sb_putf(out, "uptime %u.%06u s (28-bit microsecond counter)\n", us / 1000000u, us % 1000000u);
    } else if (strcmp(c, "ip") == 0) {
        char sip[16], smask[16], sgw[16], sdns[16];
        format_ip(sip, net_ip());
        format_ip(smask, net_netmask());
        format_ip(sgw, net_gateway());
        format_ip(sdns, net_dns());
        const struct dhcp_lease *dl = dhcp_info();
        sb_putf(out, "ip %s mask %s gw %s dns %s dhcp_state %u lease %u s\n",
                sip, smask, sgw, sdns, dl->state, dl->lease_seconds);
    } else if (strcmp(c, "flash") == 0) {
        uint8_t fmac[6];
        int has_fmac = (spiflash_factory_mac(fmac) == 0);
        char smac[18];
        format_mac(smac, fmac);
        struct part_table pt;
        int pt_def = part_table_read(&pt, FLASH_PARTTABLE_OFF);
        struct app_header ha, hb;
        int va = app_slot_valid_flash(FLASH_SLOTA_OFF, FLASH_SLOTA_SIZE, &ha);
        int vb = app_slot_valid_flash(FLASH_SLOTB_OFF, FLASH_SLOTB_SIZE, &hb);
        struct boot_summary sum;
        bootstate_scan(FLASH_BOOTSTATE_OFF, &sum);
        sb_putf(out, "factory_mac %s (%s) ptable %s (%u entries)\n",
                smac, has_fmac ? "H601" : "fallback", pt_def ? "default" : "flash", pt.count);
        sb_putf(out, "slotA: %s seq=%u len=%u ver=%s\n",
                va ? "valid" : "empty", va ? ha.seq : 0u, va ? ha.length : 0u, va ? ha.version : "-");
        sb_putf(out, "slotB: %s seq=%u len=%u ver=%s\n",
                vb ? "valid" : "empty", vb ? hb.seq : 0u, vb ? hb.length : 0u, vb ? hb.version : "-");
        sb_putf(out, "bootstate: last_slot=%d seq=%u tries=%u confirmed=%d records=%u\n",
                sum.last_slot, sum.last_seq, sum.unconfirmed_tries, sum.confirmed, sum.records_used);
    } else if (strcmp(c, "confirm") == 0) {
        int rc = bootstate_confirm(FLASH_BOOTSTATE_OFF);
        sb_putf(out, "confirm: %s\n", rc == 0 ? "ok" : "error");
    } else if (strcmp(c, "link") == 0) {
        static const char *const spd[] = { "10", "100", "1000", "?" };
        for (int p = 0; p < NUM_PHY_PORTS; p++) {
            uint32_t s = eth_port_status(p);
            sb_putf(out, "port %d: %s", p, (s & PS_LINK) ? "up" : "down");
            if (s & PS_LINK) sb_putf(out, " %sM %s", spd[s & PS_SPEED_MASK], (s & PS_FULL_DUPLEX) ? "FD" : "HD");
            sb_putf(out, "  rx frames %u\n", eth_rx_port_count[p]);
        }
    } else if (strcmp(c, "stats") == 0) {
        sb_putf(out, "rx %u tx %u arp_req %u arp_reply_sent %u arp_reply_rcvd %u icmp %u udp_rx %u udp_tx %u nolistener %u dropped %u\n",
                net_stats.rx, net_stats.tx, net_stats.arp_req, net_stats.arp_reply_sent, net_stats.arp_reply_rcvd,
                net_stats.icmp_echo, net_stats.udp_rx, net_stats.udp_tx, net_stats.udp_nolistener, net_stats.dropped);
        sb_putf(out, "tcp: syn %u established %u accepted %u closed %u retransmits %u resets %u timeouts %u dropped %u\n",
                tcp_stats.syn, tcp_stats.established, tcp_stats.accepted, tcp_stats.closed,
                tcp_stats.retransmits, tcp_stats.resets, tcp_stats.timeouts, tcp_stats.dropped);
    } else if (strcmp(c, "peek") == 0 && argc >= 2) {
        uint32_t addr, n = 4;
        if (!parse_hex(argv[1], &addr)) { sb_put(out, "bad address\n"); return CONSOLE_OK; }
        if (argc >= 3) parse_hex(argv[2], &n);
        if (n > 64) n = 64;
        addr &= ~3u;
        for (uint32_t i = 0; i < n; i++) {
            if (i % 4 == 0) sb_putf(out, "%08x:", addr + 4 * i);
            sb_putf(out, " %08x", REG32(addr + 4 * i));
            if (i % 4 == 3 || i == n - 1) sb_put(out, "\n");
        }
    } else if (strcmp(c, "poke") == 0 && argc >= 3) {
        uint32_t addr, val;
        if (!parse_hex(argv[1], &addr) || !parse_hex(argv[2], &val)) { sb_put(out, "bad args\n"); return CONSOLE_OK; }
        REG32(addr & ~3u) = val;
        sb_putf(out, "%08x <- %08x, reads back %08x\n", addr & ~3u, val, REG32(addr & ~3u));
    } else if (strcmp(c, "led") == 0 && argc >= 2) {
        uint32_t mask = 0;
        parse_hex(argv[1], &mask);
        console_led_mask = mask & 0x7F;
        gpio_leds_write(console_led_mask);
        sb_putf(out, "leds = %02x\n", console_led_mask);
    } else if (strcmp(c, "reboot") == 0) {
        sb_put(out, "rebooting\n");
        return CONSOLE_REBOOT;
    } else {
        sb_putf(out, "unknown command '%s' (try help)\n", c);
    }
    return CONSOLE_OK;
}

/* ---- Shared TCP/UDP console & echo listeners ---- */
static struct { char line[200]; uint16_t len; } s_tcp_lines[TCP_MAX_CONN];
static char s_reply_buf[2048];

static void tcon_prompt(tcp_conn_t *c)
{
    tcp_send(c, "> ", 2);
}

static void tcon_data(tcp_conn_t *c, const uint8_t *data, uint32_t len)
{
    int i = tcp_index(c);
    for (uint32_t k = 0; k < len; k++) {
        uint8_t ch = data[k];
        if (ch == '\n' || ch == '\r') {
            if (s_tcp_lines[i].len == 0) continue;
            s_tcp_lines[i].line[s_tcp_lines[i].len] = 0;
            s_tcp_lines[i].len = 0;
            struct sb out;
            sb_init(&out, s_reply_buf, sizeof s_reply_buf);
            if (strcmp(s_tcp_lines[i].line, "quit") == 0 || strcmp(s_tcp_lines[i].line, "exit") == 0) {
                tcp_send(c, "bye\n", 4);
                tcp_close(c);
                return;
            }
            int rc = console_exec(s_tcp_lines[i].line, &out);
            tcp_send(c, out.buf, out.len);
            if (rc == CONSOLE_REBOOT) { net_poll(); delay_ms(100); wdt_reboot(); }
            tcon_prompt(c);
        } else if (ch == 8 || ch == 127) {
            if (s_tcp_lines[i].len) s_tcp_lines[i].len--;
        } else if (ch >= 32 && s_tcp_lines[i].len < sizeof s_tcp_lines[i].line - 1) {
            s_tcp_lines[i].line[s_tcp_lines[i].len++] = (char)ch;
        }
    }
}

static void tcon_event(tcp_conn_t *c, int ev)
{
    int i = tcp_index(c);
    if (ev == TCP_EV_ACCEPT) {
        s_tcp_lines[i].len = 0;
        tcp_send(c, "OpenRTL8196C tcp console (help, quit)\n", 38);
        tcon_prompt(c);
    } else if (ev == TCP_EV_PEER_FIN) {
        tcp_close(c);
    }
}

static void udp_console_cb(const uint8_t src_ip[4], uint16_t sport, uint16_t dport,
                           const uint8_t *data, uint32_t len)
{
    char line[200];
    struct sb out;
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, data, len);
    line[len] = 0;
    sb_init(&out, s_reply_buf, sizeof s_reply_buf);
    int rc = console_exec(line, &out);
    net_udp_send(src_ip, dport, sport, out.buf, out.len);
    if (rc == CONSOLE_REBOOT) { delay_ms(50); wdt_reboot(); }
}

static void echo_tcp_data(tcp_conn_t *c, const uint8_t *data, uint32_t len)
{
    tcp_send(c, data, len);
}

static void echo_tcp_event(tcp_conn_t *c, int ev)
{
    if (ev == TCP_EV_PEER_FIN)
        tcp_close(c);
}

static void echo_udp_cb(const uint8_t src_ip[4], uint16_t sport, uint16_t dport,
                        const uint8_t *data, uint32_t len)
{
    net_udp_send(src_ip, dport, sport, data, len);
}

int console_tcp_listen(uint16_t port) { return tcp_listen(port, tcon_data, tcon_event); }
int console_udp_listen(uint16_t port) { return net_udp_listen(port, udp_console_cb); }
int echo_tcp_listen(uint16_t port)    { return tcp_listen(port, echo_tcp_data, echo_tcp_event); }
int echo_udp_listen(uint16_t port)    { return net_udp_listen(port, echo_udp_cb); }

