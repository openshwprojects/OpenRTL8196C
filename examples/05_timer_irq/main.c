/*
 * 05_timer_irq - the same periodic timer, but delivered as an interrupt.
 *
 * The handler only increments counters; main prints them so the UART never
 * runs inside the interrupt.  Cause is reported so we learn which CPU line
 * the routing value selected.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "irq.h"

#define DIVISOR   200u
#define RELOAD    1000000u
#define ROUTE     IRQ_RS_IP2

static volatile uint32_t ticks;

static void on_tc0(int irq, void *arg)
{
    (void)irq; (void)arg;
    timer_ack(0);
    ticks++;
}

int main(void)
{
    uart_init();
    printf("\n\ntimer irq: divisor %u reload %u route %u\n", DIVISOR, RELOAD, ROUTE);

    irq_init();
    irq_attach(IRQ_TC0, ROUTE, on_tc0, 0);
    irq_enable(IRQ_TC0);

    timer_set_divisor(DIVISOR);
    timer_start(0, RELOAD, 1, 1);
    irq_global_enable();
    printf("Status now %08x, waiting for ticks\n", cp0_status_read());

    uint32_t seen = 0;
    for (;;) {
        if (ticks != seen) {
            seen = ticks;
            printf("tick %u  irqs=%u cause=%08x gisr=%08x\n",
                   seen, irq_count_total, irq_last_cause, REG32(INTC_GISR));
        }
    }
}
