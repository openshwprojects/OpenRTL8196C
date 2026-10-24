#ifndef _SHIM_LWIP_INET_CHKSUM_H
#define _SHIM_LWIP_INET_CHKSUM_H

#include "err.h"

u16_t inet_chksum(const void *dataptr, u16_t len);

#endif
