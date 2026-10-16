/*
 * net.c - tiny polled IPv4 stack (ARP, ICMP echo, UDP)
 *
 * Every host we hear from is cached with its MAC and the switch port it
 * came in on, so replies go out of that port only.  Unknown destinations
 * are sent to the broadcast MAC on all ports (and an ARP request is sent
 * so the next packet goes direct).
 */
#include "rtl8196c.h"
#include "net.h"
#include "swcore.h"
#include "libc.h"

struct arp_entry { uint8_t ip[4]; uint8_t mac[6]; uint8_t port; uint8_t valid; };

static uint8_t our_mac[6], our_ip[4], alias_ip[4], active_ip[4];
static uint8_t our_mask[4] = { 255, 255, 255, 0 };
static uint8_t our_gw[4]   = { 192, 168, 0, 1 };
static uint8_t our_dns[4]  = { 192, 168, 0, 1 };
static struct arp_entry cache[NET_ARP_CACHE];
static unsigned cache_next;
static struct { uint16_t port; udp_handler_t h; } listeners[NET_MAX_UDP_LISTENERS];
static uint8_t txf[1600];
struct net_stats net_stats;
static const struct net_link *link;                /* NULL = Ethernet switch */

/* Secondary (Wi-Fi) interface state for dual-homed operation */
static const struct net_link *wifi_link;
static uint8_t wifi_mac[6], wifi_ip[4];
static uint8_t wifi_mask[4] = { 255, 255, 255, 0 };
static uint8_t wifi_gw[4]   = { 0, 0, 0, 0 };
static uint8_t wifi_dns[4]  = { 0, 0, 0, 0 };
static int cur_rx_port = -1;
static int tx_port_override = -1;

void net_set_link(const struct net_link *l) { link = l; }

void net_set_wifi_link(const struct net_link *wlink, const uint8_t mac[6])
{
    wifi_link = wlink;
    if (mac) memcpy(wifi_mac, mac, 6);
}
const uint8_t *net_wifi_mac(void)     { return wifi_mac; }
const uint8_t *net_wifi_ip(void)      { return wifi_ip; }
const uint8_t *net_wifi_netmask(void) { return wifi_mask; }
const uint8_t *net_wifi_gateway(void) { return wifi_gw; }
const uint8_t *net_wifi_dns(void)     { return wifi_dns; }
void net_set_wifi_ip(const uint8_t ip[4])     { memcpy(wifi_ip, ip, 4); }
void net_set_wifi_netmask(const uint8_t m[4]) { memcpy(wifi_mask, m, 4); }
void net_set_wifi_gateway(const uint8_t gw[4])
{
    memcpy(wifi_gw, gw, 4);
    if (gw[0] | gw[1] | gw[2] | gw[3]) {
        int saved = tx_port_override;
        tx_port_override = NET_PORT_WIFI;
        net_arp_request(gw);
        tx_port_override = saved;
    }
}
void net_set_wifi_dns(const uint8_t dns[4])   { memcpy(wifi_dns, dns, 4); }
int  net_rx_port(void)                        { return cur_rx_port; }
void net_set_tx_port(int port)                { tx_port_override = port; }

static int eth_any_link_up(void)
{
    for (int p = 0; p < NUM_PHY_PORTS; p++)
        if (eth_link_up(p)) return 1;
    return 0;
}

uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint32_t rd32(const uint8_t *p) { return ((uint32_t)rd16(p) << 16) | rd16(p + 2); }
void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)(v >> 16)); wr16(p + 2, (uint16_t)v); }

uint16_t net_l4_checksum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                         const uint8_t *seg, uint32_t len)
{
    uint32_t seed = 0;
    for (int i = 0; i < 4; i += 2) seed += rd16(src + i) + rd16(dst + i);
    seed += proto + len;
    return net_checksum(seg, len, seed);
}

void tcp_input(const uint8_t src_ip[4], const uint8_t *seg, uint32_t len);   /* tcp.c */
void tcp_poll(void);

