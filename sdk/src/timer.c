/*
 * timer.c - TC0/TC1 timers and watchdog
 */
#include "rtl8196c.h"
#include "timer.h"

void timer_set_divisor(uint32_t div)
{
    REG32(CDBR) = (div & 0xFFFFu) << CDBR_DIV_SHIFT;
}

void timer_start(int tc, uint32_t reload, int periodic, int irq)
{
    uint32_t en   = tc ? TCCNR_TC1EN : TCCNR_TC0EN;
    uint32_t mode = tc ? TCCNR_TC1_TIMER : TCCNR_TC0_TIMER;
    uint32_t ie   = tc ? TCIR_TC1IE : TCIR_TC0IE;
    uint32_t ip   = tc ? TCIR_TC1IP : TCIR_TC0IP;

    REG32(TCCNR) &= ~(en | mode);
    REG32(tc ? TC1DATA : TC0DATA) = (reload & TC_DATA_MAX) << TC_DATA_SHIFT;
    REG32(TCIR) = (REG32(TCIR) & ~ie) | ip;              /* clear stale pending */
    if (irq)
        REG32(TCIR) |= ie;
    REG32(TCCNR) |= en | (periodic ? mode : 0);
}

void timer_stop(int tc)
{
    REG32(TCCNR) &= ~(tc ? (TCCNR_TC1EN | TCCNR_TC1_TIMER) : (TCCNR_TC0EN | TCCNR_TC0_TIMER));
    REG32(TCIR)  &= ~(tc ? TCIR_TC1IE : TCIR_TC0IE);
}

uint32_t timer_count(int tc)
{
    return REG32(tc ? TC1CNT : TC0CNT) >> TC_DATA_SHIFT;
}

int timer_pending(int tc)
{
    return (REG32(TCIR) & (tc ? TCIR_TC1IP : TCIR_TC0IP)) != 0;
}

void timer_ack(int tc)
{
    REG32(TCIR) |= tc ? TCIR_TC1IP : TCIR_TC0IP;         /* write 1 to clear */
}

void timer_delay_init(void)
{
    timer_set_divisor(BUS_CLOCK_HZ / 1000000u);
    timer_start(1, TC_DATA_MAX, 1, 0);
}

/*
 * TC1 read quirk (probes/88_timer_glitch, measured cached and uncached):
 * reads taken close together see the lowest count bit flip, so consecutive
 * reads go +1, -1, +1, ... while the long-run rate is exact.  Any
 * "now - start" arithmetic then sees a -1 as a ~268 s jump.  micros()
 * therefore never goes backwards: a value slightly behind the last one is
 * a read glitch and is ignored (costs at most 1 us of precision).  Large
 * backward differences are real wraps and are accepted.
 */
static uint32_t mono_last;

uint32_t micros(void)
{
    uint32_t v = timer_count(1);
    uint32_t back = (mono_last - v) & TC_DATA_MAX;
    if (back != 0 && back < 64u)
        return mono_last;
    mono_last = v;
    return v;
}

uint32_t elapsed_us(uint32_t since)
{
    return (micros() - since) & TC_DATA_MAX;
}

static uint32_t ms_accum;
static uint32_t ms_last_us;
static int      ms_inited;

uint32_t timer_ms(void)
{
    uint32_t now = micros();
    if (!ms_inited) {
        ms_last_us = now;
        ms_inited = 1;
        return ms_accum;
    }
    uint32_t diff = (now - ms_last_us) & TC_DATA_MAX;
    if (diff >= 1000u) {
        uint32_t add = diff / 1000u;
        ms_accum += add;
        ms_last_us = (ms_last_us + add * 1000u) & TC_DATA_MAX;
    }
    return ms_accum;
}

int every_ms(uint32_t *last, uint32_t period_ms)
{
    if (elapsed_us(*last) < period_ms * 1000u)
        return 0;
    *last = micros();
    return 1;
}

/* safe because micros() is monotonic (see above) */
void delay_us(uint32_t us)
{
    uint32_t start = micros();
    while (((micros() - start) & TC_DATA_MAX) < us)
        ;
}

void delay_ms(uint32_t ms)
{
    while (ms) {
        uint32_t chunk = ms > 1000u ? 1000u : ms;
        delay_us(chunk * 1000u);
        ms -= chunk;
    }
}

void delay_loop(uint32_t iterations)
{
    while (iterations--)
        __asm__ volatile("nop");
}

void wdt_disable(void)
{
    REG32(WDTCNR) = WDT_STOP;
}

void wdt_reboot(void)
{
    REG32(WDTCNR) = WDT_BITE_NOW;
    for (;;)
        ;
}
