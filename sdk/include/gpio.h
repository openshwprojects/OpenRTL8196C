#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>
#include "rtl8196c.h"

/* pin = GPIO_PIN('B', 3) etc. (0..31 over ports A..D) */
void gpio_set_dir(int pin, int output);
void gpio_write(int pin, int value);
int  gpio_read(int pin);
void gpio_toggle(int pin);

/* Switch the 7 LAN LED pins from the switch's LED controller to GPIO and
 * make them outputs.  Keeps the UART pins untouched. */
void gpio_leds_init(void);
void gpio_leds_write(uint32_t mask);   /* bit i = LED i (0..6), 1 = on */

#endif
