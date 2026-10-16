/*
 * wl_sta.c - Wi-Fi station, see wl_sta.h
 *
 * Frames (802.11, little-endian fields):
 *   to the AP (ToDS):   addr1 = BSSID, addr2 = us, addr3 = DA
 *   from the AP (FromDS): addr1 = us/broadcast, addr2 = BSSID, addr3 = SA
 *   protected data: 8-byte CCMP header after the MAC header (PN0 PN1 00
 *   ExtIV|keyid<<6 PN2..PN5), the chip encrypts and appends the 8-byte MIC;
 *   received frames arrive decrypted but still carry header and MIC.
 * EAPOL-Key (IEEE 802.11-2016 12.7): descriptor type 2, key info with the
 * descriptor version 2 (HMAC-SHA1-128 MIC, AES key wrap).
 */
#include "wl_sta.h"
#include "wpa_crypto.h"
#include "timer.h"
#include "uart.h"
#include "libc.h"

/* key table (rtlwifi cam.h) */
#define REG_CAMCMD   0x0670
#define REG_CAMWRITE 0x0674
#define REG_SECCFG   0x0680
#define CAM_AES      0x04
#define CAM_VALID    (1u << 15)
#define CAM_PAIRWISE_ENTRY 4

static struct wl_dev *dev;
static char ssid[33];
static char pass[64];
static struct wl_sta_info info;
static uint16_t seq;
static uint32_t t_state, t_last_tx;
static uint8_t scan_chan;
static uint8_t pmk[32], ptk[48], snonce[32], anonce[32], replay[8];
static int have_pmk;
static uint8_t rsn_ie[22];                                  /* our RSN element for assoc + EAPOL msg 2 */
static uint8_t txframe[1700];
static uint32_t tx_pn_lo, tx_pn_hi;                         /* CCMP packet number */

#define RXQ 8
static uint8_t rxq[RXQ][1600];
static uint32_t rxq_len[RXQ], rxq_head, rxq_tail;

int wl_sta_trace = 0;         /* set to 1 to print the first received data frames */

static const char *const state_names[] = { "idle", "scanning", "authenticating", "associating", "handshake",
                                           "connected", "failed" };
const char *wl_sta_state_name(enum wl_sta_state s) { return s <= WL_STA_FAILED ? state_names[s] : "?"; }
const struct wl_sta_info *wl_sta_info(void) { return &info; }

static void set_state(enum wl_sta_state s) { info.state = s; t_state = timer_ms(); }
static void fail(const char *why) { info.fail_reason = why; set_state(WL_STA_FAILED); }

static uint32_t mgmt_hdr(uint8_t *f, uint8_t subtype, const uint8_t *da)
{
    f[0] = subtype; f[1] = 0; f[2] = 0; f[3] = 0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, dev->mac, 6);
    memcpy(f + 16, da, 6);                                   /* BSSID = the AP */
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

static int tx(const uint8_t *f, uint32_t n)
{
    int rc = wl_hw_tx(dev, f, n, 0, 30);
    if (rc < 0) info.tx_fail++;
    t_last_tx = timer_ms();
    return rc;
}

/* ---------------------------------------------------------- management */
static void send_probe(void)
{
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    uint32_t n = mgmt_hdr(txframe, 0x40, bc);
    uint32_t sl = strlen(ssid);
    txframe[n++] = 0; txframe[n++] = (uint8_t)sl; memcpy(txframe + n, ssid, sl); n += sl;
    n = put_rates(txframe, n);
    tx(txframe, n);
}

static void send_auth(void)
{
    uint32_t n = mgmt_hdr(txframe, 0xB0, info.bssid);
    txframe[n++] = 0; txframe[n++] = 0;                      /* open system */
    txframe[n++] = 1; txframe[n++] = 0;                      /* sequence 1 */
    txframe[n++] = 0; txframe[n++] = 0;
    tx(txframe, n);
}

