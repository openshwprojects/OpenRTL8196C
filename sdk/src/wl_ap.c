/*
 * wl_ap.c - soft AP, see wl_ap.h
 *
 * Frame formats used here (802.11, little-endian fields):
 *   management: FC(2) dur(2) DA(6) SA(6) BSSID(6) seq(2) body
 *   data from a station (ToDS):   addr1 = BSSID, addr2 = SA, addr3 = DA
 *   data to a station (FromDS):   addr1 = DA, addr2 = BSSID, addr3 = SA
 *   data body: LLC/SNAP AA AA 03 00 00 00 + ethertype(2) + payload
 */
#include "wl_ap.h"
#include "timer.h"
#include "uart.h"
#include "libc.h"

static struct wl_dev *dev;
static char ap_ssid[33];
static uint8_t ap_chan;
static uint16_t seq;
static uint8_t beacon[256];
static uint32_t beacon_len;
static struct wl_ap_client clients[WL_AP_MAX_CLIENTS];
static int nclients;
struct wl_ap_stats wl_ap_stats;
int wl_ap_sw_beacons = 0;
int wl_ap_trace = 1;          /* print the first frames stations send to us (not probe requests) */     /* software beacons from the normal queue: measured to deter clients, off by default */

/* Ethernet frames converted from received data, waiting for net_poll */
#define RXQ 8
static uint8_t rxq[RXQ][1600];
static uint32_t rxq_len[RXQ];
static uint32_t rxq_head, rxq_tail;              /* head = next to hand out, tail = next free */
static uint8_t txframe[1600 + 32];

static uint32_t mgmt_hdr(uint8_t *f, uint8_t subtype, const uint8_t *da)
{
    f[0] = subtype; f[1] = 0;
    f[2] = 0; f[3] = 0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, dev->mac, 6);
    memcpy(f + 16, dev->mac, 6);
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4); seq++;
    return 24;
}

static uint32_t put_rates(uint8_t *f, uint32_t n)
{
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const uint8_t ext[4] = { 0x30, 0x48, 0x60, 0x6C };
    f[n++] = 1; f[n++] = 8; memcpy(f + n, rates, 8); n += 8;
    f[n++] = 50; f[n++] = 4; memcpy(f + n, ext, 4); n += 4;
    return n;
}

/* the chip's TSF (microseconds since the MAC started), little-endian into the frame */
static void put_tsf(uint8_t *p)
{
    uint32_t lo = wl_rd32(dev, 0x560), hi = wl_rd32(dev, 0x564);   /* REG_TSFTR */
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(lo >> (8 * i)); p[4 + i] = (uint8_t)(hi >> (8 * i)); }
}

/* beacon / probe response body */
static uint32_t bss_body(uint8_t *f, uint32_t n, int with_tim)
{
    put_tsf(f + n); n += 8;                        /* timestamp: the chip's TSF, so clients see it advance */
    f[n++] = 0x64; f[n++] = 0x00;                  /* beacon interval 100 TU */
    f[n++] = 0x01; f[n++] = 0x00;                  /* capability: ESS, open (the joinable recipe) */
    uint32_t sl = strlen(ap_ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ap_ssid, sl); n += sl;
    f[n++] = 1; f[n++] = 8;
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    memcpy(f + n, rates, 8); n += 8;
    f[n++] = 3; f[n++] = 1; f[n++] = ap_chan;      /* DS parameter set */
    if (with_tim) { f[n++] = 5; f[n++] = 4; f[n++] = 0; f[n++] = 1; f[n++] = 0; f[n++] = 0; }
    f[n++] = 42; f[n++] = 1; f[n++] = 0x00;        /* ERP */
    f[n++] = 50; f[n++] = 4; f[n++] = 0x30; f[n++] = 0x48; f[n++] = 0x60; f[n++] = 0x6C;
    return n;
}

