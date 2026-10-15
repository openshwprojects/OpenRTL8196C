#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Two 28-bit UP counters clocked by 200 MHz / divisor (CDBR, shared).
 * A timer counts 0..period-1 and wraps; the wrap sets the pending flag.
 * With divisor 200 one tick is 1 us (measured). */

void     timer_set_divisor(uint32_t div);          /* 1..65535 */
void     timer_start(int tc, uint32_t period, int periodic, int irq);
void     timer_stop(int tc);
uint32_t timer_count(int tc);                      /* current 28-bit value */
void     timer_ack(int tc);                        /* clear pending flag */
int      timer_pending(int tc);

/* Calibrated delays: timer_delay_init() runs TC1 as a free-running 1 MHz
 * counter (divisor 200); after that delay_us()/delay_ms() are exact. */
void     timer_delay_init(void);
uint32_t micros(void);                             /* wraps every 2^28 us */
uint32_t elapsed_us(uint32_t since);               /* micros() - since, wrap safe */
uint32_t timer_ms(void);                           /* wrap-safe millisecond counter */
/* periodic helper: returns 1 and bumps *last when period_ms have passed since *last */
int      every_ms(uint32_t *last, uint32_t period_ms);
void     delay_us(uint32_t us);
void     delay_ms(uint32_t ms);
void     delay_loop(uint32_t iterations);          /* uncalibrated busy wait (~2.5 M/s uncached), needs no timer */

/* watchdog */
void     wdt_disable(void);
void     wdt_reboot(void);                         /* never returns */

#endif