static void send_assoc(void)
{
    uint32_t n = mgmt_hdr(txframe, 0x00, info.bssid);
    txframe[n++] = (uint8_t)(0x01 | (pass[0] ? 0x10 : 0)); /* ESS, privacy when WPA2 */
    txframe[n++] = 0x04;                                     /* short preamble */
    txframe[n++] = 10; txframe[n++] = 0;                     /* listen interval */
    uint32_t sl = strlen(ssid);
    txframe[n++] = 0; txframe[n++] = (uint8_t)sl; memcpy(txframe + n, ssid, sl); n += sl;
    n = put_rates(txframe, n);
    if (pass[0]) { memcpy(txframe + n, rsn_ie, sizeof rsn_ie); n += sizeof rsn_ie; }
    tx(txframe, n);
}

/* ------------------------------------------------------------ data link */
static uint32_t data_hdr(uint8_t *f, const uint8_t *da, int protect)
{
    f[0] = 0x08; f[1] = (uint8_t)(0x01 | (protect ? 0x40 : 0));   /* data, ToDS, protected */
    f[2] = 0; f[3] = 0;
    memcpy(f + 4, info.bssid, 6);
    memcpy(f + 10, dev->mac, 6);
    memcpy(f + 16, da, 6);
    f[22] = (uint8_t)(seq << 4); f[23] = (uint8_t)(seq >> 4); seq++;
    uint32_t n = 24;
    if (protect) {                                           /* CCMP header, key id 0 */
        if (++tx_pn_lo == 0) tx_pn_hi++;
        f[n++] = (uint8_t)tx_pn_lo; f[n++] = (uint8_t)(tx_pn_lo >> 8);
        f[n++] = 0; f[n++] = 0x20;
        f[n++] = (uint8_t)(tx_pn_lo >> 16); f[n++] = (uint8_t)(tx_pn_lo >> 24);
        f[n++] = (uint8_t)tx_pn_hi; f[n++] = (uint8_t)(tx_pn_hi >> 8);
    }
    return n;
}

static int send_eapol(const uint8_t *body, uint32_t blen)
{
    uint32_t n = data_hdr(txframe, info.bssid, 0);          /* EAPOL goes out in the clear */
    static const uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E };
    memcpy(txframe + n, llc, 8); n += 8;
    memcpy(txframe + n, body, blen); n += blen;
    info.eapol_tx++;
    return tx(txframe, n);
}

/* ---------------------------------------------------- WPA2 4-way handshake */
static void build_rsn_ie(void)
{
    static const uint8_t ie[22] = { 48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4,          /* RSN v1, group CCMP */
                                    1, 0, 0x00, 0x0F, 0xAC, 4,                  /* 1 pairwise: CCMP */
                                    1, 0, 0x00, 0x0F, 0xAC, 2,                  /* 1 AKM: PSK */
                                    0, 0 };                                     /* RSN capabilities */
    memcpy(rsn_ie, ie, 22);
}

static void make_snonce(void)
{
    uint8_t seed[32];
    uint32_t t = wl_rd32(dev, 0x560), u = micros();
    memcpy(seed, dev->mac, 6);
    memcpy(seed + 6, &t, 4); memcpy(seed + 10, &u, 4);
    memcpy(seed + 14, info.bssid, 6);
    for (int i = 20; i < 32; i++) seed[i] = (uint8_t)(wl_rd32(dev, 0x560) >> (i & 7));
    sha1(seed, 32, snonce);
    sha1(snonce, 20, snonce + 12);
}

/* key info bits */
#define KI_VERSION_MASK 0x0007
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ENCRYPTED    0x1000

static void eapol_mic(uint8_t *eapol, uint32_t len)
{
    memset(eapol + 81, 0, 16);                               /* MIC field: bytes 81..96 of the EAPOL frame */
    uint8_t h[20];
    hmac_sha1(ptk, 16, eapol, len, h);                       /* KCK */
    memcpy(eapol + 81, h, 16);
}

