/*
 * 28_wifi_ap - soft-AP demo: beacon an OPEN test network and answer probe,
 * authentication and association requests, so a phone or laptop can see and
 * join it.  No encryption and no data forwarding yet: once a client
 * associates it is on the "network" but there is no DHCP or internet.
 *
 * SSID "OpenRTL8196C" on channel 7 (quiet here).  Open system only.
 * Tables + firmware: sdk/include/wifi_blobs.inc (tools/extract_wifi_blobs.py).
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "pcie.h"
#include "wl.h"
#include "libc.h"
#include "wifi_blobs.inc"

#define SSID "OpenRTL8196C"
#define CHAN 7

static struct wl_dev dev;
static uint8_t frame[256];
static uint16_t seq;
static struct { uint8_t mac[6]; int state; } clients[8];   /* state: 1 authed, 2 assoc */
static uint32_t nclients, beacons_sent, probes_ans, auths, assocs;

static struct wl_tables tables(void)
{
    struct wl_tables t = { wifi_fw, WIFI_FW_LEN, wifi_mac, WIFI_MAC_N, wifi_phy_2t, WIFI_PHY_2T_N,
        wifi_phy_fix, WIFI_PHY_FIX_N, wifi_agc, WIFI_AGC_N, wifi_radio_a, WIFI_RADIO_A_N,
        wifi_radio_b, WIFI_RADIO_B_N };
    return t;
}

static uint32_t hdr(uint8_t *f, uint16_t fc, const uint8_t *da)
{
    f[0] = (uint8_t)fc; f[1] = (uint8_t)(fc >> 8);
    f[2] = 0; f[3] = 0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, dev.mac, 6);                            /* SA */
    memcpy(f + 16, dev.mac, 6);                            /* BSSID = us */
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4); seq++;
    return 24;
}

/* fixed fields + IEs common to beacon and probe response */
static uint32_t body_bss(uint8_t *f, uint32_t n)
{
    memset(f + n, 0, 8); n += 8;                           /* timestamp (hw fills) */
    f[n++] = 0x64; f[n++] = 0x00;                          /* beacon interval 100 TU */
    f[n++] = 0x01; f[n++] = 0x00;                          /* capability: ESS, open */
    f[n++] = 0; f[n++] = (uint8_t)strlen(SSID); memcpy(f + n, SSID, strlen(SSID)); n += strlen(SSID);
    f[n++] = 1; f[n++] = 8;                                /* supported rates */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96; f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;
    f[n++] = 3; f[n++] = 1; f[n++] = CHAN;                 /* DS parameter set */
    f[n++] = 5; f[n++] = 4; f[n++] = 0; f[n++] = 1; f[n++] = 0; f[n++] = 0;   /* TIM */
    f[n++] = 42; f[n++] = 1; f[n++] = 0x00;                /* ERP info: 11g, no protection */
    f[n++] = 50; f[n++] = 4; f[n++] = 0x30; f[n++] = 0x48; f[n++] = 0x60; f[n++] = 0x6C;  /* ext rates 24/36/48/54 */
    return n;
}

static void send_beacon(void)
{
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    uint32_t n = hdr(frame, 0x0080, bc);                   /* beacon */
    n = body_bss(frame, n);
    int rc = wl_hw_tx(&dev, frame, n, 0, 50);              /* normal queue: proven TX path */
    if (rc == 0) beacons_sent++;
    else if (beacons_sent == 0) { static int once; if (!once) { once = 1; printf("beacon TX rc=%d\n", rc); } }
}

