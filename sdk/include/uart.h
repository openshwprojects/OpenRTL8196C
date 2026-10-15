#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stdarg.h>

/* The boot loader leaves UART0 at 38400 8N1; uart_init() only sets up the
 * FIFO and clears pending interrupts, it does not touch the baud rate. */
void uart_init(void);

void uart_putc(char c);          /* blocking, waits for THRE */
void uart_puts(const char *s);   /* no newline translation */
int  uart_getc_nb(void);         /* -1 when no byte is waiting */
int  uart_getc(void);            /* blocking */
int  uart_tx_ready(void);
int  uart_rx_ready(void);
uint8_t uart_lsr(void);          /* raw line status, for probing */

/* formatted output */
int  printf(const char *fmt, ...);
int  vprintf(const char *fmt, va_list ap);
int  snprintf(char *buf, unsigned size, const char *fmt, ...);
int  vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap);
int  sprintf(char *buf, const char *fmt, ...);
int  vsprintf(char *buf, const char *fmt, va_list ap);
int  putchar(int c);
int  puts(const char *s);

#endif
