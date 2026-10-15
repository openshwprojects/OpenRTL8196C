/*
 * dhcp.c - non-blocking DHCPv4 client (DISCOVER -> OFFER -> REQUEST -> ACK)
 */
#include "dhcp.h"
#include "net.h"
#include "timer.h"
#include "libc.h"

static struct dhcp_lease lease;
static char host_name[24] = "OpenRTL8196C";
static uint8_t offered_ip[4], server_id[4];
static uint32_t t_last_tx;

static void send_dhcp(uint8_t msg_type)
{
    uint8_t pkt[300];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 1;                /* BOOTREQUEST */
    pkt[1] = 1;                /* Ethernet */
    pkt[2] = 6;                /* MAC len */
    wr32(pkt + 4, lease.xid);
    wr16(pkt + 10, 0x8000);    /* broadcast flag */
    memcpy(pkt + 28, net_mac(), 6);

    /* magic cookie 99.130.83.99 */
    pkt[236] = 99; pkt[237] = 130; pkt[238] = 83; pkt[239] = 99;
    uint8_t *o = pkt + 240;

    /* option 53: DHCP message type */
    *o++ = 53; *o++ = 1; *o++ = msg_type;

    /* option 61: client identifier */
    *o++ = 61; *o++ = 7; *o++ = 1;
    memcpy(o, net_mac(), 6); o += 6;

    /* option 12: hostname */
    uint8_t hlen = (uint8_t)strlen(host_name);
    if (hlen) {
        *o++ = 12; *o++ = hlen;
        memcpy(o, host_name, hlen); o += hlen;
    }

    if (msg_type == 3) {       /* DHCPREQUEST */
        *o++ = 50; *o++ = 4; memcpy(o, offered_ip, 4); o += 4;
        if (server_id[0] | server_id[1] | server_id[2] | server_id[3]) {
            *o++ = 54; *o++ = 4; memcpy(o, server_id, 4); o += 4;
        }
    }

    /* option 55: parameter request list (subnet mask, router, DNS, lease time) */
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;                /* end */

    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    net_udp_send(bcast, 68, 67, pkt, (uint32_t)(o - pkt));
    t_last_tx = micros();
}

static void on_dhcp_udp(const uint8_t src_ip[4], uint16_t sport, uint16_t dport,
                        const uint8_t *d, uint32_t len)
{
    (void)sport; (void)dport;
    if (len < 240 || d[0] != 2 || rd32(d + 4) != lease.xid)
        return;
    if (memcmp(d + 28, net_mac(), 6) != 0)
        return;
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
        return;

    uint8_t msg_type = 0;
    uint8_t mask[4] = { 255, 255, 255, 0 };
    uint8_t gw[4]   = { 0, 0, 0, 0 };
    uint8_t dns[4]  = { 0, 0, 0, 0 };
    uint8_t srv[4]  = { 0, 0, 0, 0 };
    uint32_t ltime  = 3600;

    uint32_t i = 240;
    while (i < len) {
        uint8_t opt = d[i++];
        if (opt == 0) continue;
        if (opt == 255 || i >= len) break;
        uint8_t olen = d[i++];
        if (i + olen > len) break;
        const uint8_t *v = d + i;
        if (opt == 53 && olen >= 1) msg_type = v[0];
        else if (opt == 1 && olen >= 4) memcpy(mask, v, 4);
        else if (opt == 3 && olen >= 4) memcpy(gw, v, 4);
        else if (opt == 6 && olen >= 4) memcpy(dns, v, 4);
        else if (opt == 54 && olen >= 4) memcpy(srv, v, 4);
        else if (opt == 51 && olen >= 4) ltime = rd32(v);
        i += olen;
    }

    if (!(srv[0] | srv[1] | srv[2] | srv[3]))
        memcpy(srv, src_ip, 4);

    if (msg_type == 2 && lease.state == DHCP_DISCOVERING) {   /* DHCPOFFER */
        memcpy(offered_ip, d + 16, 4);
        memcpy(server_id, srv, 4);
        lease.state = DHCP_REQUESTING;
        send_dhcp(3);
    } else if (msg_type == 5 && (lease.state == DHCP_REQUESTING || lease.state == DHCP_DISCOVERING)) { /* DHCPACK */
        memcpy(lease.ip, d + 16, 4);
        memcpy(lease.mask, mask, 4);
        memcpy(lease.gw, gw, 4);
        memcpy(lease.dns, (dns[0] | dns[1] | dns[2] | dns[3]) ? dns : gw, 4);
        memcpy(lease.server, srv, 4);
        lease.lease_seconds = ltime;
        lease.state = DHCP_BOUND;
        net_set_ip(lease.ip);
        net_set_netmask(lease.mask);
        if (gw[0] | gw[1] | gw[2] | gw[3]) net_set_gateway(lease.gw);
        if (lease.dns[0] | lease.dns[1] | lease.dns[2] | lease.dns[3]) net_set_dns(lease.dns);
    }
}

void dhcp_start(const char *hostname)
{
    if (hostname && hostname[0]) {
        strncpy(host_name, hostname, sizeof(host_name) - 1);
        host_name[sizeof(host_name) - 1] = 0;
    }
    const uint8_t *m = net_mac();
    lease.xid = 0x8196C000u ^ ((uint32_t)m[4] << 8) ^ m[5] ^ micros();
    lease.state = DHCP_DISCOVERING;
    net_udp_listen(68, on_dhcp_udp);
    send_dhcp(1);
}

void dhcp_poll(void)
{
    if (lease.state == DHCP_DISCOVERING || lease.state == DHCP_REQUESTING) {
        if (elapsed_us(t_last_tx) > 2500000u) {
            lease.state = DHCP_DISCOVERING;
            send_dhcp(1);
        }
    }
}

int dhcp_bound(void)
{
    return lease.state == DHCP_BOUND;
}

const struct dhcp_lease *dhcp_info(void)
{
    return &lease;
}
