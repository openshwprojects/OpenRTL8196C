#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* register frame saved by exc_entry (see exception.S) */
struct exc_frame {
    uint32_t at, v0, v1, a0, a1, a2, a3;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t t8, t9, gp, sp, fp, ra;
    uint32_t hi, lo;
    uint32_t epc, status, cause, badvaddr;
    uint32_t pad;
};

typedef void (*irq_handler_t)(int irq, void *arg);

/* Install the exception trampoline at the Lexra vector (0x80000080),
 * invalidate the I-cache, mask every source in GIMR and clear routing. */
void irq_init(void);

/* Route source irq (bit number in GIMR) to CPU line rs (IRQ_RS_IP2..7) and
 * attach a handler.  The source is still masked until irq_enable(). */
void irq_attach(int irq, int rs, irq_handler_t fn, void *arg);
void irq_enable(int irq);
void irq_disable(int irq);

/* CPU-level interrupt enable (Status.IEc + IM2..IM7). */
void irq_global_enable(void);
void irq_global_disable(void);

/* Optional hook for non-interrupt exceptions.  Return nonzero to resume at
 * frame->epc (adjust it first), zero to let the default handler dump and halt. */
extern int (*exception_hook)(struct exc_frame *frame);

/* statistics for the examples */
extern volatile uint32_t irq_count_total;
extern volatile uint32_t irq_count[32];
extern volatile uint32_t irq_last_cause;

/* assembly helpers (exception.S) */
uint32_t cp0_status_read(void);
void     cp0_status_write(uint32_t v);
uint32_t cp0_cause_read(void);
uint32_t cp0_prid_read(void);
void     cctl_write(uint32_t cmd);

#endif
