/*
 * 93_route_probe - which IRR nibble and which GIMR bit belong to TC0, TC1, UART0?
 *
 * Routing value N in a nibble sends that source to CPU line IPN (measured),
 * so: route one nibble at a time to 2, trigger the source, look for
 * Cause.IP2.  Some sources are pending permanently (baseline); they are
 * measured first and ignored.  Then, with the found nibble routed, walk
 * GIMR one bit at a time to find the mask bit.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "irq.h"
#include "timer.h"
#include "gpio.h"

#define IP2 (1u << 10)

static void irr_clear(void)
{
    REG32(INTC_IRR0) = 0; REG32(INTC_IRR1) = 0; REG32(INTC_IRR2) = 0; REG32(INTC_IRR3) = 0;
}

static void irr_set_nibble(int n, uint32_t v)
{
    uint32_t reg = INTC_IRR0 + 4 * (n / 8);
    int sh = 4 * (n % 8);
    REG32(reg) = (REG32(reg) & ~(0xFu << sh)) | (v << sh);
}

static void settle(void)
{
    delay_loop(2000);
}

/* returns bitmask of sources whose nibble raised IP2 while trigger() keeps the source pending */
typedef void (*trigger_fn)(void);

static uint32_t sweep_nibbles(trigger_fn trigger)
{
    uint32_t hits = 0;
    for (int n = 0; n < 32; n++) {
        irr_clear();
        irr_set_nibble(n, 2);
        if (trigger) trigger();
        settle();
        if (cp0_cause_read() & IP2)
            hits |= 1u << n;
    }
    irr_clear();
    return hits;
}

static uint32_t sweep_gimr(int nibble, trigger_fn trigger)
{
    uint32_t hits = 0;
    irr_clear();
    irr_set_nibble(nibble, 2);
    for (int b = 0; b < 32; b++) {
        REG32(INTC_GIMR) = 1u << b;
        if (trigger) trigger();
        settle();
        if (cp0_cause_read() & IP2)
            hits |= 1u << b;
    }
    REG32(INTC_GIMR) = 0xFFFFFFFFu;
    irr_clear();
    return hits;
}

static int first_bit(uint32_t m)
{
    for (int i = 0; i < 32; i++)
        if (m & (1u << i)) return i;
    return -1;
}

static void trig_tc0(void)
{
    timer_ack(0);
    for (uint32_t i = 0; i < 50000000; i++)
        if (REG32(TCIR) & TCIR_TC0IP) return;
}

static void trig_tc1(void)
{
    timer_ack(1);
    for (uint32_t i = 0; i < 50000000; i++)
        if (REG32(TCIR) & TCIR_TC1IP) return;
}

static void trig_uart(void)
{
    REG8(UART_IER) = 0;
    while (!(REG8(UART_LSR) & UART_LSR_TEMT))
        ;
    REG8(UART_IER) = UART_IER_THRE;   /* transmitter idle -> pending immediately */
}

static void report(const char *name, uint32_t baseline, uint32_t nibbles, trigger_fn trigger)
{
    uint32_t own = nibbles & ~baseline;
    int src = first_bit(own);
    printf("%s: nibble hits %08x, minus baseline -> %08x => source %d", name, nibbles, own, src);
    if (src >= 0) {
        uint32_t g = sweep_gimr(src, trigger);
        printf(", GIMR bit %d (hits %08x), GISR now %08x", first_bit(g), g, REG32(INTC_GISR));
    }
    printf("\n");
}

int main(void)
{
    uart_init();
    printf("\n\nroute probe v2\n");
    irq_init();
    REG32(INTC_GIMR) = 0xFFFFFFFFu;
    timer_stop(0); timer_ack(0);
    timer_stop(1); timer_ack(1);
    REG8(UART_IER) = 0;
    settle();

    uint32_t baseline = sweep_nibbles(0);
    printf("baseline (always pending sources): %08x  GISR=%08x\n", baseline, REG32(INTC_GISR));

    timer_set_divisor(200);
    timer_start(0, 20000, 1, 1);                 /* 20 ms, IE on */
    report("TC0  ", baseline, sweep_nibbles(trig_tc0), trig_tc0);
    timer_stop(0); timer_ack(0);

    timer_start(1, 20000, 1, 1);
    report("TC1  ", baseline, sweep_nibbles(trig_tc1), trig_tc1);
    timer_stop(1); timer_ack(1);

    trig_uart();
    report("UART0", baseline, sweep_nibbles(trig_uart), trig_uart);
    REG8(UART_IER) = 0;

    REG32(INTC_GIMR) = 0;
    irr_clear();
    printf("done\n");
    return 0;
}