static struct wl_ap_client *client_find(const uint8_t *mac)
{
    for (int i = 0; i < nclients; i++) if (!memcmp(clients[i].mac, mac, 6)) return &clients[i];
    return 0;
}

static struct wl_ap_client *client_add(const uint8_t *mac)
{
    struct wl_ap_client *c = client_find(mac);
    if (c) return c;
    if (nclients >= WL_AP_MAX_CLIENTS) {           /* evict the oldest */
        int o = 0;
        for (int i = 1; i < nclients; i++) if (clients[i].last_seen_ms < clients[o].last_seen_ms) o = i;
        c = &clients[o];
    } else {
        c = &clients[nclients++];
    }
    memset(c, 0, sizeof *c);
    memcpy(c->mac, mac, 6);
    c->aid = (uint8_t)((c - clients) + 1);
    return c;
}

static void client_remove(const uint8_t *mac)
{
    for (int i = 0; i < nclients; i++)
        if (!memcmp(clients[i].mac, mac, 6)) { clients[i] = clients[nclients - 1]; nclients--; return; }
}

static int tx(const uint8_t *f, uint32_t n)
{
    int rc = wl_hw_tx(dev, f, n, 0, 30);
    if (rc < 0) wl_ap_stats.tx_fail++;
    return rc;
}

/* probe request: respond when the SSID is ours or the wildcard */
static void on_probe_req(const uint8_t *f, uint32_t len, const uint8_t *sa)
{
    wl_ap_stats.probe_req++;
    int match = 0;
    for (uint32_t o = 24; o + 2 <= len;) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > len) break;
        if (id == 0) { match = (l == 0) || (l == strlen(ap_ssid) && !memcmp(f + o + 2, ap_ssid, l)); break; }
        o += 2u + l;
    }
    if (!match) return;
    if (f[25] != 0) wl_ap_stats.probe_directed++;               /* SSID element non-empty: asked for us by name */
    uint32_t n = mgmt_hdr(txframe, 0x50, sa);
    n = bss_body(txframe, n, 0);
    if (tx(txframe, n) == 0) wl_ap_stats.probe_resp++;
}

static void on_auth(const uint8_t *f, uint32_t len, const uint8_t *sa, int rssi)
{
    if (len < 30) return;
    uint16_t algo = (uint16_t)(f[24] | (f[25] << 8)), aseq = (uint16_t)(f[26] | (f[27] << 8));
    if (algo != 0 || aseq != 1) return;            /* open system, first message only */
    uint32_t n = mgmt_hdr(txframe, 0xB0, sa);
    txframe[n++] = 0; txframe[n++] = 0;            /* open */
    txframe[n++] = 2; txframe[n++] = 0;            /* sequence 2 */
    txframe[n++] = 0; txframe[n++] = 0;            /* status success */
    if (tx(txframe, n) == 0) {
        struct wl_ap_client *c = client_add(sa);
        c->state = 1; c->rssi = rssi; c->last_seen_ms = timer_ms();
        wl_ap_stats.auth++;
    }
}

static void on_assoc(const uint8_t *f, uint32_t len, const uint8_t *sa, int reassoc, int rssi)
{
    (void)len;
    struct wl_ap_client *c = client_add(sa);
    uint32_t n = mgmt_hdr(txframe, reassoc ? 0x30 : 0x10, sa);
    txframe[n++] = 0x01; txframe[n++] = 0x04;      /* capability */
    txframe[n++] = 0; txframe[n++] = 0;            /* status success */
    txframe[n++] = (uint8_t)(0xC0 | c->aid); txframe[n++] = 0xC0;   /* AID with the two top bits set */
    n = put_rates(txframe, n);
    if (tx(txframe, n) == 0) {
        c->state = 2; c->rssi = rssi; c->last_seen_ms = timer_ms();
        wl_ap_stats.assoc++;
    }
    (void)f;
}

