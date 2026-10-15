/*
 * 88_timer_glitch - TC1 counter reads and the SDK waits.
 *
 * Part 1: raw reads with different pauses (histogram of read-to-read
 * differences).  Measured: back-to-back reads flip the lowest count bit
 * (+1, -1, ...); with >= 200 nops between reads they are clean.
 * Part 2: calibrate delay_ms / delay_us / elapsed_us / every_ms against
 * the PC clock (run with flash.py -t; each step should take 5 s).
 * Run it both uncached (default) and cached (--load 0x80500000).
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"

static void pause(uint32_t n)
{
    while (n--) __asm__ volatile("nop");
}

static void run(uint32_t gap, uint32_t reads)
{
    uint32_t hist[8] = { 0 };   /* 0, 1, 2-3, 4-15, 16-255, 256-4095, >=4096, backwards */
    uint32_t first = REG32(TC1CNT) >> TC_DATA_SHIFT, prev = first;
    for (uint32_t i = 0; i < reads; i++) {
        pause(gap);
        uint32_t v = REG32(TC1CNT) >> TC_DATA_SHIFT;
        uint32_t d = (v - prev) & TC_DATA_MAX;
        int b = d == 0 ? 0 : d == 1 ? 1 : d < 4 ? 2 : d < 16 ? 3 : d < 256 ? 4 : d < 4096 ? 5 :
                d < TC_DATA_MAX / 2 ? 6 : 7;
        hist[b]++;
        prev = v;
    }
    printf("raw reads, gap %4u nops: 0:%u 1:%u 2-3:%u 4-15:%u 16-255:%u 256-4095:%u >=4096:%u back:%u\n",
           gap, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
}

static void mark(const char *what)
{
    printf("MARK %-28s micros=%u\n", what, micros());
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\ntimer probe, running at %08x (%s)\n", (unsigned)&main,
           ((unsigned)&main >> 29) == 5 ? "uncached" : "cached");
    run(0, 1000000);
    run(200, 50000);

    mark("start");
    delay_ms(5000);
    mark("after delay_ms(5000)");
    for (int i = 0; i < 5000; i++) delay_us(1000);
    mark("after 5000 x delay_us(1000)");
    uint32_t t0 = micros();
    while (elapsed_us(t0) < 5000000u) ;
    mark("after elapsed_us 5 s");
    uint32_t last = micros();
    while (!every_ms(&last, 5000)) ;
    mark("after every_ms 5 s");
    printf("done\n");
    return 0;
}
