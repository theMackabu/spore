.bss
.balign 16
.skip 65536
.global kernel_stack_top
kernel_stack_top:
.text
.globl port_return_to_user
port_return_to_user:
    addi sp,a1,-256
    mv t1,sp
    li t2,32
1:  ld t0,0(a0)
    sd t0,0(t1)
    addi a0,a0,8
    addi t1,t1,8
    addi t2,t2,-1
    bnez t2,1b
    li t0,256
    csrc sstatus,t0
    li t0,32
    csrs sstatus,t0
    j restore_frame

.balign 4
.global trap_entry
trap_entry:
    csrrw sp, sscratch, sp
    bgez sp,supervisor_trap
    addi sp, sp, -256
    sd ra, 0(sp)
    sd t0, 8(sp)
    sd t1, 16(sp)
    sd t2, 24(sp)
    sd t3, 32(sp)
    sd t4, 40(sp)
    sd t5, 48(sp)
    sd t6, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s0, 128(sp)
    sd s1, 136(sp)
    sd s2, 144(sp)
    sd s3, 152(sp)
    sd s4, 160(sp)
    sd s5, 168(sp)
    sd s6, 176(sp)
    sd s7, 184(sp)
    sd s8, 192(sp)
    sd s9, 200(sp)
    sd s10, 208(sp)
    sd s11, 216(sp)
    sd gp, 224(sp)
    sd tp, 232(sp)
    csrr t0, sscratch
    sd t0, 240(sp)
    ld t0,256(sp)
    csrw sscratch,t0
    csrr t0, sepc
    sd t0, 248(sp)
    csrr t0, scause
    bltz t0,timer_irq
    li t1, 8
    bne t0, t1, fault
    csrr t0, sepc
    addi t0, t0, 4
    sd t0, 248(sp)
    mv a0, sp
    call port_syscall
2:
restore_frame:
    ld t0, 248(sp)
    csrw sepc, t0
    ld t0, 240(sp)
    csrw sscratch, t0
    ld gp, 224(sp)
    ld tp,232(sp)
    ld s0, 128(sp)
    ld s1, 136(sp)
    ld s2, 144(sp)
    ld s3, 152(sp)
    ld s4, 160(sp)
    ld s5, 168(sp)
    ld s6, 176(sp)
    ld s7, 184(sp)
    ld s8, 192(sp)
    ld s9, 200(sp)
    ld s10, 208(sp)
    ld s11, 216(sp)
    ld ra, 0(sp)
    ld t0, 8(sp)
    ld t1, 16(sp)
    ld t2, 24(sp)
    ld t3, 32(sp)
    ld t4, 40(sp)
    ld t5, 48(sp)
    ld t6, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    addi sp, sp, 256
    csrrw sp, sscratch, sp
    sret
supervisor_trap:
    csrrw sp,sscratch,sp
    tail platform_poweroff
fault:
    li a2, 0
    li t1, 13
    beq t0,t1,page_fault
    li a2, 1
    li t1, 15
    beq t0,t1,page_fault
    li a2, 2
    li t1, 12
    beq t0,t1,page_fault
    li a1, 11
    li t1, 2
    bne t0, t1, 1f
    li a1, 4
1:  mv a0,sp
    ld a2,248(sp)
    call port_user_exception
    j restore_frame

page_fault:
    mv a0,sp
    csrr a1,stval
    call port_page_fault
    j 2b

timer_irq:
    mv a0,sp
    call port_timer_interrupt
    j 2b

.globl riscv_save_fp, riscv_restore_fp
riscv_save_fp:
    fsd f0,0(a0)
    fsd f1,8(a0)
    fsd f2,16(a0)
    fsd f3,24(a0)
    fsd f4,32(a0)
    fsd f5,40(a0)
    fsd f6,48(a0)
    fsd f7,56(a0)
    fsd f8,64(a0)
    fsd f9,72(a0)
    fsd f10,80(a0)
    fsd f11,88(a0)
    fsd f12,96(a0)
    fsd f13,104(a0)
    fsd f14,112(a0)
    fsd f15,120(a0)
    fsd f16,128(a0)
    fsd f17,136(a0)
    fsd f18,144(a0)
    fsd f19,152(a0)
    fsd f20,160(a0)
    fsd f21,168(a0)
    fsd f22,176(a0)
    fsd f23,184(a0)
    fsd f24,192(a0)
    fsd f25,200(a0)
    fsd f26,208(a0)
    fsd f27,216(a0)
    fsd f28,224(a0)
    fsd f29,232(a0)
    fsd f30,240(a0)
    fsd f31,248(a0)
    frcsr t0
    sd t0,256(a0)
    ret
riscv_restore_fp:
    fld f0,0(a0)
    fld f1,8(a0)
    fld f2,16(a0)
    fld f3,24(a0)
    fld f4,32(a0)
    fld f5,40(a0)
    fld f6,48(a0)
    fld f7,56(a0)
    fld f8,64(a0)
    fld f9,72(a0)
    fld f10,80(a0)
    fld f11,88(a0)
    fld f12,96(a0)
    fld f13,104(a0)
    fld f14,112(a0)
    fld f15,120(a0)
    fld f16,128(a0)
    fld f17,136(a0)
    fld f18,144(a0)
    fld f19,152(a0)
    fld f20,160(a0)
    fld f21,168(a0)
    fld f22,176(a0)
    fld f23,184(a0)
    fld f24,192(a0)
    fld f25,200(a0)
    fld f26,208(a0)
    fld f27,216(a0)
    fld f28,224(a0)
    fld f29,232(a0)
    fld f30,240(a0)
    fld f31,248(a0)
    ld t0,256(a0)
    fscsr t0
    ret