/* data frame from a station -> Ethernet frame into the queue for net.c */
static void on_data(const uint8_t *f, uint32_t len, int rssi)
{
    uint8_t fc0 = f[0], fc1 = f[1];
    if (!(fc1 & 0x01) || (fc1 & 0x02) || (fc1 & 0x40)) { wl_ap_stats.rx_drop++; return; }   /* need ToDS, no FromDS, not protected */
    if (memcmp(f + 4, dev->mac, 6) != 0) return;                /* addr1 = BSSID must be us */
    uint32_t hl = 24 + ((fc0 & 0x80) ? 2 : 0);                   /* QoS data has 2 more header bytes */
    if ((fc0 & 0xF0) == 0x40 || (fc0 & 0xF0) == 0xC0) return;   /* null / QoS null: nothing to bridge */
    if (len < hl + 8) { wl_ap_stats.rx_drop++; return; }
    const uint8_t *llc = f + hl;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03)) { wl_ap_stats.rx_drop++; return; }
    uint32_t plen = len - hl - 8;
    if (14 + plen > 1600) { wl_ap_stats.rx_drop++; return; }
    uint32_t next = (rxq_tail + 1) % RXQ;
    if (next == rxq_head) { wl_ap_stats.rx_queue_full++; return; }
    uint8_t *e = rxq[rxq_tail];
    memcpy(e, f + 16, 6);                                        /* dst = addr3 */
    memcpy(e + 6, f + 10, 6);                                    /* src = addr2 */
    e[12] = llc[6]; e[13] = llc[7];                              /* ethertype */
    memcpy(e + 14, llc + 8, plen);
    rxq_len[rxq_tail] = 14 + plen;
    rxq_tail = next;
    wl_ap_stats.data_rx++;
    struct wl_ap_client *c = client_find(f + 10);
    if (c) { c->last_seen_ms = timer_ms(); c->rssi = rssi; }
}

static void on_rx(void *ctx, const uint8_t *f, uint32_t len, uint8_t ch, int rssi)
{
    (void)ctx; (void)ch;
    if (len < 24) return;
    uint8_t type = f[0] & 0x0C, sub = f[0] & 0xF0;
    const uint8_t *sa = f + 10, *da = f + 4;
    int to_us = !memcmp(da, dev->mac, 6);
    if (to_us && !(type == 0x00 && sub == 0x40) && wl_ap_trace) {    /* anything but probe requests */
        static uint32_t traced;
        if (traced++ < 40)
            printf("ap rx: fc %02x%02x from %02x:%02x:%02x:%02x:%02x:%02x len %u\n", f[0], f[1], sa[0], sa[1], sa[2],
                   sa[3], sa[4], sa[5], len);
    }
    if (type == 0x00) {                                          /* management */
        if (sub == 0x40) { if (to_us || (da[0] & 1)) on_probe_req(f, len, sa); }
        else if (!to_us) return;
        else if (sub == 0xB0) on_auth(f, len, sa, rssi);
        else if (sub == 0x00) on_assoc(f, len, sa, 0, rssi);
        else if (sub == 0x20) on_assoc(f, len, sa, 1, rssi);
        else if (sub == 0xA0 || sub == 0xC0) { client_remove(sa); wl_ap_stats.deauth++; }
    } else if (type == 0x08) {                                   /* data */
        on_data(f, len, rssi);
    }
}

/* ---- net_link ---- */
static int link_recv(uint8_t **frame, uint32_t *len, int *port)
{
    wl_hw_rx_poll(dev, on_rx, 0);
    if (rxq_head == rxq_tail) return 0;
    *frame = rxq[rxq_head]; *len = rxq_len[rxq_head]; *port = 0;
    return 1;
}
static void link_recv_done(void) { if (rxq_head != rxq_tail) rxq_head = (rxq_head + 1) % RXQ; }