uint16_t net_checksum(const uint8_t *p, uint32_t len, uint32_t seed)
{
    uint32_t sum = seed;
    while (len > 1) { sum += rd16(p); p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

const uint8_t *net_ip(void)        { return our_ip; }
const uint8_t *net_active_ip(void) { return active_ip; }
void net_set_active_ip(const uint8_t ip[4]) { memcpy(active_ip, ip, 4); }
const uint8_t *net_mac(void)       { return our_mac; }
const uint8_t *net_netmask(void)   { return our_mask; }
const uint8_t *net_gateway(void)   { return our_gw; }
const uint8_t *net_dns(void)       { return our_dns; }
void net_set_ip(const uint8_t ip[4])       { memcpy(our_ip, ip, 4); memcpy(active_ip, ip, 4); }
void net_set_netmask(const uint8_t m[4])   { memcpy(our_mask, m, 4); }
void net_set_gateway(const uint8_t gw[4])
{
    memcpy(our_gw, gw, 4);
    if (gw[0] | gw[1] | gw[2] | gw[3]) {
        int saved = tx_port_override;
        tx_port_override = 0;
        net_arp_request(gw);
        tx_port_override = saved;
    }
}
void net_set_dns(const uint8_t dns[4])     { memcpy(our_dns, dns, 4); }

static int is_eth_unicast(const uint8_t ip[4])
{
    if ((our_ip[0] | our_ip[1] | our_ip[2] | our_ip[3]) && memcmp(ip, our_ip, 4) == 0)
        return 1;
    if ((alias_ip[0] | alias_ip[1] | alias_ip[2] | alias_ip[3]) && memcmp(ip, alias_ip, 4) == 0)
        return 1;
    return 0;
}

static int is_wifi_unicast(const uint8_t ip[4])
{
    if (wifi_link && (wifi_ip[0] | wifi_ip[1] | wifi_ip[2] | wifi_ip[3]) && memcmp(ip, wifi_ip, 4) == 0)
        return 1;
    return 0;
}

static int is_our_unicast(const uint8_t ip[4])
{
    return is_eth_unicast(ip) || is_wifi_unicast(ip);
}

static int is_broadcast_ip(const uint8_t ip[4])
{
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255)
        return 1;
    if (ip[3] == 255 && memcmp(ip, our_ip, 3) == 0)
        return 1;
    if (wifi_link && (wifi_ip[0] | wifi_ip[1] | wifi_ip[2] | wifi_ip[3]) &&
        ip[3] == 255 && memcmp(ip, wifi_ip, 3) == 0)
        return 1;
    return 0;
}

static struct arp_entry *cache_find_if(const uint8_t ip[4], int want_wifi)
{
    for (int i = 0; i < NET_ARP_CACHE; i++) {
        if (!cache[i].valid || memcmp(cache[i].ip, ip, 4) != 0)
            continue;
        if (want_wifi < 0 || ((cache[i].port == NET_PORT_WIFI) == (want_wifi != 0)))
            return &cache[i];
    }
    return 0;
}

static void cache_learn(const uint8_t ip[4], const uint8_t mac[6], int port)
{
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0 || is_broadcast_ip(ip))
        return;
    struct arp_entry *e = cache_find_if(ip, port == NET_PORT_WIFI);
    if (!e) {
        e = &cache[cache_next];
        cache_next = (cache_next + 1) % NET_ARP_CACHE;
        memcpy(e->ip, ip, 4);
        e->valid = 1;
    }
    memcpy(e->mac, mac, 6);
    e->port = (uint8_t)port;
}

