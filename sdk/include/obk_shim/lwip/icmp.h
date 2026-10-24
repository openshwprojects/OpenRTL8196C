#ifndef _SHIM_LWIP_ICMP_H
#define _SHIM_LWIP_ICMP_H

#include "err.h"

#define ICMP_ER   0  /* echo reply */
#define ICMP_ECHO 8  /* echo */

struct icmp_echo_hdr {
    u8_t  type;
    u8_t  code;
    u16_t chksum;
    u16_t id;
    u16_t seqno;
};

#define ICMPH_TYPE(hdr)          ((hdr)->type)
#define ICMPH_CODE(hdr)          ((hdr)->code)
#define ICMPH_TYPE_SET(hdr, t)   ((hdr)->type = (t))
#define ICMPH_CODE_SET(hdr, c)   ((hdr)->code = (c))

#endif
