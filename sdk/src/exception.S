/*
 * exception.S - exception/interrupt entry for the Lexra RLX4181
 *
 * The core is R3000-class: the general exception vector is at 0x80000080,
 * return is "jr $k0 ; rfe" (there is no eret) and Status keeps a 3-level
 * KU/IE stack that rfe pops.
 *
 * exc_trampoline is a position independent stub (4 words) that irq_init()
 * copies to the vector; it jumps to exc_entry which saves the full context
 * on the current stack and calls exception_dispatch(frame).
 */
#include "rtl8196c.h"

    .set noreorder
    .set noat

/* frame layout (bytes) */
#define F_AT   0x00
#define F_V0   0x04
#define F_V1   0x08
#define F_A0   0x0C
#define F_A1   0x10
#define F_A2   0x14
#define F_A3   0x18
#define F_T0   0x1C
#define F_T1   0x20
#define F_T2   0x24
#define F_T3   0x28
#define F_T4   0x2C
#define F_T5   0x30
#define F_T6   0x34
#define F_T7   0x38
#define F_S0   0x3C
#define F_S1   0x40
#define F_S2   0x44
#define F_S3   0x48
#define F_S4   0x4C
#define F_S5   0x50
#define F_S6   0x54
#define F_S7   0x58
#define F_T8   0x5C
#define F_T9   0x60
#define F_GP   0x64
#define F_SP   0x68
#define F_FP   0x6C
#define F_RA   0x70
#define F_HI   0x74
#define F_LO   0x78
#define F_EPC  0x7C
#define F_STATUS 0x80
#define F_CAUSE  0x84
#define F_BADVA  0x88
#define F_SIZE   0x90

    .section .text.exc_trampoline, "ax"
    .globl exc_trampoline
    .globl exc_trampoline_end
exc_trampoline:
    lui     $k0, %hi(exc_entry)
    addiu   $k0, $k0, %lo(exc_entry)
    jr      $k0
    nop
exc_trampoline_end:

    .text
    .globl exc_entry
    .ent   exc_entry
exc_entry:
    addiu   $sp, $sp, -F_SIZE
    sw      $at, F_AT($sp)
    sw      $v0, F_V0($sp)
    sw      $v1, F_V1($sp)
    sw      $a0, F_A0($sp)
    sw      $a1, F_A1($sp)
    sw      $a2, F_A2($sp)
    sw      $a3, F_A3($sp)
    sw      $t0, F_T0($sp)
    sw      $t1, F_T1($sp)
    sw      $t2, F_T2($sp)
    sw      $t3, F_T3($sp)
    sw      $t4, F_T4($sp)
    sw      $t5, F_T5($sp)
    sw      $t6, F_T6($sp)
    sw      $t7, F_T7($sp)
    sw      $s0, F_S0($sp)
    sw      $s1, F_S1($sp)
    sw      $s2, F_S2($sp)
    sw      $s3, F_S3($sp)
    sw      $s4, F_S4($sp)
    sw      $s5, F_S5($sp)
    sw      $s6, F_S6($sp)
    sw      $s7, F_S7($sp)
    sw      $t8, F_T8($sp)
    sw      $t9, F_T9($sp)
    sw      $gp, F_GP($sp)
    addiu   $t0, $sp, F_SIZE
    sw      $t0, F_SP($sp)
    sw      $fp, F_FP($sp)
    sw      $ra, F_RA($sp)
    mfhi    $t0
    mflo    $t1
    sw      $t0, F_HI($sp)
    sw      $t1, F_LO($sp)
    mfc0    $t0, $14            /* EPC */
    mfc0    $t1, $12            /* Status */
    mfc0    $t2, $13            /* Cause */
    mfc0    $t3, $8             /* BadVAddr */
    nop
    sw      $t0, F_EPC($sp)
    sw      $t1, F_STATUS($sp)
    sw      $t2, F_CAUSE($sp)
    sw      $t3, F_BADVA($sp)

    jal     exception_dispatch
    move    $a0, $sp

    lw      $t0, F_HI($sp)
    lw      $t1, F_LO($sp)
    nop
    mthi    $t0
    mtlo    $t1
    lw      $at, F_AT($sp)
    lw      $v0, F_V0($sp)
    lw      $v1, F_V1($sp)
    lw      $a0, F_A0($sp)
    lw      $a1, F_A1($sp)
    lw      $a2, F_A2($sp)
    lw      $a3, F_A3($sp)
    lw      $t0, F_T0($sp)
    lw      $t1, F_T1($sp)
    lw      $t2, F_T2($sp)
    lw      $t3, F_T3($sp)
    lw      $t4, F_T4($sp)
    lw      $t5, F_T5($sp)
    lw      $t6, F_T6($sp)
    lw      $t7, F_T7($sp)
    lw      $s0, F_S0($sp)
    lw      $s1, F_S1($sp)
    lw      $s2, F_S2($sp)
    lw      $s3, F_S3($sp)
    lw      $s4, F_S4($sp)
    lw      $s5, F_S5($sp)
    lw      $s6, F_S6($sp)
    lw      $s7, F_S7($sp)
    lw      $t8, F_T8($sp)
    lw      $t9, F_T9($sp)
    lw      $gp, F_GP($sp)
    lw      $fp, F_FP($sp)
    lw      $ra, F_RA($sp)
    lw      $k0, F_EPC($sp)     /* return address, possibly adjusted by C */
    addiu   $sp, $sp, F_SIZE
    jr      $k0
    .word   0x42000010          /* rfe (MIPS-I); in the jr delay slot */
    .end exc_entry

