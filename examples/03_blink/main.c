/*
 * 03_blink - walk a light across the LAN LEDs and report on the UART.
 */
#include "uart.h"
#include "gpio.h"
#include "timer.h"

int main(void)
{
    uart_init();
    gpio_leds_init();
    printf("\n\nblink start\n");
    for (unsigned step = 0;; step++) {
        unsigned led = step % 7;
        gpio_leds_write(1u << led);
        printf("step %u led %u\n", step, led);
        delay_loop(1000000);
    }
}