static int route_uses_wifi(const uint8_t dst_ip[4])
{
    if (!wifi_link) return 0;
    if (tx_port_override == NET_PORT_WIFI) return 1;
    if (tx_port_override >= 0 && tx_port_override < NET_PORT_WIFI) return 0;
    int wifi_has_ip = (wifi_ip[0] | wifi_ip[1] | wifi_ip[2] | wifi_ip[3]) != 0;
    if (wifi_has_ip) {
        /* Explicit reply from our Wi-Fi IP (e.g. TCP c->lip, ICMP reply, UDP) */
        if (memcmp(active_ip, wifi_ip, 4) == 0 && !is_eth_unicast(active_ip))
            return 1;
        /* Destination is on the Wi-Fi subnet (e.g. 192.168.4.x in AP mode) and not on the Ethernet subnet */
        if (dst_ip && !is_broadcast_ip(dst_ip)) {
            int in_wifi_sub = ((rd32(dst_ip) ^ rd32(wifi_ip)) & rd32(wifi_mask)) == 0;
            int in_eth_sub  = ((our_ip[0] | our_ip[1] | our_ip[2] | our_ip[3]) != 0) &&
                              (((rd32(dst_ip) ^ rd32(our_ip)) & rd32(our_mask)) == 0);
            if (in_wifi_sub && !in_eth_sub)
                return 1;
            if (!in_wifi_sub && !in_eth_sub && cache_find_if(dst_ip, 1) && !cache_find_if(dst_ip, 0))
                return 1;
        }
        /* Standalone wireless operation (no Ethernet cable plugged in) */
        if (!link && !eth_any_link_up())
            return 1;
    }
    if (cur_rx_port == NET_PORT_WIFI)
        return 1;
    return 0;
}

const uint8_t *net_select_src_ip(const uint8_t dst_ip[4])
{
    if (route_uses_wifi(dst_ip) && (wifi_ip[0] | wifi_ip[1] | wifi_ip[2] | wifi_ip[3]))
        return wifi_ip;
    return our_ip;
}

const uint8_t *net_select_dns(void)
{
    if (wifi_link && !link && !eth_any_link_up() &&
        (wifi_dns[0] | wifi_dns[1] | wifi_dns[2] | wifi_dns[3]))
        return wifi_dns;
    return our_dns;
}

void net_init(const uint8_t mac[6], const uint8_t ip[4])
{
    memcpy(our_mac, mac, 6);
    memcpy(our_ip, ip, 4);
    memcpy(alias_ip, ip, 4);
    memcpy(active_ip, ip, 4);
    memset(cache, 0, sizeof cache);
    memset(listeners, 0, sizeof listeners);
    memset(&net_stats, 0, sizeof net_stats);
    if (!link) eth_init(mac);
}

int net_udp_listen(uint16_t port, udp_handler_t h)
{
    if (!h) {
        for (int i = 0; i < NET_MAX_UDP_LISTENERS; i++)
            if (listeners[i].port == port) {
                listeners[i].port = 0;
                listeners[i].h = 0;
            }
        return 0;
    }
    for (int i = 0; i < NET_MAX_UDP_LISTENERS; i++)
        if (!listeners[i].h || listeners[i].port == port) {
            listeners[i].port = port;
            listeners[i].h = h;
            return 0;
        }
    return -1;
}

static int send_frame(const uint8_t *dst_mac, int port, uint32_t len)
{
    memcpy(txf, dst_mac, 6);
    if (wifi_link && port == NET_PORT_WIFI) {
        memcpy(txf + 6, wifi_mac, 6);
        int r = wifi_link->send(txf, len, ALL_PORT_MASK);
        if (r == 0) net_stats.tx++;
        return r;
    }
    memcpy(txf + 6, our_mac, 6);
    uint8_t mask = port < 0 || port > 7 ? ALL_PORT_MASK : (uint8_t)(1u << port);
    int r = link ? link->send(txf, len, mask) : eth_send(txf, len, mask);
    if (r == 0) net_stats.tx++;
    return r;
}

void net_arp_request(const uint8_t ip[4])
{
    int on_wifi = route_uses_wifi(ip);
    wr16(txf + 12, 0x0806);
    uint8_t *a = txf + 14;
    wr16(a, 1); wr16(a + 2, 0x0800); a[4] = 6; a[5] = 4; wr16(a + 6, 1);
    memcpy(a + 8, on_wifi ? wifi_mac : our_mac, 6);
    memcpy(a + 14, on_wifi ? wifi_ip : our_ip, 4);
    memset(a + 18, 0, 6);
    memcpy(a + 24, ip, 4);
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    send_frame(bcast, on_wifi ? NET_PORT_WIFI : -1, 42);
    net_stats.arp_req++;
}