/* EAPOL-Key frame = 4-byte 802.1X header + 95-byte key descriptor + key data */
static uint32_t build_eapol_key(uint8_t *e, uint16_t key_info, const uint8_t *nonce, const uint8_t *kdata,
                                uint16_t kdlen)
{
    memset(e, 0, 99);
    e[0] = 2; e[1] = 3;                                      /* 802.1X-2004, EAPOL-Key */
    uint16_t body = (uint16_t)(95 + kdlen);
    e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body;
    e[4] = 2;                                                /* RSN key descriptor */
    e[5] = (uint8_t)(key_info >> 8); e[6] = (uint8_t)key_info;
    e[7] = 0; e[8] = 0;                                      /* key length 0 in msg 2/4 */
    memcpy(e + 9, replay, 8);
    if (nonce) memcpy(e + 17, nonce, 32);
    e[97] = (uint8_t)(kdlen >> 8); e[98] = (uint8_t)kdlen;
    if (kdlen) memcpy(e + 99, kdata, kdlen);
    return 99u + kdlen;
}

static void cam_write(uint32_t entry, const uint8_t *mac, const uint8_t *key, uint32_t keyid, uint32_t alg)
{
    uint32_t cfg = CAM_VALID | (alg << 2) | keyid;
    for (int i = 7; i >= 0; i--) {
        uint32_t content;
        if (i == 0) content = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 24) | cfg;
        else if (i == 1) content = ((uint32_t)mac[5] << 24) | ((uint32_t)mac[4] << 16) | ((uint32_t)mac[3] << 8) | mac[2];
        else if (i <= 5) { const uint8_t *k = key + (i - 2) * 4; content = ((uint32_t)k[3] << 24) | ((uint32_t)k[2] << 16) | ((uint32_t)k[1] << 8) | k[0]; }
        else content = 0;
        wl_wr32(dev, REG_CAMWRITE, content);
        wl_wr32(dev, REG_CAMCMD, (1u << 31) | (1u << 16) | (entry * 8 + (uint32_t)i));
        for (int n = 0; n < 100 && (wl_rd32(dev, REG_CAMCMD) & (1u << 31)); n++) ;
    }
}

static void install_keys(const uint8_t *gtk, uint32_t gtk_len, uint32_t gtk_id)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    wl_wr32(dev, REG_CAMCMD, (1u << 31) | (1u << 30));      /* clear all entries */
    delay_ms(1);
    cam_write(CAM_PAIRWISE_ENTRY, info.bssid, ptk + 32, 0, CAM_AES);          /* TK, key id 0 */
    if (gtk_len >= 16) cam_write(gtk_id, bcast, gtk, gtk_id, CAM_AES);
    wl_wr8(dev, 0x101, wl_rd8(dev, 0x101) | 0x02);          /* CR: security engine */
    wl_wr8(dev, REG_SECCFG, 0x0C | 0xC0);                   /* TXENC, RXDEC, TX/RX broadcast use default key */
    if (wl_sta_trace) wl_wr32(dev, 0x608, wl_rd32(dev, 0x608) | (1u << 9));   /* RCR AICV: deliver undecryptable frames too (diagnostic) */
    tx_pn_lo = 0; tx_pn_hi = 0;
    info.secured = 1;
}

