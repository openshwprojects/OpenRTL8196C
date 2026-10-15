/*
 * 99_usb_vbus_blink - make the USB port power visible.
 *
 * Every 5 s: A4 HIGH with the router's USB LED (C3) on, then A4 LOW with
 * the LED off.  An optical mouse (or a reader with an LED) on the port
 * blinks in step with the USB LED if high = power on, in the opposite
 * rhythm if low = power on, and stays dark if the switch never turns on.
 * Port state is printed every second so a connect is also logged.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "usb.h"

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb vbus blink: A4 high + USB LED on 5 s, A4 low + LED off 5 s, forever\n");
    usb_host_init();
    gpio_set_dir(GPIO_C3_USB_LED, 1);
    for (unsigned n = 0;; n++) {
        int high = (n & 1) == 0;
        gpio_set_dir(GPIO_A4_USB_POWER, 1);
        gpio_write(GPIO_A4_USB_POWER, high);
        gpio_write(GPIO_C3_USB_LED, high ? 0 : 1);     /* LED active low */
        printf("--- phase %u: A4 %s\n", n, high ? "HIGH (USB LED on)" : "LOW (USB LED off)");
        for (int i = 0; i < 5; i++) {
            delay_ms(1000);
            usb_print_ports();
        }
    }
}