int net_ip_send(const uint8_t dst_ip[4], uint8_t proto, const uint8_t *payload, uint32_t len)
{
    if (len > 1480) return -1;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int on_wifi = route_uses_wifi(dst_ip);
    const uint8_t *sip  = on_wifi ? wifi_ip   : active_ip;
    const uint8_t *mask = on_wifi ? wifi_mask : our_mask;
    const uint8_t *gw   = on_wifi ? wifi_gw   : our_gw;

    int bcast_dst = is_broadcast_ip(dst_ip);
    const uint8_t *nexthop = dst_ip;
    if (!bcast_dst && (sip[0] | sip[1] | sip[2] | sip[3]) &&
        (gw[0] | gw[1] | gw[2] | gw[3])) {
        if (((rd32(dst_ip) ^ rd32(sip)) & rd32(mask)) != 0)
            nexthop = gw;
    }
    struct arp_entry *e = bcast_dst ? 0 : cache_find_if(nexthop, on_wifi);
    if (!e && !bcast_dst) {
        int saved_override = tx_port_override;
        tx_port_override = on_wifi ? NET_PORT_WIFI : 0;
        net_arp_request(nexthop);
        tx_port_override = saved_override;
        for (int tries = 0; tries < 2000 && !e; tries++) {
            net_poll();
            e = cache_find_if(nexthop, on_wifi);
        }
    }
    const uint8_t *dmac = e ? e->mac : bcast;
    int port = on_wifi ? NET_PORT_WIFI : (e ? e->port : -1);
    static uint16_t ip_id;

    wr16(txf + 12, 0x0800);
    uint8_t *ip = txf + 14;
    uint32_t total = 20 + len;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint16_t)total);
    wr16(ip + 4, ip_id++); wr16(ip + 6, 0x4000);   /* DF */
    ip[8] = 64; ip[9] = proto;
    wr16(ip + 10, 0);
    memcpy(ip + 12, sip, 4);
    memcpy(ip + 16, dst_ip, 4);
    wr16(ip + 10, net_checksum(ip, 20, 0));
    memcpy(ip + 20, payload, len);
    int r = send_frame(dmac, port, 14 + total);
    return r;
}

int net_udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port, const void *data, uint32_t len)
{
    static uint8_t seg[1480];
    if (len > 1472) return -1;
    int on_wifi = route_uses_wifi(dst_ip);
    const uint8_t *sip = on_wifi ? wifi_ip : active_ip;
    wr16(seg, src_port); wr16(seg + 2, dst_port);
    wr16(seg + 4, (uint16_t)(8 + len));
    wr16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    uint16_t c = net_l4_checksum(sip, dst_ip, 17, seg, 8 + len);
    wr16(seg + 6, c ? c : 0xFFFF);
    int r = net_ip_send(dst_ip, 17, seg, 8 + len);
    if (r == 0) net_stats.udp_tx++;
    return r;
}

static void handle_arp(const uint8_t *f, uint32_t len, int port)
{
    const uint8_t *a = f + 14;
    if (len < 42 || rd16(a) != 1 || rd16(a + 2) != 0x0800) return;
    uint16_t opc = rd16(a + 6);
    if (!is_our_unicast(a + 14))                    /* learn anybody who talks about themselves */
        cache_learn(a + 14, a + 8, port);
    if (opc == 2) {
        if (is_our_unicast(a + 24)) net_stats.arp_reply_rcvd++;
        return;
    }
    int for_this_if = (wifi_link && port == NET_PORT_WIFI) ? is_wifi_unicast(a + 24) : is_eth_unicast(a + 24);
    if (opc != 1 || !for_this_if) return;
    wr16(txf + 12, 0x0806);
    uint8_t *r = txf + 14;
    wr16(r, 1); wr16(r + 2, 0x0800); r[4] = 6; r[5] = 4; wr16(r + 6, 2);
    memcpy(r + 8, (wifi_link && port == NET_PORT_WIFI) ? wifi_mac : our_mac, 6);
    memcpy(r + 14, a + 24, 4);                      /* answer with whichever IP was queried */
    memcpy(r + 18, a + 8, 10);
    if (send_frame(f + 6, port, 42) == 0) net_stats.arp_reply_sent++;
}

