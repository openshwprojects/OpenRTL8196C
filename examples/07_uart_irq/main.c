/*
 * 07_uart_irq - UART receive by interrupt.  The handler drains the FIFO into
 * a ring buffer; main echoes from the ring and prints a heartbeat from TC0.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "irq.h"

#define RING 256
static volatile uint8_t ring[RING];
static volatile unsigned head, tail;
static volatile uint32_t ticks, uart_irqs;

static void on_uart(int irq, void *arg)
{
    (void)irq; (void)arg;
    uart_irqs++;
    while (REG8(UART_LSR) & UART_LSR_DR) {
        uint8_t c = REG8(UART_RBR);
        unsigned next = (head + 1) % RING;
        if (next != tail) {
            ring[head] = c;
            head = next;
        }
    }
    (void)REG8(UART_IIR);
}

static void on_tc0(int irq, void *arg)
{
    (void)irq; (void)arg;
    timer_ack(0);
    ticks++;
}

int main(void)
{
    uart_init();
    printf("\n\nuart irq echo\n");

    irq_init();
    irq_attach(IRQ_UART0, IRQ_RS_IP2, on_uart, 0);
    irq_attach(IRQ_TC0, IRQ_RS_IP2, on_tc0, 0);
    irq_enable(IRQ_UART0);
    irq_enable(IRQ_TC0);

    timer_set_divisor(200);
    timer_start(0, 1000000, 1, 1);

    REG8(UART_IER) = UART_IER_RDA;          /* receive data available */
    irq_global_enable();

    uint32_t seen = 0;
    for (;;) {
        while (tail != head) {
            uint8_t c = ring[tail];
            tail = (tail + 1) % RING;
            uart_putc((char)c);
        }
        if (ticks != seen) {
            seen = ticks;
            printf("[tick %u uart_irqs=%u]\n", seen, uart_irqs);
        }
    }
}
