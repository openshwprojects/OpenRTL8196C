/*
 * 04_timer_poll - run TC0 as a periodic timer and poll its pending flag.
 *
 * Divisor 200 gives a 1 MHz tick (bus clock 200 MHz, measured), so a period
 * of 1000000 fires once per second.  TC1 runs as a free-running 28-bit
 * microsecond counter; dtc1 should print ~1000000 every line.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"

#define DIVISOR   200u
#define RELOAD    1000000u

int main(void)
{
    uart_init();
    printf("\n\ntimer poll: divisor %u reload %u\n", DIVISOR, RELOAD);

    timer_set_divisor(DIVISOR);
    timer_start(1, TC_DATA_MAX, 1, 0);    /* free running microseconds */
    timer_start(0, RELOAD, 1, 0);         /* periodic, no interrupt */

    uint32_t last_tc1 = timer_count(1);
    for (unsigned tick = 1;; tick++) {
        while (!timer_pending(0))
            ;
        timer_ack(0);
        uint32_t tc1 = timer_count(1);
        uint32_t delta = (tc1 - last_tc1) & TC_DATA_MAX;  /* up counter */
        last_tc1 = tc1;
        printf("tick %u  tc0=%u tc1=%u dtc1=%u\n", tick, timer_count(0), tc1, delta);
    }
}
