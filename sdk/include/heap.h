#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_REGION_BASE 0x80700000u
#define HEAP_REGION_SIZE (1536u * 1024u) /* 1.5 MB: 0x80700000..0x80880000 (cached KSEG0) */

void  heap_init(void);
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);

void  *pvPortMalloc(size_t size);
void   vPortFree(void *ptr);
void  *pvPortRealloc(void *ptr, size_t size);
size_t xPortGetFreeHeapSize(void);

#endif
