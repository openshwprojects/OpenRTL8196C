/*
 * 87_cache_policy - is the D-cache write-through, and does CCTL invalidate?
 * Build cached:  python tools/build.py probes/87_cache_policy --load 0x80500000 --stack 0x80600000
 *
 * 1. load a word into the cache (cached read), store a new value through
 *    the cached address, read the uncached alias: equal = write-through.
 * 2. change memory through the uncached alias, read cached: stale value
 *    expected; then CCTL D-invalidate and read again: new value expected.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "irq.h"

static volatile uint32_t word[64] __attribute__((aligned(32)));

int main(void)
{
    uart_init();
    timer_delay_init();
    volatile uint32_t *c = &word[8];
    volatile uint32_t *u = (volatile uint32_t *)(((uint32_t)c & 0x1FFFFFFFu) | 0xA0000000u);
    printf("\n\ncache policy probe, code at %08x, word cached %08x uncached %08x\n", (unsigned)&main,
           (unsigned)c, (unsigned)u);

    *u = 0x11111111u;
    (void)*c;                                   /* line now in the D-cache */
    *c = 0x22222222u;                           /* store through the cached address */
    uint32_t seen = *u;
    printf("1. after cached store: memory holds %08x -> %s\n", seen,
           seen == 0x22222222u ? "WRITE-THROUGH" : "write-back (memory not updated)");

    (void)*c;                                   /* make sure it is cached */
    *u = 0x33333333u;                           /* memory changes behind the cache */
    uint32_t stale = *c;
    cctl_write(CCTL_DINVAL);
    uint32_t fresh = *c;
    printf("2. memory changed behind the cache: cached read %08x (%s), after CCTL DINVAL %08x (%s)\n", stale,
           stale == 0x33333333u ? "not cached?" : "stale, as expected", fresh,
           fresh == 0x33333333u ? "invalidate works" : "invalidate did NOT work");
    /* 3. does an invalidate lose recent cached stores? */
    volatile uint32_t *c3 = &word[16], *u3 = (volatile uint32_t *)(((uint32_t)c3 & 0x1FFFFFFFu) | 0xA0000000u);
    *u3 = 0x44444444u;
    (void)*c3;
    *c3 = 0x55555555u;
    uint32_t mem_before = *u3;
    cctl_write(CCTL_DINVAL);
    uint32_t after_c = *c3, after_u = *u3;
    printf("3. cached store 55555555, memory before invalidate %08x, after invalidate: cached %08x memory %08x -> %s\n",
           mem_before, after_c, after_u, after_c == 0x55555555u ? "store kept" : "STORE LOST");

    /* 4. same with 64 stores spread over 8 KB (more than one cache way) */
    static volatile uint32_t spread[2048] __attribute__((aligned(32)));
    for (int i = 0; i < 2048; i++) spread[i] = 0;
    for (int i = 0; i < 2048; i++) (void)spread[i];
    for (int i = 0; i < 2048; i += 32) spread[i] = 0xA5000000u | i;
    cctl_write(CCTL_DINVAL);
    int lost = 0;
    for (int i = 0; i < 2048; i += 32) if (spread[i] != (0xA5000000u | i)) lost++;
    volatile uint32_t *su = (volatile uint32_t *)(((uint32_t)spread & 0x1FFFFFFFu) | 0xA0000000u);
    printf("4. 64 cached stores, then invalidate: %d lost (spread[32]: cached %08x memory %08x)\n", lost,
           spread[32], su[32]);

    /* 5. which stores reach memory, and when? */
    for (int variant = 0; variant < 5; variant++) {
        for (int i = 0; i < 2048; i++) spread[i] = 0;
        for (int i = 0; i < 2048; i++) (void)spread[i];
        for (int i = 0; i < 2048; i += 32) spread[i] = 0xB6000000u | i;
        if (variant == 1) { uint32_t t = micros(); while (((micros() - t) & 0xFFFFFFu) < 10000u) ; }
        if (variant == 2) (void)*(volatile uint32_t *)0xA0000000u;   /* one unrelated uncached read */
        if (variant == 3) cctl_write(0x100);                         /* RLX: D-cache write back */
        if (variant == 4) cctl_write(0x200);                         /* RLX: write back + invalidate */
        else cctl_write(CCTL_DINVAL);
        lost = 0;
        int mem = 0;
        for (int i = 0; i < 2048; i += 32) if (spread[i] != (0xB6000000u | i)) lost++;
        for (int i = 0; i < 2048; i += 32) mem += su[i] == (0xB6000000u | i);
        static const char *const vn[] = { "no wait", "10 ms busy wait", "uncached read", "CCTL 0x100",
                                          "CCTL 0x200 only" };
        printf("5.%d %-16s then invalidate: %d of 64 stores lost (uncached view %d)\n", variant, vn[variant], lost,
               mem);
    }
    /* 6. zeroing loop alone: are the zeros from the first loop in memory? */
    for (int i = 0; i < 2048; i++) su[i] = 0x77777777u;
    for (int i = 0; i < 2048; i++) spread[i] = 0;
    int z = 0;
    for (int i = 0; i < 2048; i++) z += su[i] == 0;
    printf("6. 2048 cached stores of 0 over uncached 77777777: %d of 2048 reached memory\n", z);
    printf("done\n");
    return 0;
}
