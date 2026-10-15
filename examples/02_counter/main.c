/*
 * 02_counter - print 1 2 3 4 5 ... forever, one number per ~second (busy loop).
 * The loop constant is a guess; 04_timer_poll replaces it with a real timer.
 */
#include "uart.h"
#include "gpio.h"
#include "timer.h"

int main(void)
{
    uart_init();
    printf("\n\ncounter start\n");
    for (unsigned n = 1;; n++) {
        printf("%u\n", n);
        delay_loop(2000000);
    }
}
