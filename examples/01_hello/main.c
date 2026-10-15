/*
 * 01_hello - prove the toolchain + loader path: print a banner and some CP0 state.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "irq.h"

int main(void)
{
    uart_init();
    printf("\n\nOpenRTL8196C hello from RAM\n");
    printf("PRId   = %08x\n", cp0_prid_read());
    printf("Status = %08x\n", cp0_status_read());
    printf("Cause  = %08x\n", cp0_cause_read());
    printf("GIMR   = %08x  GISR = %08x\n", REG32(INTC_GIMR), REG32(INTC_GISR));
    printf("IRR0-3 = %08x %08x %08x %08x\n",
           REG32(INTC_IRR0), REG32(INTC_IRR1), REG32(INTC_IRR2), REG32(INTC_IRR3));
    printf("TCCNR  = %08x  TCIR = %08x  CDBR = %08x  WDT = %08x\n",
           REG32(TCCNR), REG32(TCIR), REG32(CDBR), REG32(WDTCNR));
    printf("UART   LSR=%02x IER=%02x IIR=%02x LCR=%02x MCR=%02x\n",
           REG8(UART_LSR), REG8(UART_IER), REG8(UART_IIR), REG8(UART_LCR), REG8(UART_MCR));
    printf("PINMUX = %08x\n", REG32(PIN_MUX_SEL));
    printf("done\n");
    return 0;
}