static void on_eapol(const uint8_t *e, uint32_t len)
{
    info.eapol_rx++;
    if (len < 99 || e[1] != 3 || e[4] != 2) return;
    uint16_t ki = (uint16_t)((e[5] << 8) | e[6]);
    uint16_t kdlen = (uint16_t)((e[97] << 8) | e[98]);
    if (99u + kdlen > len) return;
    if ((ki & KI_VERSION_MASK) != 2) { fail("AP wants a key descriptor version we do not do (TKIP/WPA1?)"); return; }

    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK) && !(ki & KI_INSTALL)) {
        /* message 1: ANonce */
        memcpy(anonce, e + 17, 32);
        memcpy(replay, e + 9, 8);
        make_snonce();
        wpa_ptk(pmk, info.bssid, dev->mac, anonce, snonce, ptk);
        uint8_t m2[99 + 22];
        uint32_t n = build_eapol_key(m2, 2 | KI_PAIRWISE | KI_MIC, snonce, rsn_ie, 22);
        eapol_mic(m2, n);
        send_eapol(m2, n);
        return;
    }
    if ((ki & (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) == (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) {
        /* message 3: verify the MIC, unwrap the key data, find the GTK */
        uint8_t copy[600];
        if (len > sizeof copy) return;
        memcpy(copy, e, len);
        uint8_t mic[16]; memcpy(mic, e + 81, 16);
        eapol_mic(copy, len);
        if (memcmp(mic, copy + 81, 16) != 0) { fail("message 3 MIC mismatch (wrong passphrase?)"); return; }
        if (memcmp(e + 17, anonce, 32) != 0) { fail("message 3 ANonce differs"); return; }
        memcpy(replay, e + 9, 8);
        uint8_t kd[512]; uint32_t kdl = 0;
        if (ki & KI_ENCRYPTED) {
            if (kdlen > sizeof kd + 8 || aes_key_unwrap(ptk + 16, e + 99, kdlen, kd) < 0) { fail("key data unwrap failed"); return; }
            kdl = kdlen - 8;
        } else { memcpy(kd, e + 99, kdlen); kdl = kdlen; }
        const uint8_t *gtk = 0; uint32_t gtk_len = 0, gtk_id = 1;
        for (uint32_t o = 0; o + 2 <= kdl;) {
            uint8_t id = kd[o], l = kd[o + 1];
            if (id == 0xDD && l == 0) break;
            if (o + 2 + l > kdl) break;
            if (id == 0xDD && l >= 6 && kd[o + 2] == 0x00 && kd[o + 3] == 0x0F && kd[o + 4] == 0xAC && kd[o + 5] == 1) {
                gtk_id = kd[o + 6] & 3; gtk = kd + o + 8; gtk_len = l - 6u;
            }
            o += 2u + l;
        }
        uint8_t m4[99];
        uint32_t n = build_eapol_key(m4, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0);
        eapol_mic(m4, n);
        send_eapol(m4, n);
        info.gtk_len = (uint8_t)gtk_len; info.gtk_id = (uint8_t)gtk_id;
        install_keys(gtk, gtk_len, gtk_id);
        set_state(WL_STA_CONNECTED);
        return;
    }
}

/* -------------------------------------------------------------- receive */
static void note_bss(const uint8_t *f, uint32_t len, uint8_t ch, int rssi)
{
    if (len < 36) return;
    const uint8_t *s = 0; uint8_t sl = 0, chan = ch;
    for (uint32_t o = 36; o + 2 <= len;) {
        uint8_t id = f[o], l = f[o + 1]; const uint8_t *v = f + o + 2;
        if (o + 2 + l > len) break;
        if (id == 0) { s = v; sl = l; }
        if (id == 3 && l >= 1) chan = v[0];
        if (id == 48 && l >= 8) {                            /* RSN: group cipher, pairwise list */
            info.ap_group_cipher = v[5];
            uint16_t np = (uint16_t)(v[6] | (v[7] << 8));
            for (uint16_t k = 0; k < np && 8 + 4 * k + 3 < l; k++) {
                uint8_t c = v[8 + 4 * k + 3];
                if (c == 4) info.ap_pair_ccmp = 1;
                if (c == 2) info.ap_pair_tkip = 1;
            }
        }
        o += 2u + l;
    }
    if (!s || sl != strlen(ssid) || memcmp(s, ssid, sl) != 0) return;
    memcpy(info.bssid, f + 16, 6);
    info.chan = chan; info.rssi = rssi;
    wl_hw_set_channel(dev, chan);
    wl_hw_set_bssid(dev, info.bssid);
    wl_hw_set_opmode(dev, 2);                                /* infrastructure station */
    set_state(WL_STA_AUTHENTICATING);
    send_auth();
}

static void on_data(const uint8_t *f, uint32_t len)
{
    uint8_t fc0 = f[0], fc1 = f[1];
    if (!(fc1 & 0x02) || (fc1 & 0x01)) { info.rx_drop++; return; }   /* FromDS only */
    if (memcmp(f + 10, info.bssid, 6) != 0) return;                   /* from our AP */
    if (memcmp(f + 4, dev->mac, 6) != 0 && !(f[4] & 1)) return;      /* to us or broadcast */
    uint32_t hl = 24 + ((fc0 & 0x80) ? 2 : 0);                      /* QoS data: 2 more header bytes */
    if ((fc0 & 0xF0) == 0x40 || (fc0 & 0xF0) == 0xC0) return;        /* null data */
    /* protected frames keep their 8-byte CCMP header; the chip strips the MIC
     * (measured: ARP arrived as 20 bytes when 8 were subtracted for a MIC) */
    uint32_t tail = 0;
    if (fc1 & 0x40) hl += 8;
    if (len < hl + 8 + tail) { info.rx_drop++; return; }
    const uint8_t *llc = f + hl;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03)) {
        if (fc1 & 0x40) info.rx_undecrypted++; else info.rx_drop++;
        return;
    }
    uint32_t plen = len - hl - 8 - tail;
    uint16_t type = (uint16_t)((llc[6] << 8) | llc[7]);
    if (f[4] & 1) info.rx_bcast++; else info.rx_unicast++;
    if (wl_sta_trace) {
        static int shown;
        if (shown < 8) {
            shown++;
            const uint8_t *p = llc + 8;
            printf("sta rx: %s %s type %04x len %u", (f[4] & 1) ? "bcast" : "UNICAST", (fc1 & 0x40) ? "enc" : "clear",
                   type, plen);
            if (type == 0x0800 && plen >= 20) printf(" IP %u.%u.%u.%u -> %u.%u.%u.%u proto %u (IP total %u)", p[12], p[13],
                                                     p[14], p[15], p[16], p[17], p[18], p[19], p[9], (p[2] << 8) | p[3]);
            if (type == 0x0806 && plen >= 28) printf(" ARP %u.%u.%u.%u asks %u.%u.%u.%u", p[14], p[15], p[16], p[17], p[24],
                                                     p[25], p[26], p[27]);
            printf("\n");
        }
    }
    if (type == 0x888E) { on_eapol(llc + 8, plen); return; }
    if (info.state != WL_STA_CONNECTED && pass[0]) return;           /* no data before the keys */
    if (14 + plen > 1600) { info.rx_drop++; return; }
    uint32_t next = (rxq_tail + 1) % RXQ;
    if (next == rxq_head) { info.rx_drop++; return; }
    uint8_t *e = rxq[rxq_tail];
    memcpy(e, f + 4, 6);                                             /* dst = addr1 */
    memcpy(e + 6, f + 16, 6);                                        /* src = addr3 */
    e[12] = llc[6]; e[13] = llc[7];
    memcpy(e + 14, llc + 8, plen);
    rxq_len[rxq_tail] = 14 + plen;
    rxq_tail = next;
    info.data_rx++;
}

