/*
 * net.h - a very small IPv4 stack on top of the switch CPU port:
 * ARP (reply + cache), ICMP echo, UDP in/out.  Polled, no buffers kept.
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>

#define NET_MAX_UDP_LISTENERS 16
#define NET_ARP_CACHE 16

typedef void (*udp_handler_t)(const uint8_t src_ip[4], uint16_t src_port, uint16_t dst_port,
                              const uint8_t *data, uint32_t len);

/* Link layer.  By default net.c drives the Ethernet switch (swcore.c).  A
 * different interface (Wi-Fi) plugs in by providing these four calls before
 * net_init; frames are Ethernet-shaped (dst, src, type, payload). */
struct net_link {
    int  (*recv)(uint8_t **frame, uint32_t *len, int *port);   /* 1 = frame available */
    void (*recv_done)(void);
    int  (*send)(const void *frame, uint32_t len, uint8_t portmask);
    void (*reclaim)(void);                                     /* may be NULL */
};
void net_set_link(const struct net_link *link);   /* call before net_init; NULL = Ethernet */

/* Dual-interface mode: keep Ethernet on switch ports 0..4 and attach Wi-Fi on NET_PORT_WIFI */
#define NET_PORT_WIFI 7
void net_set_wifi_link(const struct net_link *wlink, const uint8_t mac[6]);
const uint8_t *net_wifi_mac(void);
const uint8_t *net_wifi_ip(void);
const uint8_t *net_wifi_netmask(void);
const uint8_t *net_wifi_gateway(void);
const uint8_t *net_wifi_dns(void);
void net_set_wifi_ip(const uint8_t ip[4]);
void net_set_wifi_netmask(const uint8_t mask[4]);
void net_set_wifi_gateway(const uint8_t gw[4]);
void net_set_wifi_dns(const uint8_t dns[4]);
int  net_rx_port(void);                           /* port of frame currently in net_poll() */
void net_set_tx_port(int port);                   /* -1 = auto, 0 = eth, NET_PORT_WIFI = wifi */
const uint8_t *net_select_src_ip(const uint8_t dst_ip[4]);
const uint8_t *net_select_dns(void);

void net_init(const uint8_t mac[6], const uint8_t ip[4]);
void net_poll(void);                                   /* handle every pending frame */
int  net_udp_listen(uint16_t port, udp_handler_t h);  /* 0 ok, -1 full */
int  net_udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
                  const void *data, uint32_t len);     /* 0 ok, -1 no route/ring full */
void net_arp_request(const uint8_t ip[4]);             /* broadcast "who has" */
void net_igmp_join(const uint8_t group_ip[4]);         /* join IPv4 multicast group & send IGMPv2 report */
void net_igmp_report_all(void);                        /* send IGMPv2 reports for all joined groups */
typedef void (*icmp_reply_handler_t)(const uint8_t src_ip[4], uint16_t id, uint16_t seq, uint32_t len);
void net_icmp_set_reply_handler(icmp_reply_handler_t h);
int  net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t id, uint16_t seq, uint32_t payload_len);

const uint8_t *net_ip(void);
const uint8_t *net_active_ip(void);
void net_set_active_ip(const uint8_t ip[4]);
const uint8_t *net_mac(void);
const uint8_t *net_netmask(void);
const uint8_t *net_gateway(void);
const uint8_t *net_dns(void);
void net_set_ip(const uint8_t ip[4]);
void net_set_netmask(const uint8_t mask[4]);
void net_set_gateway(const uint8_t gw[4]);
void net_set_dns(const uint8_t dns[4]);

struct net_stats {
    uint32_t rx, tx, arp_req, arp_reply_sent, arp_reply_rcvd, icmp_echo, udp_rx, udp_tx, udp_nolistener, dropped;
};
extern struct net_stats net_stats;

/* raw IPv4 send (payload copied), used by UDP and TCP */
int  net_ip_send(const uint8_t dst_ip[4], uint8_t proto, const uint8_t *payload, uint32_t len);
/* checksum of a TCP/UDP segment including the pseudo header */
uint16_t net_l4_checksum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                         const uint8_t *seg, uint32_t len);
uint32_t rd32(const uint8_t *p);
void     wr32(uint8_t *p, uint32_t v);

/* helpers */
uint16_t net_checksum(const uint8_t *p, uint32_t len, uint32_t seed);
uint16_t rd16(const uint8_t *p);
void     wr16(uint8_t *p, uint16_t v);

#endif