static void handle_icmp(const uint8_t *f, const uint8_t *ip, uint32_t ihl, uint32_t total, int port)
{
    const uint8_t *icmp = ip + ihl;
    if (icmp[0] != 8) return;
    wr16(txf + 12, 0x0800);
    uint8_t *oip = txf + 14;
    memcpy(oip, ip, total);
    memcpy(oip + 12, ip + 16, 4);                   /* reply from the queried IP */
    memcpy(oip + 16, ip + 12, 4);
    oip[8] = 64;
    wr16(oip + 10, 0);
    wr16(oip + 10, net_checksum(oip, ihl, 0));
    uint8_t *oicmp = oip + ihl;
    oicmp[0] = 0;
    wr16(oicmp + 2, 0);
    wr16(oicmp + 2, net_checksum(oicmp, total - ihl, 0));
    if (send_frame(f + 6, port, 14 + total) == 0) net_stats.icmp_echo++;
}

static void handle_udp(const uint8_t *ip, uint32_t ihl, uint32_t total)
{
    const uint8_t *udp = ip + ihl;
    if (total < ihl + 8) return;
    uint16_t sport = rd16(udp), dport = rd16(udp + 2), ulen = rd16(udp + 4);
    if (ulen < 8 || ulen > total - ihl) return;
    net_stats.udp_rx++;
    for (int i = 0; i < NET_MAX_UDP_LISTENERS; i++)
        if (listeners[i].h && listeners[i].port == dport) {
            listeners[i].h(ip + 12, sport, dport, udp + 8, ulen - 8);
            return;
        }
    net_stats.udp_nolistener++;
}

static void handle_ip(const uint8_t *f, uint32_t len, int port)
{
    const uint8_t *ip = f + 14;
    if (len < 34 || (ip[0] >> 4) != 4) return;
    uint32_t ihl = (ip[0] & 0xF) * 4;
    uint32_t total = rd16(ip + 2);
    if (ihl < 20 || total < ihl || total + 14 > len) return;
    int for_us = is_our_unicast(ip + 16) || is_broadcast_ip(ip + 16);
    /* Also allow unicast DHCP replies on UDP port 68 before our_ip/wifi_ip is updated */
    if (!for_us && ip[9] == 17 && total >= ihl + 8 && rd16(ip + ihl + 2) == 68)
        for_us = 1;
    if (!for_us) return;
    if (is_our_unicast(ip + 16))
        memcpy(active_ip, ip + 16, 4);
    else if (wifi_link && port == NET_PORT_WIFI)
        memcpy(active_ip, wifi_ip, 4);
    else
        memcpy(active_ip, our_ip, 4);
    cache_learn(ip + 12, f + 6, port);
    if (ip[9] == 1) handle_icmp(f, ip, ihl, total, port);
    else if (ip[9] == 17) handle_udp(ip, ihl, total);
    else if (ip[9] == 6) tcp_input(ip + 12, ip + ihl, total - ihl);
    memcpy(active_ip, our_ip, 4);
}

void net_poll(void)
{
    uint8_t *f; uint32_t len; int port;
    while (link ? link->recv(&f, &len, &port) : eth_recv(&f, &len, &port)) {
        cur_rx_port = port;
        net_stats.rx++;
        if (len >= 14) {
            uint16_t type = rd16(f + 12);
            if (type == 0x0806) handle_arp(f, len, port);
            else if (type == 0x0800) handle_ip(f, len, port);
            else net_stats.dropped++;
        }
        if (link) link->recv_done(); else eth_recv_done();
    }
    if (wifi_link) {
        while (wifi_link->recv(&f, &len, &port)) {
            cur_rx_port = NET_PORT_WIFI;
            net_stats.rx++;
            if (len >= 14) {
                uint16_t type = rd16(f + 12);
                if (type == 0x0806) handle_arp(f, len, NET_PORT_WIFI);
                else if (type == 0x0800) handle_ip(f, len, NET_PORT_WIFI);
                else net_stats.dropped++;
            }
            wifi_link->recv_done();
        }
    }
    cur_rx_port = -1;
    tcp_poll();
    if (link) { if (link->reclaim) link->reclaim(); } else eth_tx_reclaim();
    if (wifi_link && wifi_link->reclaim) wifi_link->reclaim();
}
