/*
 * 26_wifi_scan - Wi-Fi active + passive scan with the RTL8192CE.
 *
 * On each channel it sends a broadcast probe request (active scan) and then
 * listens, so it hears both beacons and the probe responses solicited from
 * nearby APs.  This is the first on-air transmit: it proves the TX path.
 * Only probe requests go out, addressed to the broadcast address.
 *
 * Tables + firmware: tools/extract_wifi_blobs.py (git-ignored wifi_blobs.inc).
 * Build: python tools/build.py examples/26_wifi_scan   (runs uncached)
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "wl.h"
#include "libc.h"
#include "wifi_blobs.inc"

static struct wl_dev dev;

struct bss { uint8_t bssid[6]; char ssid[33]; uint8_t ch, sec; int rssi; uint32_t beacons, probes; };
static struct bss list[64];
static uint32_t nbss;
static uint32_t sent, heard;

static struct wl_tables tables(void)
{
    struct wl_tables t = {
        wifi_fw, WIFI_FW_LEN,
        wifi_mac, WIFI_MAC_N, wifi_phy_2t, WIFI_PHY_2T_N, wifi_phy_fix, WIFI_PHY_FIX_N,
        wifi_agc, WIFI_AGC_N, wifi_radio_a, WIFI_RADIO_A_N, wifi_radio_b, WIFI_RADIO_B_N,
    };
    return t;
}

static void note(const uint8_t *f, uint32_t len, uint8_t ch, int rssi, int is_probe_resp)
{
    if (len < 36) return;
    const uint8_t *bssid = f + 16;
    uint16_t cap = (uint16_t)(f[34] | (f[35] << 8));
    char ssid[33] = ""; uint8_t chan = ch, rsn = 0, wpa = 0;
    for (uint32_t o = 36; o + 2 <= len;) {
        uint8_t id = f[o], l = f[o + 1]; const uint8_t *v = f + o + 2;
        if (o + 2 + l > len) break;
        if (id == 0 && l <= 32) { for (uint32_t k = 0; k < l; k++) ssid[k] = (v[k] >= 32 && v[k] < 127) ? (char)v[k] : '?'; ssid[l] = 0; }
        if (id == 3 && l >= 1) chan = v[0];
        if (id == 48) rsn = 1;
        if (id == 221 && l >= 4 && v[0] == 0 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) wpa = 1;
        o += 2u + l;
    }
    uint8_t sec = (cap & 0x10) ? (rsn && wpa ? 4 : rsn ? 3 : wpa ? 2 : 1) : 0;
    struct bss *b = 0;
    for (uint32_t i = 0; i < nbss; i++) if (!memcmp(list[i].bssid, bssid, 6)) b = &list[i];
    if (!b) { if (nbss >= 64) return; b = &list[nbss++]; memset(b, 0, sizeof *b); memcpy(b->bssid, bssid, 6); b->rssi = -200; }
    memcpy(b->ssid, ssid, sizeof ssid); b->ch = chan; b->sec = sec;
    if (rssi > b->rssi) b->rssi = rssi;
    if (is_probe_resp) b->probes++; else b->beacons++;
}

static void on_rx(void *ctx, const uint8_t *f, uint32_t len, uint8_t ch, int rssi)
{
    (void)ctx;
    uint8_t type = f[0];
    if (type == 0x80) { heard++; note(f, len, ch, rssi, 0); }         /* beacon */
    else if (type == 0x50) { heard++; note(f, len, ch, rssi, 1); }    /* probe response */
}

/* broadcast probe request: hdr(24) + SSID(wildcard) + supported rates */
static uint32_t build_probe_req(uint8_t *f)
{
    static uint16_t seq;
    uint32_t n = 0;
    f[0] = 0x40; f[1] = 0x00;                                         /* probe request */
    f[2] = 0; f[3] = 0;                                               /* duration */
    memset(f + 4, 0xFF, 6);                                           /* DA broadcast */
    memcpy(f + 10, dev.mac, 6);                                       /* SA */
    memset(f + 16, 0xFF, 6);                                          /* BSSID broadcast */
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4); seq++;
    n = 24;
    f[n++] = 0; f[n++] = 0;                                           /* SSID: wildcard (len 0) */
    f[n++] = 1; f[n++] = 8;                                           /* supported rates */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;       /* 1,2,5.5,11 (basic) */
    f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;       /* 6,9,12,18 */
    return n;
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi active scan (RTL8192CE)\n");
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    pcie_map_bar2();
    struct wl_tables t = tables();
    int rc = wl_hw_init(&dev, &t);
    if (rc < 0) { printf("wl_hw_init failed %d\n", rc); pcie_power_down(); return 1; }
    printf("chip up, MAC %02x:%02x:%02x:%02x:%02x:%02x (%s)\n", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3],
           dev.mac[4], dev.mac[5], dev.fuse_ok ? "from fuse" : "default");
    /* diagnostic: pure RX on channel 2 (DLINK is there) before any TX */
    wl_hw_set_channel(&dev, 2);
    uint32_t h0 = heard, t0 = micros();
    while (elapsed_us(t0) < 3000000u) wl_hw_rx_poll(&dev, on_rx, 0);
    printf("pure RX 3 s on ch 2 (no TX yet): %u frames\n", heard - h0);

    wl_hw_tx_enable(&dev);
    h0 = heard; t0 = micros();
    while (elapsed_us(t0) < 3000000u) wl_hw_rx_poll(&dev, on_rx, 0);
    printf("pure RX 3 s on ch 2 (TX enabled, idle): %u frames\n", heard - h0);

    static uint8_t probe[128];
    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t ch = WL_CH_MIN; ch <= WL_CH_MAX; ch++) {
            wl_hw_set_channel(&dev, ch);
            delay_ms(5);
            uint32_t pr = build_probe_req(probe);
            for (int r = 0; r < 2; r++) { if (wl_hw_tx(&dev, probe, pr, 0, 50) == 0) sent++; delay_ms(8); }
            uint32_t h0 = heard, t0 = micros();
            while (elapsed_us(t0) < 250000u) wl_hw_rx_poll(&dev, on_rx, 0);
            printf("  pass %d ch %2u: probes sent, %u responses/beacons\n", pass + 1, ch, heard - h0);
        }
    }
    printf("probe requests acked by TX: %u; frames heard: %u\n", sent, heard);
    printf("%u networks:\n  BSSID              ch  signal  security   bcn/probe  SSID\n", nbss);
    static const char *const sn[] = { "open", "WEP", "WPA", "WPA2", "WPA/WPA2" };
    for (uint32_t i = 0; i < nbss; i++) {
        struct bss *b = &list[i];
        printf("  %02x:%02x:%02x:%02x:%02x:%02x  %2u  %4d dBm  %-9s  %3u/%-3u    %s\n", b->bssid[0], b->bssid[1],
               b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], b->ch, b->rssi, sn[b->sec < 5 ? b->sec : 0],
               b->beacons, b->probes, b->ssid[0] ? b->ssid : "(hidden)");
    }
    pcie_power_down();
    printf("done\n");
    return 0;
}