static int link_send(const void *frame, uint32_t len, uint8_t portmask)
{
    (void)portmask;
    const uint8_t *e = frame;
    if (len < 14 || len + 18 > sizeof txframe) return -1;
    uint8_t *f = txframe;
    f[0] = 0x08; f[1] = 0x02;                                    /* data, FromDS */
    f[2] = 0; f[3] = 0;
    memcpy(f + 4, e, 6);                                         /* addr1 = DA */
    memcpy(f + 10, dev->mac, 6);                                 /* addr2 = BSSID */
    memcpy(f + 16, e + 6, 6);                                    /* addr3 = SA */
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4); seq++;
    f[24] = 0xAA; f[25] = 0xAA; f[26] = 0x03; f[27] = 0; f[28] = 0; f[29] = 0;
    f[30] = e[12]; f[31] = e[13];
    memcpy(f + 32, e + 14, len - 14);
    int rc = tx(f, 32 + len - 14);
    if (rc == 0) wl_ap_stats.data_tx++;
    return rc;
}

static const struct net_link ap_link = { link_recv, link_recv_done, link_send, 0 };
const struct net_link *wl_ap_link(void) { return &ap_link; }

static int hw_mode;

int wl_ap_start(struct wl_dev *d, const char *ssid, uint8_t chan, int hw_beacons)
{
    dev = d;
    ap_chan = chan;
    hw_mode = hw_beacons;
    strncpy(ap_ssid, ssid, 32); ap_ssid[32] = 0;
    memset(&wl_ap_stats, 0, sizeof wl_ap_stats);
    nclients = 0; rxq_head = rxq_tail = 0;
    /* Clear any hardware security / BSSID filter left over from station mode */
    wl_wr8(d, 0x0680, 0x00);
    wl_wr8(d, 0x0101, wl_rd8(d, 0x0101) & ~0x02);
    wl_wr32(d, 0x0608, wl_rd32(d, 0x0608) & ~((1u << 6) | (1u << 7) | (1u << 9)));
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    beacon_len = mgmt_hdr(beacon, 0x80, bc);
    beacon_len = bss_body(beacon, beacon_len, 1);
    if (hw_beacons) {
        wl_hw_ap_start(d, chan);
        wl_hw_tx_enable(d);
        wl_hw_beacon_update(d, beacon, beacon_len);
    } else {
        /* examples/28 recipe: no-link mode, BSSID register set (no BSSID RX
         * filter, so broadcast probes and DHCP still arrive), software beacons */
        wl_hw_set_opmode(d, 0);
        wl_hw_set_channel(d, chan);
        for (int i = 0; i < 6; i++) wl_wr8(d, 0x618 + i, d->mac[i]);
        wl_ap_sw_beacons = 1;
        wl_hw_tx_enable(d);
    }
    return 0;
}

void wl_ap_stop(void)
{
    wl_ap_sw_beacons = 0;
    hw_mode = 0;
    nclients = 0;
    rxq_head = rxq_tail = 0;
}

void wl_ap_poll(void)
{
    if (!dev) return;
    /* hardware beacon queue (sent at TBTT) when it accepts one; otherwise a
     * software beacon from the normal queue every 100 ms, so clients never
     * see beacon loss even if the TBTT path is idle */
    static uint32_t last_ms;
    uint32_t now = timer_ms();
    if (hw_mode && wl_hw_beacon_update(dev, beacon, beacon_len)) { wl_ap_stats.beacons++; last_ms = now; return; }
    if (wl_ap_sw_beacons && now - last_ms >= 100u) {
        last_ms = now;
        /* fresh sequence number and timestamp for every beacon */
        beacon[22] = (uint8_t)(seq << 4); beacon[23] = (uint8_t)(seq >> 4); seq++;
        put_tsf(beacon + 24);
        if (wl_hw_tx(dev, beacon, beacon_len, 0, 30) == 0) { wl_ap_stats.beacons++; wl_ap_stats.sw_beacons++; }
    }
}

int wl_ap_clients(const struct wl_ap_client **out) { *out = clients; return nclients; }
