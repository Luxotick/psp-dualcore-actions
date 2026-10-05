.set noreorder
.set noat

.text
.align 2
.globl me_trampoline
.type me_trampoline, @function
me_trampoline:
    lui     $sp, 0x041f
    ori     $sp, $sp, 0xfff0
    lui     $t0, 0x0000
    ori     $t0, $t0, 0x2000
    mtc0    $t0, $12
    nop
    la      $t9, me_loop
    jr      $t9
    nop
.size me_trampoline, .-me_trampoline
