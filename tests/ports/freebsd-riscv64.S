.text
.global _start
_start:
    ld t1, 0(a0)
    li t2, 2
    bne t1, t2, fail
    ld s0, 16(a0)
    li t0, 999
    ecall
    beqz t0, fail
    li t1, 78
    bne a0, t1, fail
    mv a1, s0
    li a0, 1
    li a2, 4
    li t0, 4
    ecall
    bnez t0, fail
    li t1, 4
    bne a0, t1, fail
    call freebsd_services
    bnez a0,fail
    li a0, 42
    j exit
fail:
    li a0, 1
exit:
    li t0, 1
    ecall

.section .note.tag,"a",@note
.balign 4
.long 8, 4, 1
.asciz "FreeBSD"
.long 1501000