static void on_rx(void *ctx, const uint8_t *f, uint32_t len, uint8_t ch, int rssi)
{
    (void)ctx; (void)ch; (void)rssi;
    if (len < 24) return;
    uint8_t type = f[0];
    const uint8_t *sa = f + 10, *da = f + 4;
    int to_us = !memcmp(da, dev.mac, 6), bcast = (da[0] & 1);
    if (to_us && type != 0x40) {               /* any non-probe management frame sent to us: trace it */
        printf("rx type %02x from %02x:%02x:%02x:%02x:%02x:%02x len %u\n", type, sa[0], sa[1], sa[2], sa[3],
               sa[4], sa[5], len);
    }

    if (type == 0x40) {                                    /* probe request */
        if (!to_us && !bcast) return;
        uint32_t n = hdr(frame, 0x0050, sa);               /* probe response */
        n = body_bss(frame, n);
        if (wl_hw_tx(&dev, frame, n, 0, 50) == 0) probes_ans++;
    } else if (type == 0xB0 && to_us) {                    /* authentication */
        uint32_t n = hdr(frame, 0x00B0, sa);
        frame[n++] = 0; frame[n++] = 0;                    /* algo: open */
        frame[n++] = 2; frame[n++] = 0;                    /* seq 2 */
        frame[n++] = 0; frame[n++] = 0;                    /* status: success */
        wl_hw_tx(&dev, frame, n, 0, 50);
        int known = 0; for (uint32_t i = 0; i < nclients; i++) if (!memcmp(clients[i].mac, sa, 6)) known = 1;
        if (!known && nclients < 8) { memcpy(clients[nclients].mac, sa, 6); clients[nclients].state = 1; nclients++; }
        auths++;
        printf("auth from %02x:%02x:%02x:%02x:%02x:%02x -> success\n", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
    } else if ((type == 0x00 || type == 0x20) && to_us) {  /* (re)association request */
        uint32_t n = hdr(frame, 0x0010, sa);               /* association response */
        frame[n++] = 0x01; frame[n++] = 0x00;              /* capability */
        frame[n++] = 0; frame[n++] = 0;                    /* status: success */
        frame[n++] = 0xC1; frame[n++] = 0x00;              /* AID 1 */
        frame[n++] = 1; frame[n++] = 8;
        frame[n++] = 0x82; frame[n++] = 0x84; frame[n++] = 0x8B; frame[n++] = 0x96; frame[n++] = 0x0C; frame[n++] = 0x12; frame[n++] = 0x18; frame[n++] = 0x24;
        wl_hw_tx(&dev, frame, n, 0, 50);
        for (uint32_t i = 0; i < nclients; i++) if (!memcmp(clients[i].mac, sa, 6)) clients[i].state = 2;
        assocs++;
        printf("assoc from %02x:%02x:%02x:%02x:%02x:%02x -> success (AID 1)\n", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
    }
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nwifi soft-AP demo: SSID \"%s\" (open) on channel %d\n", SSID, CHAN);
    if (pcie_init(0) < 0) { printf("pcie: no link\n"); return 1; }
    pcie_map_bar2();
    struct wl_tables t = tables();
    int rc = wl_hw_init(&dev, &t);
    if (rc < 0) { printf("wl_hw_init failed %d\n", rc); pcie_power_down(); return 1; }
    printf("AP BSSID %02x:%02x:%02x:%02x:%02x:%02x\n", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
    wl_hw_set_channel(&dev, CHAN);
    /* Stay in no-link mode: AP opmode gates TX on beacon/TBTT timing we do not
     * drive.  The MAC still auto-ACKs frames sent to our address (MACID), so
     * open auth/assoc can complete; beacons go out from the normal TX queue. */
    wl_hw_tx_enable(&dev);
    printf("beaconing; join \"%s\" from a phone (no password). Ctrl-C to stop.\n", SSID);

    uint32_t last_bcn = micros(), last_log = micros();
    for (;;) {
        wl_hw_rx_poll(&dev, on_rx, 0);
        if ((elapsed_us(last_bcn) & 0xFFFFFFFFu) >= 100000u) { send_beacon(); last_bcn = micros(); }
        if ((elapsed_us(last_log) & 0xFFFFFFFFu) >= 5000000u) {
            printf("alive: %u beacons, %u probes answered, %u auth, %u assoc, %u clients\n",
                   beacons_sent, probes_ans, auths, assocs, nclients);
            last_log = micros();
        }
    }
}