/* ------------------------------------------------------------ CP0 helpers */
    .globl cp0_status_read
    .ent   cp0_status_read
cp0_status_read:
    mfc0    $v0, $12
    jr      $ra
    nop
    .end cp0_status_read

    .globl cp0_status_write
    .ent   cp0_status_write
cp0_status_write:
    mtc0    $a0, $12
    nop
    jr      $ra
    nop
    .end cp0_status_write

    .globl cp0_cause_read
    .ent   cp0_cause_read
cp0_cause_read:
    mfc0    $v0, $13
    jr      $ra
    nop
    .end cp0_cause_read

    .globl cp0_prid_read
    .ent   cp0_prid_read
cp0_prid_read:
    mfc0    $v0, $15
    jr      $ra
    nop
    .end cp0_prid_read

/*
 * cctl_write(cmd): Lexra cache control, same clear/write/clear idiom as the
 * vendor boot code and Linux c-r3k.c.  cmd = CCTL_IINVAL (0x002) invalidates
 * the whole instruction cache.
 */
    .globl cctl_write
    .ent   cctl_write
cctl_write:
    mtc0    $zero, $20
    nop
    mtc0    $a0, $20
    nop
    nop
    mtc0    $zero, $20
    nop
    jr      $ra
    nop
    .end cctl_write

/*
 * ota_reloc_stub(dst=$a0, src=$a1, len_words=$a2, entry=$a3)
 * Position-independent stub copied to safe KSEG1 RAM (0xA0400000) to copy a
 * staged image over the currently running program, flush I/D caches via CCTL,
 * and jump to entry.
 *
 * The D-cache is write-back (docs/01-hardware-notes.md), so DINVAL (0x001)
 * must not be used here:
 *  - before the copy, CCTL_DWBINVAL (0x200) flushes the old program's dirty
 *    lines, so none can be written back over the new image later (uncached
 *    dst) and a staged src is current in RAM (uncached src);
 *  - after the copy, CCTL_DWBINVAL again pushes the new image to RAM (cached
 *    dst; DINVAL would drop the dirty tail), then CCTL_IINVAL (0x002).
 * Two separate CCTL writes: 0x200|0x002 in one write is not verified.
 */
    .globl ota_reloc_stub
    .globl ota_reloc_stub_end
ota_reloc_stub:
    mtc0    $zero, $12          /* disable interrupts */
    nop
    mtc0    $zero, $20
    nop
    addiu   $t0, $zero, 0x200   /* CCTL_DWBINVAL */
    mtc0    $t0, $20
    nop
    nop
    mtc0    $zero, $20
    nop
1:
    beq     $a2, $zero, 2f
    nop
    lw      $t0, 0($a1)
    nop
    sw      $t0, 0($a0)
    addiu   $a0, $a0, 4
    addiu   $a1, $a1, 4
    b       1b
    addiu   $a2, $a2, -1
2:
    mtc0    $zero, $20
    nop
    addiu   $t0, $zero, 0x200   /* CCTL_DWBINVAL: new image to RAM */
    mtc0    $t0, $20
    nop
    nop
    mtc0    $zero, $20
    nop
    addiu   $t0, $zero, 2       /* CCTL_IINVAL */
    mtc0    $t0, $20
    nop
    nop
    mtc0    $zero, $20
    nop
    jr      $a3
    nop
ota_reloc_stub_end:
