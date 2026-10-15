/*
 * 92_intc_probe - what does the interrupt controller block really do?
 *
 *  - dump 0xB8003000..0xB800303C before/after writes and after a timer wrap
 *  - check that GIMR / IRRx writes read back
 *  - route every nibble of every IRR to the same value and watch Cause.IP
 *  - CP0 census: read registers 0..31 (reserved-instruction traps are
 *    caught and reported) to spot Lexra extension registers
 */
#include "rtl8196c.h"
#include "uart.h"
#include "irq.h"
#include "timer.h"
#include "gpio.h"

static volatile uint32_t trap_code;

static int on_exception(struct exc_frame *f)
{
    trap_code = CAUSE_EXCCODE(f->cause);
    f->epc += 4;
    return 1;
}

static void dump_block(const char *tag)
{
    printf("%s:", tag);
    for (int i = 0; i < 16; i++)
        printf(" %08x", REG32(INTC_BASE + 4 * i));
    printf("\n");
}

static int wait_wrap(void)
{
    for (uint32_t n = 0; n < 50000000; n++)
        if (REG32(TCIR) & TCIR_TC0IP)
            return 1;
    return 0;
}

#define MFC0(n) ({ uint32_t v; __asm__ volatile("mfc0 %0, $" #n "\n\tnop" : "=r"(v)); v; })

static uint32_t read_cp0(int n)
{
    switch (n) {
    case 0: return MFC0(0);   case 1: return MFC0(1);   case 2: return MFC0(2);   case 3: return MFC0(3);
    case 4: return MFC0(4);   case 5: return MFC0(5);   case 6: return MFC0(6);   case 7: return MFC0(7);
    case 8: return MFC0(8);   case 9: return MFC0(9);   case 10: return MFC0(10); case 11: return MFC0(11);
    case 12: return MFC0(12); case 13: return MFC0(13); case 14: return MFC0(14); case 15: return MFC0(15);
    case 16: return MFC0(16); case 17: return MFC0(17); case 18: return MFC0(18); case 19: return MFC0(19);
    case 20: return MFC0(20); case 21: return MFC0(21); case 22: return MFC0(22); case 23: return MFC0(23);
    case 24: return MFC0(24); case 25: return MFC0(25); case 26: return MFC0(26); case 27: return MFC0(27);
    case 28: return MFC0(28); case 29: return MFC0(29); case 30: return MFC0(30); default: return MFC0(31);
    }
}

int main(void)
{
    uart_init();
    printf("\n\nintc probe\n");
    irq_init();
    exception_hook = on_exception;

    dump_block("after irq_init (GIMR=0,IRR=0)");

    REG32(INTC_GIMR) = 0xFFFFFFFFu;
    REG32(INTC_IRR0) = 0x22222222u;
    REG32(INTC_IRR1) = 0x22222222u;
    REG32(INTC_IRR2) = 0x22222222u;
    REG32(INTC_IRR3) = 0x22222222u;
    dump_block("after GIMR=ffffffff IRR=2222..  ");

    timer_set_divisor(200);
    timer_start(0, 100000, 1, 1);
    wait_wrap();
    dump_block("after TC0 wrap (TC0IP set)       ");
    printf("cause=%08x status=%08x tcir=%08x\n", cp0_cause_read(), cp0_status_read(), REG32(TCIR));

    static const uint32_t patterns[] = { 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u,
                                         0x77777777u, 0x88888888u, 0xAAAAAAAAu, 0xFFFFFFFFu, 0x11111111u };
    for (unsigned p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        REG32(INTC_IRR0) = patterns[p];
        REG32(INTC_IRR1) = patterns[p];
        REG32(INTC_IRR2) = patterns[p];
        REG32(INTC_IRR3) = patterns[p];
        timer_ack(0);
        wait_wrap();
        printf("IRR=%08x: cause=%08x gisr=%08x gimr=%08x irr1=%08x\n", patterns[p],
               cp0_cause_read(), REG32(INTC_GISR), REG32(INTC_GIMR), REG32(INTC_IRR1));
    }
    timer_ack(0);

    /* GIMR bit-by-bit: which bits stick? */
    for (int b = 0; b < 32; b += 8) {
        REG32(INTC_GIMR) = 0xFFu << b;
        printf("GIMR write %08x reads %08x\n", 0xFFu << b, REG32(INTC_GIMR));
    }
    REG32(INTC_GIMR) = 0;

    printf("CP0 census:\n");
    for (int n = 0; n < 32; n++) {
        trap_code = 0;
        uint32_t v = read_cp0(n);
        if (trap_code)
            printf("  $%-2d trap exccode=%u\n", n, trap_code);
        else
            printf("  $%-2d = %08x\n", n, v);
    }

    /* does a software interrupt (Cause.IP0) work?  sets IP0 via mtc0 cause */
    printf("software interrupt test: ");
    __asm__ volatile("mfc0 $t0, $13\n\tnop\n\tori $t0, $t0, 0x100\n\tmtc0 $t0, $13\n\tnop" ::: "t0");
    printf("cause=%08x\n", cp0_cause_read());

    printf("done\n");
    return 0;
}