static void on_rx(void *ctx, const uint8_t *f, uint32_t len, uint8_t ch, int rssi)
{
    (void)ctx;
    if (len < 24) return;
    uint8_t type = f[0] & 0x0C, sub = f[0] & 0xF0;
    int to_us = !memcmp(f + 4, dev->mac, 6);
    if (type == 0x00) {
        if (info.state == WL_STA_SCANNING && (sub == 0x80 || sub == 0x50)) { note_bss(f, len, ch, rssi); return; }
        if (!to_us || memcmp(f + 16, info.bssid, 6) != 0) return;
        if (sub == 0xB0 && info.state == WL_STA_AUTHENTICATING && len >= 30) {
            uint16_t st = (uint16_t)(f[28] | (f[29] << 8));
            if (st != 0) { fail("authentication refused"); return; }
            set_state(WL_STA_ASSOCIATING);
            send_assoc();
        } else if ((sub == 0x10 || sub == 0x30) && info.state == WL_STA_ASSOCIATING && len >= 30) {
            uint16_t st = (uint16_t)(f[26] | (f[27] << 8));
            if (st != 0) { info.fail_reason = "association refused"; fail(info.fail_reason); return; }
            info.aid = (uint16_t)((f[28] | (f[29] << 8)) & 0x3FFF);
            set_state(pass[0] ? WL_STA_HANDSHAKE : WL_STA_CONNECTED);
        } else if (sub == 0xC0 || sub == 0xA0) {              /* deauth / disassoc */
            info.fail_reason = "AP sent deauthentication";
            info.secured = 0;
            set_state(WL_STA_SCANNING);
        }
    } else if (type == 0x08) {
        if (info.state >= WL_STA_ASSOCIATING) on_data(f, len);
    }
}

/* -------------------------------------------------------------- driving */
int wl_sta_start(struct wl_dev *d, const char *s, const char *passphrase)
{
    dev = d;
    strncpy(ssid, s, 32); ssid[32] = 0;
    strncpy(pass, passphrase ? passphrase : "", 63); pass[63] = 0;
    memset(&info, 0, sizeof info);
    rxq_head = rxq_tail = 0;
    build_rsn_ie();
    have_pmk = 0;
    if (pass[0]) { wpa_pmk(pass, (const uint8_t *)ssid, strlen(ssid), pmk); have_pmk = 1; }
    wl_hw_tx_enable(d);
    scan_chan = 1;
    wl_hw_set_channel(d, scan_chan);
    set_state(WL_STA_SCANNING);
    send_probe();
    return 0;
}

void wl_sta_poll(void)
{
    wl_hw_rx_poll(dev, on_rx, 0);
    uint32_t now = timer_ms();
    switch (info.state) {
    case WL_STA_SCANNING:
        if (now - t_last_tx >= 120u) {                       /* next channel */
            scan_chan = (uint8_t)(scan_chan >= 13 ? 1 : scan_chan + 1);
            wl_hw_set_channel(dev, scan_chan);
            send_probe();
        }
        break;
    case WL_STA_AUTHENTICATING:
        if (now - t_state > 3000u) { fail("no authentication response"); break; }
        if (now - t_last_tx >= 300u) { info.retries++; send_auth(); }
        break;
    case WL_STA_ASSOCIATING:
        if (now - t_state > 3000u) { fail("no association response"); break; }
        if (now - t_last_tx >= 300u) { info.retries++; send_assoc(); }
        break;
    case WL_STA_HANDSHAKE:
        if (now - t_state > 5000u) fail("4-way handshake timed out");
        break;
    case WL_STA_FAILED:
        if (now - t_state > 5000u) {                         /* try again from the scan */
            info.secured = 0;
            set_state(WL_STA_SCANNING);
            send_probe();
        }
        break;
    default: break;
    }
}

/* ---- net_link ---- */
static int link_recv(uint8_t **frame, uint32_t *len, int *port)
{
    wl_sta_poll();
    if (rxq_head == rxq_tail) return 0;
    *frame = rxq[rxq_head]; *len = rxq_len[rxq_head]; *port = 0;
    return 1;
}
static void link_recv_done(void) { if (rxq_head != rxq_tail) rxq_head = (rxq_head + 1) % RXQ; }
static int link_send(const void *frame, uint32_t len, uint8_t portmask)
{
    (void)portmask;
    if (info.state != WL_STA_CONNECTED) return -1;
    const uint8_t *e = frame;
    if (len < 14 || len + 40 > sizeof txframe) return -1;
    uint32_t n = data_hdr(txframe, e, info.secured);
    txframe[n++] = 0xAA; txframe[n++] = 0xAA; txframe[n++] = 0x03; txframe[n++] = 0; txframe[n++] = 0; txframe[n++] = 0;
    txframe[n++] = e[12]; txframe[n++] = e[13];
    memcpy(txframe + n, e + 14, len - 14); n += len - 14;
    int rc = tx(txframe, n);
    if (rc == 0) info.data_tx++;
    return rc;
}
static const struct net_link sta_link = { link_recv, link_recv_done, link_send, 0 };
const struct net_link *wl_sta_link(void) { return &sta_link; }
