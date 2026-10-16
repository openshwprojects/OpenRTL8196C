/*
 * 27_wifi_sta - join a Wi-Fi network as a station (WPA2-PSK or open), get an
 * address with the SDK's DHCP client, resolve a name through the network's
 * DNS, and serve the SDK web page on the address we got.
 *
 * Credentials: examples/27_wifi_sta/wifi_config.h (git-ignored):
 *   #define DEFAULT_WIFI_SSID "..."
 *   #define DEFAULT_WIFI_PASS "..."      ("" for an open network)
 * Tables + firmware: python tools/extract_wifi_blobs.py backups/flash_<date>.bin
 * Build: python tools/build.py examples/27_wifi_sta
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "wl.h"
#include "wl_sta.h"
#include "net.h"
#include "dhcp.h"
#include "tcp.h"
#include "httpd.h"
#include "cli.h"
#include "libc.h"
#include "wifi_blobs.inc"
#include "wifi_config.h"

static struct wl_dev dev;
static int dns_sent, dns_answered;
static char dns_result[64] = "(no answer yet)";

static struct wl_tables tables(void)
{
    struct wl_tables t = { wifi_fw, WIFI_FW_LEN, wifi_mac, WIFI_MAC_N, wifi_phy_2t, WIFI_PHY_2T_N,
        wifi_phy_fix, WIFI_PHY_FIX_N, wifi_agc, WIFI_AGC_N, wifi_radio_a, WIFI_RADIO_A_N,
        wifi_radio_b, WIFI_RADIO_B_N };
    return t;
}

/* ask the network's DNS for example.com, to prove traffic goes beyond the AP */
static void on_dns(const uint8_t src_ip[4], uint16_t sport, uint16_t dport, const uint8_t *d, uint32_t len)
{
    (void)src_ip; (void)sport; (void)dport;
    if (len < 12) return;
    uint16_t an = rd16(d + 6);
    uint32_t o = 12;
    while (o < len && d[o]) o += 1u + d[o];
    o += 5;                                                  /* name end + qtype + qclass */
    for (uint16_t i = 0; i < an && o + 12 <= len; i++) {
        o += 2;                                              /* name (pointer) */
        uint16_t type = rd16(d + o), rdlen = rd16(d + o + 8);
        o += 10;
        if (type == 1 && rdlen == 4 && o + 4 <= len) {
            snprintf(dns_result, sizeof dns_result, "example.com = %u.%u.%u.%u", d[o], d[o + 1], d[o + 2], d[o + 3]);
            dns_answered = 1;
            return;
        }
        o += rdlen;
    }
}

