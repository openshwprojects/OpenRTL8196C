/*
 * 29_wifi_ap_http - open Wi-Fi access point with a web server on it.
 *
 * The router beacons "OpenRTL8196C" (channel 7, no password).  A phone that
 * joins gets 192.168.4.x from the built-in DHCP server, every DNS name
 * resolves to the router, and http://192.168.4.1/ (or the phone's own
 * "sign in to network" page) is served by the SDK's httpd.c over Wi-Fi.
 * No OBK here: this is the SDK's own stack (net.c, tcp.c, httpd.c) with
 * the Wi-Fi driver plugged in as its link layer (sdk/src/wl_ap.c).
 *
 * Tables + firmware: python tools/extract_wifi_blobs.py backups/flash_<date>.bin
 * Build: python tools/build.py examples/29_wifi_ap_http     (runs uncached)
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "wl.h"
#include "wl_ap.h"
#include "net.h"
#include "tcp.h"
#include "httpd.h"
#include "dhcpd.h"
#include "dnsd.h"
#include "cli.h"
#include "libc.h"
#include "wifi_blobs.inc"

#define SSID "OpenRTL8196C"
#define CHAN 7

static struct wl_dev dev;
static const uint8_t ap_ip[4] = { 192, 168, 4, 1 }, ap_mask[4] = { 255, 255, 255, 0 };

static struct wl_tables tables(void)
{
    struct wl_tables t = { wifi_fw, WIFI_FW_LEN, wifi_mac, WIFI_MAC_N, wifi_phy_2t, WIFI_PHY_2T_N,
        wifi_phy_fix, WIFI_PHY_FIX_N, wifi_agc, WIFI_AGC_N, wifi_radio_a, WIFI_RADIO_A_N,
        wifi_radio_b, WIFI_RADIO_B_N };
    return t;
}

static void page(const struct http_request *req, struct http_response *resp)
{
    struct sb *b = &resp->body;
    const struct wl_ap_client *c; int nc = wl_ap_clients(&c);
    const struct dhcpd_lease *le; int nl = dhcpd_leases(&le);
    sb_put(b, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>OpenRTL8196C</title><style>body{font:16px system-ui;margin:24px;max-width:40em}"
              "h1{font-size:1.4em}td,th{padding:4px 10px;text-align:left}code{background:#eee;padding:1px 4px}</style></head><body>");
    sb_put(b, "<h1>Hello from the router, over Wi-Fi</h1>");
    sb_putf(b, "<p>You are connected to <b>%s</b>, an access point run by a from-scratch driver on the RTL8196C's "
               "RTL8192CE radio. This page comes from the SDK's own HTTP server at <code>192.168.4.1</code>.</p>", SSID);
    sb_putf(b, "<p>Uptime %u s &middot; requests %u &middot; you asked for <code>%s</code></p>",
            timer_ms() / 1000u, httpd_requests, req->path);
    sb_put(b, "<h2>Wi-Fi</h2><table><tr><th>Station</th><th>State</th><th>Signal</th></tr>");
    for (int i = 0; i < nc; i++)
        sb_putf(b, "<tr><td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%s</td><td>%d dBm</td></tr>", c[i].mac[0], c[i].mac[1],
                c[i].mac[2], c[i].mac[3], c[i].mac[4], c[i].mac[5], c[i].state == 2 ? "associated" : "authenticated",
                c[i].rssi);
    sb_putf(b, "</table><p>%u beacons, %u probe responses, %u auth, %u assoc, %u data frames in, %u out, %u TX failures</p>",
            wl_ap_stats.beacons, wl_ap_stats.probe_resp, wl_ap_stats.auth, wl_ap_stats.assoc, wl_ap_stats.data_rx,
            wl_ap_stats.data_tx, wl_ap_stats.tx_fail);
    sb_put(b, "<h2>DHCP leases</h2><table>");
    for (int i = 0; i < nl; i++)
        if (le[i].valid)
            sb_putf(b, "<tr><td>%u.%u.%u.%u</td><td>%02x:%02x:%02x:%02x:%02x:%02x</td><td>%s</td></tr>", le[i].ip[0],
                    le[i].ip[1], le[i].ip[2], le[i].ip[3], le[i].mac[0], le[i].mac[1], le[i].mac[2], le[i].mac[3],
                    le[i].mac[4], le[i].mac[5], le[i].hostname);
    sb_putf(b, "</table><p>DNS: %u queries, last <code>%s</code></p>", dnsd_stats.queries, dnsd_stats.last_name);
    sb_put(b, "<p><a href='/'>refresh</a></p></body></html>");
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi AP + HTTP: SSID \"%s\" channel %d, http://192.168.4.1/\n", SSID, CHAN);
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    pcie_map_bar2();
    struct wl_tables t = tables();
    int rc = wl_hw_init(&dev, &t);
    if (rc < 0) { printf("wl_hw_init failed %d\n", rc); pcie_power_down(); return 1; }
    printf("radio up, BSSID %02x:%02x:%02x:%02x:%02x:%02x\n", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3],
           dev.mac[4], dev.mac[5]);

    wl_ap_start(&dev, SSID, CHAN, 0);               /* software beacons, no-link mode: the joinable recipe */
    net_set_link(wl_ap_link());
    net_init(dev.mac, ap_ip);
    net_set_netmask(ap_mask);
    net_set_gateway(ap_ip);
    dhcpd_start(ap_ip, ap_mask, 2, 8);
    dnsd_start(ap_ip);
    httpd_init(80);
    httpd_route("/", page);
    httpd_route("/*", page);                        /* captive-portal probes land here too */
    printf("AP running. Join \"%s\" (no password), then open http://192.168.4.1/\n", SSID);

    uint32_t last = timer_ms(), bcn0 = 0;
