#include "rtl8196c.h"
#include "statuspage.h"
#include "httpd.h"
#include "console.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "swcore.h"
#include "libc.h"

static void page_status(const struct http_request *req, struct http_response *r)
{
    (void)req;
    static const char *const spd[] = { "10", "100", "1000", "?" };
    struct sb *b = &r->body;
    uint32_t us = micros();
    sb_put(b, "<!doctype html><html><head><meta charset=utf-8><title>OpenRTL8196C</title>"
              "<meta name=viewport content='width=device-width'>"
              "<style>body{font-family:system-ui,sans-serif;margin:2em;max-width:40em;background:#111;color:#eee}"
              "h1{color:#7cf}table{border-collapse:collapse}td,th{border:1px solid #444;padding:.3em .8em;text-align:left}"
              "a.b{display:inline-block;margin:.2em;padding:.4em .8em;background:#246;color:#fff;text-decoration:none;border-radius:4px}"
              "a.on{background:#2a2}code{color:#fc8}</style></head><body>");
    sb_put(b, "<h1>OpenRTL8196C</h1><p>Served from RAM by a bare-metal RTL8196C (Lexra RLX4181, 390&nbsp;MHz), no OS, no flash.</p>");
    sb_putf(b, "<p>Uptime <b>%u.%u s</b> &middot; HTTP requests <b>%u</b> &middot; chip revision <code>%08x</code></p>",
            us / 1000000u, (us / 100000u) % 10, httpd_requests, REG32(0xB8000000));
    sb_put(b, "<h2>Switch ports</h2><table><tr><th>port</th><th>link</th><th>frames in</th></tr>");
    for (int p = 0; p < NUM_PHY_PORTS; p++) {
        uint32_t s = eth_port_status(p);
        sb_putf(b, "<tr><td>%d</td><td>%s", p, (s & PS_LINK) ? "up " : "down");
        if (s & PS_LINK) sb_putf(b, "%sM %s", spd[s & PS_SPEED_MASK], (s & PS_FULL_DUPLEX) ? "FD" : "HD");
        sb_putf(b, "</td><td>%u</td></tr>", eth_rx_port_count[p]);
    }
    sb_put(b, "</table><h2>LEDs</h2><p>");
    for (int i = 0; i < 7; i++)
        sb_putf(b, "<a class='b%s' href='/led/%x'>LED %d</a>", (console_led_mask >> i) & 1 ? " on" : "",
                console_led_mask ^ (1u << i), i);
    sb_put(b, "<a class=b href='/led/7f'>all</a><a class=b href='/led/0'>none</a></p>");
    sb_putf(b, "<h2>Network</h2><p>rx %u &middot; tx %u &middot; arp replies %u &middot; icmp %u &middot; udp %u<br>"
               "tcp: syn %u, accepted %u, closed %u, retransmits %u, resets %u, timeouts %u</p>",
            net_stats.rx, net_stats.tx, net_stats.arp_reply_sent, net_stats.icmp_echo, net_stats.udp_rx,
            tcp_stats.syn, tcp_stats.accepted, tcp_stats.closed, tcp_stats.retransmits, tcp_stats.resets, tcp_stats.timeouts);
    sb_put(b, "<h2>Registers</h2><p><a class=b href='/regs?addr=b8003100&n=8'>timers</a>"
              "<a class=b href='/regs?addr=b8003000&n=6'>interrupt ctrl</a>"
              "<a class=b href='/regs?addr=bb804128&n=5'>port status</a>"
              "<a class=b href='/api/status'>JSON</a></p></body></html>");
}

static void page_led(const struct http_request *req, struct http_response *r)
{
    uint32_t m = 0;
    parse_hex(req->path + 5, &m);
    console_led_mask = m & 0x7F;
    gpio_leds_write(console_led_mask);
    httpd_redirect(r, "/");
}

static void page_regs(const struct http_request *req, struct http_response *r)
{
    char a[16], nb[8];
    uint32_t addr = 0xB8003100, n = 8;
    if (query_get(req->query, "addr", a, sizeof a)) parse_hex(a, &addr);
    if (query_get(req->query, "n", nb, sizeof nb)) parse_u32(nb, &n);
    if (n > 256) n = 256;
    addr &= ~3u;
    r->content_type = "text/plain";
    for (uint32_t i = 0; i < n; i++) {
        if (i % 4 == 0) sb_putf(&r->body, "%08x:", addr + 4 * i);
        sb_putf(&r->body, " %08x", REG32(addr + 4 * i));
        if (i % 4 == 3 || i == n - 1) sb_put(&r->body, "\n");
    }
}

static void page_api(const struct http_request *req, struct http_response *r)
{
    (void)req;
    struct sb *b = &r->body;
    r->content_type = "application/json";
    sb_putf(b, "{\"uptime_us\":%u,\"requests\":%u,\"leds\":%u,\"revision\":\"%08x\",\"ports\":[",
            micros(), httpd_requests, console_led_mask, REG32(0xB8000000));
    for (int p = 0; p < NUM_PHY_PORTS; p++) {
        uint32_t s = eth_port_status(p);
        sb_putf(b, "%s{\"port\":%d,\"link\":%s,\"rx\":%u}", p ? "," : "", p, (s & PS_LINK) ? "true" : "false",
                eth_rx_port_count[p]);
    }
    sb_putf(b, "],\"net\":{\"rx\":%u,\"tx\":%u,\"icmp\":%u,\"udp\":%u},\"tcp\":{\"syn\":%u,\"accepted\":%u,\"retransmits\":%u}}\n",
            net_stats.rx, net_stats.tx, net_stats.icmp_echo, net_stats.udp_rx,
            tcp_stats.syn, tcp_stats.accepted, tcp_stats.retransmits);
}

void statuspage_register(void)
{
    httpd_route("/", page_status);
    httpd_route("/index*", page_status);
    httpd_route("/led/*", page_led);
    httpd_route("/regs", page_regs);
    httpd_route("/api/status", page_api);
}
