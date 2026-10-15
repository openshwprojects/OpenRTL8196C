/*
 * 06_uart_echo - UART in and out, polled.  Echoes every byte, shows its hex
 * value, and keeps a one-line command buffer: "reboot" resets the SoC.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "libc.h"

int main(void)
{
    char line[64];
    unsigned len = 0;

    uart_init();
    printf("\n\nuart echo: type something, 'reboot' to reset\n> ");
    for (;;) {
        int c = uart_getc();
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            printf("\nline (%u bytes): \"%s\"\n", len, line);
            if (strcmp(line, "reboot") == 0) {
                printf("rebooting\n");
                wdt_reboot();
            }
            len = 0;
            printf("> ");
        } else if (c == 8 || c == 127) {
            if (len) { len--; uart_puts("\b \b"); }
        } else {
            if (len < sizeof(line) - 1)
                line[len++] = (char)c;
            uart_putc((char)c);
            if (c < 32 || c > 126)
                printf("[%02x]", c);
        }
    }
}
