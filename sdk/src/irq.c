/*
 * irq.c - interrupt controller + exception dispatch
 *
 * Dispatch is driven by GISR & GIMR rather than by Cause.IP, so it works no
 * matter which CPU line a source was routed to.  Non-interrupt exceptions
 * are reported on the UART and the CPU is parked.
 */
#include "rtl8196c.h"
#include "irq.h"
#include "uart.h"
#include "timer.h"

extern const uint32_t exc_trampoline[];
extern const uint32_t exc_trampoline_end[];

static irq_handler_t handlers[32];
static void *handler_args[32];

int (*exception_hook)(struct exc_frame *frame);

volatile uint32_t irq_count_total;
volatile uint32_t irq_count[32];
volatile uint32_t irq_last_cause;

static const char *const exc_names[32] = {
    "Int", "TLBmod", "TLBL", "TLBS", "AdEL", "AdES", "IBE", "DBE",
    "Sys", "Bp", "RI", "CpU", "Ov", "Tr", "?14", "FPE",
    "?16", "?17", "?18", "?19", "?20", "?21", "?22", "?23",
    "?24", "?25", "?26", "?27", "?28", "?29", "?30", "?31",
};

void irq_init(void)
{
    irq_global_disable();

    REG32(INTC_GIMR) = 0;
    REG32(INTC_IRR0) = 0;
    REG32(INTC_IRR1) = 0;
    REG32(INTC_IRR2) = 0;
    REG32(INTC_IRR3) = 0;

    /* copy the trampoline through the uncached alias, then drop the I-cache */
    volatile uint32_t *vec = (volatile uint32_t *)KSEG1(EXC_VECTOR_PHYS);
    for (const uint32_t *p = exc_trampoline; p < exc_trampoline_end; p++)
        *vec++ = *p;
    cctl_write(CCTL_IINVAL);
}

void irq_attach(int irq, int rs, irq_handler_t fn, void *arg)
{
    uint32_t reg = INTC_IRR0 + 4 * (irq / 8);
    int shift = 4 * (irq % 8);

    handlers[irq] = fn;
    handler_args[irq] = arg;
    REG32(reg) = (REG32(reg) & ~(0xFu << shift)) | ((uint32_t)(rs & 0xF) << shift);
}

void irq_enable(int irq)
{
    REG32(INTC_GIMR) |= 1u << irq;
}

void irq_disable(int irq)
{
    REG32(INTC_GIMR) &= ~(1u << irq);
}

void irq_global_enable(void)
{
    cp0_status_write((cp0_status_read() & ~0xFF00u) | ST_IM_ALL_HW | ST_IEC);
}

void irq_global_disable(void)
{
    cp0_status_write(cp0_status_read() & ~ST_IEC);
}

static void dump_frame(const struct exc_frame *f)
{
    printf("\n*** exception %s (code %u) epc=%08x cause=%08x status=%08x badva=%08x\n",
           exc_names[CAUSE_EXCCODE(f->cause)], CAUSE_EXCCODE(f->cause),
           f->epc, f->cause, f->status, f->badvaddr);
    printf("at=%08x v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x\n",
           f->at, f->v0, f->v1, f->a0, f->a1, f->a2, f->a3);
    printf("t0=%08x t1=%08x t2=%08x t3=%08x t4=%08x t5=%08x t6=%08x t7=%08x\n",
           f->t0, f->t1, f->t2, f->t3, f->t4, f->t5, f->t6, f->t7);
    printf("s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x\n",
           f->s0, f->s1, f->s2, f->s3, f->s4, f->s5, f->s6, f->s7);
    printf("t8=%08x t9=%08x gp=%08x sp=%08x fp=%08x ra=%08x hi=%08x lo=%08x\n",
           f->t8, f->t9, f->gp, f->sp, f->fp, f->ra, f->hi, f->lo);
}

static uint32_t frame_get_reg(const struct exc_frame *f, uint32_t r)
{
    if (r == 0) return 0;
    const uint32_t *regs = &f->at; /* at is $1 .. t9 is $25 */
    if (r >= 1 && r <= 25) return regs[r - 1];
    if (r == 28) return f->gp;
    if (r == 29) return f->sp;
    if (r == 30) return f->fp;
    if (r == 31) return f->ra;
    return 0;
}

