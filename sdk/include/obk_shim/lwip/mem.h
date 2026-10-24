#ifndef _SHIM_LWIP_MEM_H
#define _SHIM_LWIP_MEM_H

#include "err.h"
#include <stddef.h>

#ifndef LWIP_DBG_ON
#define LWIP_DBG_ON 0x80U
#endif

#ifndef LWIP_ASSERT
#define LWIP_ASSERT(message, assertion) do { (void)(message); (void)(assertion); } while (0)
#endif

#endif
