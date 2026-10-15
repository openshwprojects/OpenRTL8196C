/*
 * rtos_switch.S - Cooperative context switch for Lexra RLX4181 (pure MIPS-I).
 */
    .set noreorder
    .set noat
    .text

/*
 * void rtos_switch_ctx(struct rtos_ctx *old_ctx, const struct rtos_ctx *new_ctx)
 */
    .globl rtos_switch_ctx
    .ent   rtos_switch_ctx
rtos_switch_ctx:
    sw  $16,  0($4)     /* s0 */
    sw  $17,  4($4)     /* s1 */
    sw  $18,  8($4)     /* s2 */
    sw  $19, 12($4)     /* s3 */
    sw  $20, 16($4)     /* s4 */
    sw  $21, 20($4)     /* s5 */
    sw  $22, 24($4)     /* s6 */
    sw  $23, 28($4)     /* s7 */
    sw  $28, 32($4)     /* gp */
    sw  $29, 36($4)     /* sp */
    sw  $30, 40($4)     /* fp */
    sw  $31, 44($4)     /* ra */

    lw  $16,  0($5)     /* s0 */
    lw  $17,  4($5)     /* s1 */
    lw  $18,  8($5)     /* s2 */
    lw  $19, 12($5)     /* s3 */
    lw  $20, 16($5)     /* s4 */
    lw  $21, 20($5)     /* s5 */
    lw  $22, 24($5)     /* s6 */
    lw  $23, 28($5)     /* s7 */
    lw  $28, 32($5)     /* gp */
    lw  $29, 36($5)     /* sp */
    lw  $30, 40($5)     /* fp */
    lw  $31, 44($5)     /* ra */
    nop
    jr  $31
    nop
    .end rtos_switch_ctx

/*
 * Trampoline entered when a new task is scheduled for the first time.
 * s0 holds the task entry function pointer; s1 holds its argument.
 */
    .globl rtos_task_trampoline
    .ent   rtos_task_trampoline
rtos_task_trampoline:
    move $4, $17        /* a0 = s1 (arg) */
    jalr $16            /* call s0 (entry_fn) */
    nop
    jal  rtos_task_exit
    nop
1:  b    1b
    nop
    .end rtos_task_trampoline

/*
 * uint32_t rtos_get_gp(void)
 */
    .globl rtos_get_gp
    .ent   rtos_get_gp
rtos_get_gp:
    jr   $31
    move $2, $28
    .end rtos_get_gp