static void frame_set_reg(struct exc_frame *f, uint32_t r, uint32_t val)
{
    if (r == 0) return;
    uint32_t *regs = &f->at;
    if (r >= 1 && r <= 25) { regs[r - 1] = val; return; }
    if (r == 28) { f->gp = val; return; }
    if (r == 29) { f->sp = val; return; }
    if (r == 30) { f->fp = val; return; }
    if (r == 31) { f->ra = val; return; }
}

static int emulate_unaligned_or_lwl(struct exc_frame *f)
{
    uint32_t code = CAUSE_EXCCODE(f->cause);
    if (code != 10 && code != 4 && code != 5)
        return 0;
    if (f->cause & CAUSE_BD)
        return 0;

    uint32_t insn = *(const uint32_t *)f->epc;
    uint32_t op   = insn >> 26;
    uint32_t rs   = (insn >> 21) & 0x1Fu;
    uint32_t rt   = (insn >> 16) & 0x1Fu;
    int32_t  imm  = (int16_t)(insn & 0xFFFFu);
    uint32_t addr = frame_get_reg(f, rs) + (uint32_t)imm;
    uint8_t *p    = (uint8_t *)addr;
    uint32_t k    = addr & 3u;
    uint32_t val  = frame_get_reg(f, rt);

    switch (op) {
    case 0x22: /* lwl */
        for (uint32_t i = 0; i <= 3u - k; i++) {
            uint32_t shift = 24u - 8u * i;
            val = (val & ~(0xFFu << shift)) | ((uint32_t)p[i] << shift);
        }
        frame_set_reg(f, rt, val);
        f->epc += 4;
        return 1;
    case 0x26: /* lwr */
        for (uint32_t i = 0; i <= k; i++) {
            uint32_t shift = 8u * i;
            val = (val & ~(0xFFu << shift)) | ((uint32_t)(*(p - i)) << shift);
        }
        frame_set_reg(f, rt, val);
        f->epc += 4;
        return 1;
    case 0x2A: /* swl */
        for (uint32_t i = 0; i <= 3u - k; i++)
            p[i] = (uint8_t)(val >> (24u - 8u * i));
        f->epc += 4;
        return 1;
    case 0x2E: /* swr */
        for (uint32_t i = 0; i <= k; i++)
            *(p - i) = (uint8_t)(val >> (8u * i));
        f->epc += 4;
        return 1;
    case 0x23: /* unaligned lw */
        val = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
              ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
        frame_set_reg(f, rt, val);
        f->epc += 4;
        return 1;
    case 0x2B: /* unaligned sw */
        p[0] = (uint8_t)(val >> 24);
        p[1] = (uint8_t)(val >> 16);
        p[2] = (uint8_t)(val >> 8);
        p[3] = (uint8_t)val;
        f->epc += 4;
        return 1;
    case 0x21: /* unaligned lh */
        val = (uint32_t)(int32_t)(int16_t)(((uint16_t)p[0] << 8) | p[1]);
        frame_set_reg(f, rt, val);
        f->epc += 4;
        return 1;
    case 0x25: /* unaligned lhu */
        val = ((uint32_t)p[0] << 8) | p[1];
        frame_set_reg(f, rt, val);
        f->epc += 4;
        return 1;
    case 0x29: /* unaligned sh */
        p[0] = (uint8_t)(val >> 8);
        p[1] = (uint8_t)val;
        f->epc += 4;
        return 1;
    default:
        return 0;
    }
}

void exception_dispatch(struct exc_frame *f)
{
    irq_last_cause = f->cause;

    if (CAUSE_EXCCODE(f->cause) != 0) {
        if (emulate_unaligned_or_lwl(f))
            return;
        if (exception_hook && exception_hook(f))
            return;
        dump_frame(f);
        printf("*** rebooting via watchdog\n");
        REG32(GPIO_PABCD_DAT) &= ~(1u << 4);
        wdt_reboot();
    }

    irq_count_total++;
    uint32_t pending = REG32(INTC_GISR) & REG32(INTC_GIMR);
    if (!pending) {
        /* nothing we own: silently return, the IP bit must be cleared by someone else */
        return;
    }
    for (int i = 0; i < 32; i++) {
        if (pending & (1u << i)) {
            irq_count[i]++;
            if (handlers[i])
                handlers[i](i, handler_args[i]);
            else
                REG32(INTC_GIMR) &= ~(1u << i);   /* unhandled: mask it off */
        }
    }
}
