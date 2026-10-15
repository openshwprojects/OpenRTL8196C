/*
 * 91_irq_probe - find out how exceptions and interrupts really work here.
 *
 * Phase 1: install the trampoline at 0x80000000/0x80/0x180, execute `break`,
 *          then restore the loader's words at 0x000 and 0x180 and `break`
 *          again.  If the count still grows, the vector is 0x80000080.
 * Phase 2: with CPU interrupts masked, make TC0 wrap and look at Cause.IP
 *          and GISR for every IRR1 routing nibble value 0..15.
 * Phase 3: enable CPU interrupts with the first routing value that showed an
 *          IP bit and count handler invocations for a few seconds.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "irq.h"
#include "timer.h"
#include "gpio.h"

extern const uint32_t exc_trampoline[];
extern const uint32_t exc_trampoline_end[];

static volatile uint32_t breaks;
static volatile uint32_t tc0_irqs;
static volatile uint32_t last_cause_in_irq;

static int on_exception(struct exc_frame *f)
{
    if (CAUSE_EXCCODE(f->cause) == 9) {        /* Bp */
        breaks++;
        f->epc += 4;
        return 1;
    }
    return 0;
}

static void on_tc0(int irq, void *arg)
{
    (void)irq; (void)arg;
    last_cause_in_irq = cp0_cause_read();
    timer_ack(0);
    tc0_irqs++;
}

static void install_at(uint32_t phys)
{
    volatile uint32_t *vec = (volatile uint32_t *)KSEG1(phys);
    for (const uint32_t *p = exc_trampoline; p < exc_trampoline_end; p++)
        *vec++ = *p;
}

static void save_words(uint32_t phys, uint32_t *buf, int n)
{
    volatile uint32_t *v = (volatile uint32_t *)KSEG1(phys);
    for (int i = 0; i < n; i++) buf[i] = v[i];
}

static void restore_words(uint32_t phys, const uint32_t *buf, int n)
{
    volatile uint32_t *v = (volatile uint32_t *)KSEG1(phys);
    for (int i = 0; i < n; i++) v[i] = buf[i];
}

static void do_break(void)
{
    __asm__ volatile("break 0\n\tnop");
}

static int wait_tc0_wrap(uint32_t timeout_loops)
{
    while (timeout_loops--) {
        if (REG32(TCIR) & TCIR_TC0IP)
            return 1;
    }
    return 0;
}

int main(void)
{
    uint32_t orig0[4], orig80[4], orig180[4];

    uart_init();
    printf("\n\nirq probe\n");
    printf("loader vectors: 000=%08x %08x  080=%08x %08x  180=%08x %08x\n",
           REG32(KSEG1(0x000)), REG32(KSEG1(0x004)), REG32(KSEG1(0x080)), REG32(KSEG1(0x084)),
           REG32(KSEG1(0x180)), REG32(KSEG1(0x184)));
    save_words(0x000, orig0, 4);
    save_words(0x080, orig80, 4);
    save_words(0x180, orig180, 4);

    /* ---- phase 1: vector address ---- */
    irq_global_disable();
    REG32(INTC_GIMR) = 0;
    exception_hook = on_exception;
    install_at(0x000);
    install_at(0x080);
    install_at(0x180);
    cctl_write(CCTL_IINVAL);
    printf("phase1: break with trampolines at 000/080/180 ... ");
    do_break();
    do_break();
    printf("breaks=%u\n", breaks);

    restore_words(0x000, orig0, 4);
    restore_words(0x180, orig180, 4);
    cctl_write(CCTL_IINVAL);
    printf("phase1: break with trampoline only at 080 ... ");
    do_break();
    printf("breaks=%u (3 means vector is 0x80000080)\n", breaks);

    /* ---- phase 2: routing sweep with CPU interrupts masked ---- */
    irq_init();                                   /* trampoline at 0x80, GIMR=0, IRR=0 */
    exception_hook = on_exception;
    timer_set_divisor(200);
    timer_start(0, 100000, 1, 1);                 /* 100 ms period, TC0IE on */
    printf("phase2: GIMR=0: wait wrap -> ");
    wait_tc0_wrap(50000000);
    printf("tcir=%08x gisr=%08x cause=%08x\n", REG32(TCIR), REG32(INTC_GISR), cp0_cause_read());
    timer_ack(0);

    REG32(INTC_GIMR) = 0xFFFFFFFFu;               /* unmask everything in the controller */
    printf("phase2: GIMR=ffffffff, IRR all 0: wait wrap -> ");
    wait_tc0_wrap(50000000);
    printf("tcir=%08x gisr=%08x cause=%08x\n", REG32(TCIR), REG32(INTC_GISR), cp0_cause_read());
    timer_ack(0);

    int found_rs = -1;
    for (int rs = 0; rs < 16; rs++) {
        REG32(INTC_IRR1) = (uint32_t)rs;          /* nibble 0 = source 8 */
        REG32(INTC_IRR0) = 0;
        REG32(INTC_IRR2) = 0;
        REG32(INTC_IRR3) = 0;
        timer_ack(0);
        int ok = wait_tc0_wrap(50000000);
        uint32_t cause = cp0_cause_read();
        uint32_t gisr = REG32(INTC_GISR);
        timer_ack(0);
        delay_loop(1000);
        uint32_t cause_after = cp0_cause_read();
        printf("  rs=%2d wrap=%d cause=%08x ip=%02x gisr=%08x  after ack: cause=%08x\n",
               rs, ok, cause, (cause >> 8) & 0xFF, gisr, cause_after);
        if (found_rs < 0 && ((cause >> 10) & 0x3F))
            found_rs = rs;
    }

    /* ---- phase 3: real interrupts ---- */
    if (found_rs < 0) {
        printf("phase3: no routing value raised Cause.IP, trying rs=2 anyway\n");
        found_rs = 2;
    }
    irq_attach(IRQ_TC0, found_rs, on_tc0, 0);
    REG32(INTC_GIMR) = 1u << IRQ_TC0;
    timer_ack(0);
    irq_global_enable();
    printf("phase3: rs=%d, interrupts on, status=%08x\n", found_rs, cp0_status_read());
    for (int i = 0; i < 10; i++) {
        delay_loop(600000);
        printf("  tc0_irqs=%u total=%u cause_in_irq=%08x gisr=%08x\n",
               tc0_irqs, irq_count_total, last_cause_in_irq, REG32(INTC_GISR));
    }
    irq_global_disable();
    printf("done\n");
    return 0;
}