#ifndef ABC_TEST
#define ABC_TEST 0                                  /* 1 = visibility test: cycle beacon modes, 40 s each */
#endif
    uint32_t phase_t0 = timer_ms(); int phase = -1;
    for (;;) {
        if (ABC_TEST) {
            int p = (int)((timer_ms() - phase_t0) / 40000u) % 3;
            if (p != phase) {
                phase = p;
                if (p == 0) { wl_ap_sw_beacons = 0; wl_hw_set_opmode(&dev, 3); wl_wr8(&dev, 0x553, 3); }
                if (p == 1) { wl_ap_sw_beacons = 1; wl_hw_set_opmode(&dev, 3); }
                if (p == 2) { wl_ap_sw_beacons = 1; wl_hw_set_opmode(&dev, 0); }
                printf("=== PHASE %c at %u s: %s ===\n", 'A' + p, timer_ms() / 1000u,
                       p == 0 ? "AP mode, hardware beacons only" :
                       p == 1 ? "AP mode, hardware + software beacons" : "no-link mode, software beacons only");
            }
        }
        uart_esc_reboot_poll();                         /* ESC from flash.py reboots into the loader */
        net_poll();
        wl_ap_poll();
        if (timer_ms() - last >= 5000u) {
            const struct wl_ap_client *c; int nc = wl_ap_clients(&c);
            printf("alive: beacons %u (+%u, sw %u), probe %u (%u by name)/%u, auth %u, assoc %u, clients %d | data in %u out %u "
                   "drop %u qfull %u fail %u | dhcp D%u O%u R%u A%u N%u | dns %u | http %u | net rx %u tx %u\n",
                   wl_ap_stats.beacons, wl_ap_stats.beacons - bcn0, wl_ap_stats.sw_beacons, wl_ap_stats.probe_req,
                   wl_ap_stats.probe_directed, wl_ap_stats.probe_resp, wl_ap_stats.auth, wl_ap_stats.assoc, nc, wl_ap_stats.data_rx,
                   wl_ap_stats.data_tx, wl_ap_stats.rx_drop, wl_ap_stats.rx_queue_full,
                   wl_ap_stats.tx_fail, dhcpd_stats.discover, dhcpd_stats.offer, dhcpd_stats.request,
                   dhcpd_stats.ack, dhcpd_stats.nak, dnsd_stats.queries, httpd_requests, net_stats.rx, net_stats.tx);
            /* TSF rate check: should be 1000 counts per ms */
            static uint32_t tsf0, ms0;
            uint32_t tsf = wl_rd32(&dev, 0x560), ms = timer_ms();
            if (ms0) printf("  hw: TSF rate %u per ms (want 1000), TDECTRL %08x HISR %08x\n",
                            (tsf - tsf0) / (ms - ms0 ? ms - ms0 : 1), wl_rd32(&dev, 0x208), wl_rd32(&dev, 0x124));
            tsf0 = tsf; ms0 = ms;
            bcn0 = wl_ap_stats.beacons;
            last = timer_ms();
        }
    }
}