static void send_dns_query(void)
{
    static const uint8_t q[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    net_udp_listen(5353, on_dns);
    if (net_udp_send(dhcp_info()->dns, 5353, 53, q, sizeof q) == 0) dns_sent++;
}

static void page(const struct http_request *req, struct http_response *resp)
{
    const struct wl_sta_info *s = wl_sta_info();
    const struct dhcp_lease *l = dhcp_info();
    struct sb *b = &resp->body;
    sb_put(b, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>OpenRTL8196C station</title><style>body{font:16px system-ui;margin:24px;max-width:40em}code{background:#eee;padding:1px 4px}</style></head><body>");
    sb_putf(b, "<h1>The router is a Wi-Fi client now</h1><p>Joined <b>%s</b> (%s), channel %u, %d dBm, %s.</p>",
            DEFAULT_WIFI_SSID, wl_sta_state_name(s->state), s->chan, s->rssi, s->secured ? "WPA2 AES keys in the chip" : "open");
    sb_putf(b, "<p>Address %u.%u.%u.%u from the network's DHCP, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u.</p>",
            l->ip[0], l->ip[1], l->ip[2], l->ip[3], l->gw[0], l->gw[1], l->gw[2], l->gw[3], l->dns[0], l->dns[1], l->dns[2], l->dns[3]);
    sb_putf(b, "<p>DNS test: %s</p><p>Frames: %u in, %u out, %u EAPOL in, %u out, %u TX failures. Uptime %u s, requests %u, path <code>%s</code>.</p>",
            dns_result, s->data_rx, s->data_tx, s->eapol_rx, s->eapol_tx, s->tx_fail, timer_ms() / 1000u, httpd_requests, req->path);
    sb_put(b, "</body></html>");
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi station: joining \"%s\" (%s)\n", DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS[0] ? "WPA2-PSK" : "open");
    if (!DEFAULT_WIFI_SSID[0]) { printf("fill in examples/27_wifi_sta/wifi_config.h first\n"); return 1; }
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    pcie_map_bar2();
    struct wl_tables t = tables();
    int rc = wl_hw_init(&dev, &t);
    if (rc < 0) { printf("wl_hw_init failed %d\n", rc); pcie_power_down(); return 1; }
    printf("radio up, our MAC %02x:%02x:%02x:%02x:%02x:%02x\n", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);

    uint32_t t0 = micros();
    wl_sta_trace = 1;                                        /* show the first received frames in this demo */
    wl_sta_start(&dev, DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    printf("PMK derived in %u ms, scanning\n", elapsed_us(t0) / 1000u);
    static const uint8_t zero_ip[4] = { 0, 0, 0, 0 };
    net_set_link(wl_sta_link());
    net_init(dev.mac, zero_ip);
    httpd_init(80);
    httpd_route("/", page);
    httpd_route("/*", page);

    enum wl_sta_state last_state = WL_STA_IDLE;
    int dhcp_started = 0, bound_printed = 0;
    uint32_t last = timer_ms();
    for (;;) {
        uart_esc_reboot_poll();                              /* ESC from flash.py reboots into the loader */
        net_poll();                                          /* polls the radio through the link */
        if (dhcp_started) dhcp_poll();
        const struct wl_sta_info *s = wl_sta_info();
        if (s->state != last_state) {
            printf("[%u ms] %s", timer_ms(), wl_sta_state_name(s->state));
            if (s->state == WL_STA_AUTHENTICATING)
                printf(": found %02x:%02x:%02x:%02x:%02x:%02x on channel %u, %d dBm", s->bssid[0], s->bssid[1],
                       s->bssid[2], s->bssid[3], s->bssid[4], s->bssid[5], s->chan, s->rssi);
            if (s->state == WL_STA_AUTHENTICATING)
                printf(" (RSN: group cipher %u, pairwise CCMP %u TKIP %u)", s->ap_group_cipher, s->ap_pair_ccmp, s->ap_pair_tkip);
            if (s->state == WL_STA_CONNECTED) printf(": associated (AID %u)%s, GTK %u bytes id %u", s->aid,
                                                     s->secured ? ", WPA2 keys installed" : "", s->gtk_len, s->gtk_id);
            if (s->state == WL_STA_FAILED) printf(": %s", s->fail_reason);
            printf("\n");
            if (s->state == WL_STA_CONNECTED && !dhcp_started) {
                /* unicast-reception test: the ARP reply from the gateway comes back encrypted and unicast */
                static const uint8_t gw_guess[4] = { 192, 168, 0, 1 };
                net_arp_request(gw_guess);
                dhcp_start("OpenRTL8196C"); dhcp_started = 1;
            }
            if (s->state != WL_STA_CONNECTED) { dhcp_started = 0; bound_printed = 0; }
            last_state = s->state;
        }
        if (dhcp_started && dhcp_bound() && !bound_printed) {
            const struct dhcp_lease *l = dhcp_info();
            net_set_ip(l->ip); net_set_netmask(l->mask); net_set_gateway(l->gw); net_set_dns(l->dns);
            printf("DHCP: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u -> http://%u.%u.%u.%u/\n", l->ip[0], l->ip[1],
                   l->ip[2], l->ip[3], l->gw[0], l->gw[1], l->gw[2], l->gw[3], l->dns[0], l->dns[1], l->dns[2], l->dns[3],
                   l->ip[0], l->ip[1], l->ip[2], l->ip[3]);
            bound_printed = 1;
            send_dns_query();
        }
        if (dns_sent && !dns_answered && timer_ms() - last > 3000u && dns_sent < 5) send_dns_query();
        if (timer_ms() - last >= 5000u) {
            printf("alive: %s, data in %u (uni %u bc %u undec %u) out %u drop %u, eapol %u/%u, tx fail %u, "
                   "dhcp state %u, net rx %u tx %u udp rx %u arp replies %u, dns %s, http %u\n",
                   wl_sta_state_name(s->state), s->data_rx, s->rx_unicast, s->rx_bcast, s->rx_undecrypted, s->data_tx,
                   s->rx_drop, s->eapol_rx, s->eapol_tx, s->tx_fail, dhcp_info()->state, net_stats.rx, net_stats.tx,
                   net_stats.udp_rx, net_stats.arp_reply_rcvd, dns_result, httpd_requests);
            if (dhcp_started && !dhcp_bound() && timer_ms() - last > 20000u) { net_arp_request((const uint8_t[]){ 192, 168, 0, 1 }); }
            last = timer_ms();
        }
    }
}
