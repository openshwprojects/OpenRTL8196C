#ifndef _SHIM_LWIP_TIMEOUTS_H
#define _SHIM_LWIP_TIMEOUTS_H

#include "err.h"

typedef void (*sys_timeout_handler)(void *arg);

void sys_timeout(u32_t msecs, sys_timeout_handler handler, void *arg);
void sys_untimeout(sys_timeout_handler handler, void *arg);

#endif
