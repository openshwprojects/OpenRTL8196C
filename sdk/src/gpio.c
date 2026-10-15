/*
 * gpio.c - GPIO ports A-D and the LAN LEDs
 */
#include "rtl8196c.h"
#include "gpio.h"

static const uint8_t led_pins[7] = {
    GPIO_LED_PORT0, GPIO_LED_PORT1, GPIO_LED_PORT2, GPIO_LED_PORT3,
    GPIO_LED_PORT4, GPIO_LED_P1, GPIO_LED_P2,
};

void gpio_set_dir(int pin, int output)
{
    if (pin >= 0 && pin <= 7)
        REG32(PIN_MUX_SEL) |= PIN_MUX_PORTA_AS_GPIO;
    REG32(GPIO_PABCD_CNR) &= ~(1u << pin);      /* pin is GPIO, not peripheral */
    if (output)
        REG32(GPIO_PABCD_DIR) |= 1u << pin;
    else
        REG32(GPIO_PABCD_DIR) &= ~(1u << pin);
}

void gpio_write(int pin, int value)
{
    if (value)
        REG32(GPIO_PABCD_DAT) |= 1u << pin;
    else
        REG32(GPIO_PABCD_DAT) &= ~(1u << pin);
}

int gpio_read(int pin)
{
    return (REG32(GPIO_PABCD_DAT) >> pin) & 1;
}

void gpio_toggle(int pin)
{
    REG32(GPIO_PABCD_DAT) ^= 1u << pin;
}

void gpio_leds_init(void)
{
    REG32(PIN_MUX_SEL) = (REG32(PIN_MUX_SEL) & ~0x3FFFu) | PIN_MUX_LEDS_AS_GPIO;
    for (int i = 0; i < 7; i++)
        gpio_set_dir(led_pins[i], 1);
}

void gpio_leds_write(uint32_t mask)
{
    for (int i = 0; i < 7; i++)
        gpio_write(led_pins[i], (mask >> i) & 1);
}
