/*
 * 98_usb_vbus - which state of GPIO A4 actually powers the USB port?
 *
 * The host is brought up with the SDK driver, then A4 is held high, low
 * and released as an input (6 s each, twice), while the root port state
 * is sampled every 500 ms.  A device that gets power pulls D+ or D- up:
 * EHCI PORTSC "conn" goes to 1 and the line state leaves 0 (SE0).
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "gpio.h"
#include "usb.h"

static void phase(const char *name, int mode)
{
    if (mode < 0) {
        gpio_set_dir(GPIO_A4_USB_POWER, 0);
    } else {
        gpio_set_dir(GPIO_A4_USB_POWER, 1);
        gpio_write(GPIO_A4_USB_POWER, mode);
    }
    printf("--- A4 %s (CNR=%08x DIR=%08x DAT=%08x MUX=%08x)\n", name, REG32(GPIO_PABCD_CNR),
           REG32(GPIO_PABCD_DIR), REG32(GPIO_PABCD_DAT), REG32(PIN_MUX_SEL));
    for (int i = 0; i < 12; i++) {
        delay_ms(500);
        usb_print_ports();
    }
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb vbus probe\n");
    usb_debug = 1;
    if (usb_host_init() < 0) {
        printf("controller does not respond\n");
        return 1;
    }
    for (int round = 0; round < 2; round++) {
        phase("HIGH", 1);
        phase("LOW", 0);
        phase("INPUT (released)", -1);
    }
    printf("done\n");
    return 0;
}
