#ifndef _SHIM_LWIP_RAW_H
#define _SHIM_LWIP_RAW_H

#include "err.h"
#include "ip_addr.h"

#ifndef IP_PROTO_ICMP
#define IP_PROTO_ICMP 1
#endif

#ifndef IP_ADDR_ANY
#define IP_ADDR_ANY ((const ip_addr_t *)0)
#endif

#define PBUF_TRANSPORT 0
#define PBUF_IP        1
#define PBUF_LINK      2
#define PBUF_RAW       3

#define PBUF_RAM       0
#define PBUF_ROM       1
#define PBUF_REF       2
#define PBUF_POOL      3

#define PBUF_IP_HLEN   20

struct pbuf {
    struct pbuf *next;
    void        *payload;
    u16_t        tot_len;
    u16_t        len;
    u8_t         type;
    u8_t         flags;
    u16_t        ref;
};

struct pbuf *pbuf_alloc(u8_t layer, u16_t length, u8_t type);
u8_t         pbuf_header(struct pbuf *p, s16_t header_size_increment);
u8_t         pbuf_free(struct pbuf *p);

struct raw_pcb;

typedef u8_t (*raw_recv_fn)(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr);

struct raw_pcb {
    u8_t        protocol;
    raw_recv_fn recv;
    void       *recv_arg;
};

struct raw_pcb *raw_new(u8_t proto);
void            raw_recv(struct raw_pcb *pcb, raw_recv_fn recv, void *recv_arg);
err_t           raw_bind(struct raw_pcb *pcb, const ip_addr_t *ipaddr);
err_t           raw_sendto(struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *ipaddr);
void            raw_remove(struct raw_pcb *pcb);

#endif
