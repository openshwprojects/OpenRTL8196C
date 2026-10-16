/*
 * uart.c - polled 16550 driver for UART0
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"                   /* wdt_reboot, for uart_esc_reboot_poll */

void uart_init(void)
{
    /* let the loader's "Jump to address" text drain before touching the FIFOs,
     * otherwise its tail comes out garbled */
    while (!(REG8(UART_LSR) & UART_LSR_TEMT))
        ;
    /* enable + clear FIFOs, leave line settings (38400 8N1) as the loader set them */
    REG8(UART_FCR) = UART_FCR_EN | UART_FCR_RXCLR | UART_FCR_TXCLR;
    REG8(UART_IER) = 0;
    (void)REG8(UART_LSR);
    (void)REG8(UART_RBR);
}

uint8_t uart_lsr(void)
{
    return REG8(UART_LSR);
}

int uart_tx_ready(void)
{
    return (REG8(UART_LSR) & UART_LSR_THRE) != 0;
}

int uart_rx_ready(void)
{
    return (REG8(UART_LSR) & UART_LSR_DR) != 0;
}

void uart_putc(char c)
{
    while (!(REG8(UART_LSR) & UART_LSR_THRE))
        ;
    REG8(UART_THR) = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_esc_reboot_poll(void)
{
    int c = uart_getc_nb();
    if (c == 0x1B) {
        uart_puts("\r\nESC on UART: rebooting to the loader\r\n");
        wdt_reboot();
    }
    return c;
}

int uart_getc_nb(void)
{
    if (REG8(UART_LSR) & UART_LSR_DR)
        return REG8(UART_RBR);
    return -1;
}

int uart_getc(void)
{
    int c;
    while ((c = uart_getc_nb()) < 0)
        ;
    return c;
}
