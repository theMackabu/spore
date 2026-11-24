.text
.global _start
_start:
#ifdef FAULT
    unimp
#endif
    ld t0, 0(sp)
    li t1, 2
    bne t0, t1, fail
    ld a1, 16(sp)
    li a0, 1
    li a2, 4
    li a7, 64
    ecall
    li t0, 4
    bne a0, t0, fail
    li a0, 42
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
