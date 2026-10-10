/*
 * Runs fn(arg) on the Media Engine with our own stack and the FPU enabled.
 *
 * int me_vorbis_trampoline(void (*fn)(void *), void *arg, void *stack_top)
 *
 * The firmware ME dispatcher gives tasks an unknown (small) stack and does
 * not promise COP1 (FPU) is usable; stb_vorbis needs both. This switches to
 * stack_top, sets Status.CU1, clears FCSR (round to nearest, no FP traps),
 * calls fn, then restores FCSR, Status and the caller's stack.
 */

    .set noreorder
    .set noat
    .text
    .align 4
    .globl me_vorbis_trampoline
    .ent me_vorbis_trampoline
me_vorbis_trampoline:
    addiu   $a2, $a2, -32
    sw      $sp, 0($a2)
    sw      $ra, 4($a2)
    sw      $s0, 8($a2)
    move    $sp, $a2

    mfc0    $s0, $12            /* Status */
    lui     $t0, 0x2000         /* CU1 */
    or      $t0, $s0, $t0
    mtc0    $t0, $12
    sync
    nop
    nop
    nop
    nop
    cfc1    $t1, $31            /* FCSR (needs CU1) */
    sw      $t1, 12($sp)
    ctc1    $zero, $31

    move    $t9, $a0
    jalr    $t9
    move    $a0, $a1            /* delay slot: arg */

    lw      $t1, 12($sp)
    ctc1    $t1, $31
    mtc0    $s0, $12
    sync
    nop
    nop
    nop
    nop

    lw      $s0, 8($sp)
    lw      $ra, 4($sp)
    lw      $sp, 0($sp)
    jr      $ra
    move    $v0, $zero          /* delay slot: return 0 */
    .end me_vorbis_trampoline
