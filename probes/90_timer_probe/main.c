/*
 * 90_timer_probe - dump the raw timer registers while both timers run.
 *
 * Run with "flash.py -t" so each line carries PC time; from the raw TCxCNT
 * values you get the counting direction, the bit position of the count and
 * the tick rate (bus clock / CDBR divisor).
 */
#include "rtl8196c.h"
#include "uart.h"
#include "gpio.h"
#include "timer.h"

static void dump(const char *tag)
{
    printf("%s TC0DATA=%08x TC1DATA=%08x TC0CNT=%08x TC1CNT=%08x TCCNR=%08x TCIR=%08x CDBR=%08x\n",
           tag, REG32(TC0DATA), REG32(TC1DATA), REG32(TC0CNT), REG32(TC1CNT),
           REG32(TCCNR), REG32(TCIR), REG32(CDBR));
}

int main(void)
{
    uart_init();
    printf("\n\ntimer probe\n");
    dump("loader");

    REG32(TCCNR) = 0;                       /* stop both */
    REG32(TCIR)  = TCIR_TC0IP | TCIR_TC1IP; /* clear pending, no IE */
    REG32(CDBR)  = 200u << CDBR_DIV_SHIFT;  /* 200 MHz / 200 = 1 MHz if the guess is right */
    REG32(TC0DATA) = 0x01000000;            /* raw value: 2^24 */
    REG32(TC1DATA) = 0xFFFFFFF0;            /* raw value: maximum */
    dump("set   ");
    REG32(TCCNR) = TCCNR_TC0EN | TCCNR_TC0_TIMER | TCCNR_TC1EN | TCCNR_TC1_TIMER;

    for (int i = 0; i < 40; i++) {
        dump("run   ");
        if (REG32(TCIR) & TCIR_TC0IP) {
            printf("  TC0 pending -> ack\n");
            REG32(TCIR) |= TCIR_TC0IP;
        }
        delay_loop(300000);
    }
    printf("done\n");
    return 0;
}
