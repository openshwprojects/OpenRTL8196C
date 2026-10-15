/*
 * start.S - entry point for programs loaded by the Realtek boot loader
 *
 * The loader jumps here with "J <LOAD_ADDR>".  Nothing can be assumed:
 * no stack, no $gp, .bss not cleared, interrupts possibly enabled.
 * MIPS-I only, written with .set noreorder so every delay slot is explicit.
 */
#include "rtl8196c.h"

    .set noreorder
    .set noat

    .section .entry, "ax"
    .globl _start
    .ent   _start
_start:
    /* Status = 0: interrupts off, kernel mode, BEV = 0, caches not isolated */
    mtc0    $zero, $12
    nop
    nop

    /* stack */
    lui     $sp, %hi(_stack_top)
    addiu   $sp, $sp, %lo(_stack_top)

    /* clear .bss */
    lui     $t0, %hi(_bss_start)
    addiu   $t0, $t0, %lo(_bss_start)
    lui     $t1, %hi(_bss_end)
    addiu   $t1, $t1, %lo(_bss_end)
1:
    beq     $t0, $t1, 2f
    nop
    sw      $zero, 0($t0)
    b       1b
    addiu   $t0, $t0, 4
2:
    jal     main
    nop

    /* main returned: park the CPU with interrupts disabled */
    mtc0    $zero, $12
3:
    b       3b
    nop
    .end _start
