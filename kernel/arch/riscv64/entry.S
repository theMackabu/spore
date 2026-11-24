.bss
.balign 16
.skip 16384
kernel_stack_top:
.text
.globl user_enter
user_enter:
    la t0, trap_entry
    csrw stvec, t0
    la t0, kernel_stack_top
    csrw sscratch, t0
    csrw sepc, a0
    li t0, 0x46122
    csrc sstatus, t0
    mv sp, a1
    la t0, user_freebsd
    lbu t0, 0(t0)
    beqz t0, 1f
    mv ra, a0
    mv a0, sp
    j 2f
1:  li ra, 0
    li a0, 0
2:
    li gp, 0
    li tp, 0
    li s0, 0
    li s1, 0
    li s2, 0
    li s3, 0
    li s4, 0
    li s5, 0
    li s6, 0
    li s7, 0
    li s8, 0
    li s9, 0
    li s10, 0
    li s11, 0
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a5, 0
    li a6, 0
    li a7, 0
    li t0, 0
    li t1, 0
    li t2, 0
    li t3, 0
    li t4, 0
    li t5, 0
    li t6, 0
    sfence.vma
    fence.i
    sret

.balign 4
trap_entry:
    csrrw sp, sscratch, sp
    addi sp, sp, -128
    sd ra, 0(sp)
    sd t0, 8(sp)
    sd t1, 16(sp)
    sd t2, 24(sp)
    sd t3, 32(sp)
    sd t4, 40(sp)
    sd t5, 48(sp)
    sd t6, 56(sp)
    sd a1, 64(sp)
    sd a2, 72(sp)
    sd a3, 80(sp)
    sd a4, 88(sp)
    sd a5, 96(sp)
    sd a6, 104(sp)
    sd a7, 112(sp)
    csrr t0, scause
    li t1, 8
    bne t0, t1, fault
    csrr t0, sepc
    addi t0, t0, 4
    csrw sepc, t0
    mv a4, a3
    mv a3, a2
    mv a2, a1
    mv a1, a0
    mv a0, a7
    la t0, user_freebsd
    lbu t0, 0(t0)
    beqz t0, 1f
    ld a0, 8(sp)
1:  call user_syscall
    la t0, user_freebsd
    lbu t0, 0(t0)
    beqz t0, 3f
    sltz t0, a0
    sd t0, 8(sp)
    bnez t0, 2f
    sd zero, 64(sp)
    j 3f
2:  neg a0, a0
3:
    ld ra, 0(sp)
    ld t0, 8(sp)
    ld t1, 16(sp)
    ld t2, 24(sp)
    ld t3, 32(sp)
    ld t4, 40(sp)
    ld t5, 48(sp)
    ld t6, 56(sp)
    ld a1, 64(sp)
    ld a2, 72(sp)
    ld a3, 80(sp)
    ld a4, 88(sp)
    ld a5, 96(sp)
    ld a6, 104(sp)
    ld a7, 112(sp)
    addi sp, sp, 128
    csrrw sp, sscratch, sp
    sret
fault:
    li a1, 11
    li t1, 2
    bne t0, t1, 1f
    li a1, 4
1:  li a0, 83
    call user_finish
