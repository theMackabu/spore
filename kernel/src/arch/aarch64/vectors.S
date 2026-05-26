.section .text.vectors, "ax"
.arch_extension fp
.align 11
.global vectors
.type vectors, %function

.macro vector_slot target
    b \target
    .balign 0x80
.endm

.macro save_trap_frame
    sub sp, sp, #288
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, sp_el0
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, spsr_el1
    str x0, [sp, #264]
    mrs x0, esr_el1
    str x0, [sp, #272]
.endm

.macro restore_trap_frame
    ldr x0, [sp, #248]
    msr sp_el0, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
    ldr x0, [sp, #264]
    msr spsr_el1, x0

    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #288
.endm

vectors:
    vector_slot unhandled_exception
    vector_slot ignored_exception
    vector_slot ignored_exception
    vector_slot ignored_exception
    vector_slot unhandled_exception
    vector_slot current_irq_exception
    vector_slot ignored_exception
    vector_slot ignored_exception
    vector_slot lower_sync_exception
    vector_slot lower_irq_exception
    vector_slot ignored_exception
    vector_slot ignored_exception
    vector_slot unhandled_exception
    vector_slot ignored_exception
    vector_slot ignored_exception
    vector_slot ignored_exception

lower_sync_exception:
    save_trap_frame
    mov x0, sp
    bl handle_lower_sync
    restore_trap_frame
    eret

current_irq_exception:
    save_trap_frame
    mov x0, sp
    mov x1, #0
    bl handle_irq
    restore_trap_frame
    eret

lower_irq_exception:
    save_trap_frame
    mov x0, sp
    mov x1, #1
    bl handle_irq
    restore_trap_frame
    eret

unhandled_exception:
    bl handle_unhandled_exception
1:
    wfe
    b 1b

ignored_exception:
    eret

.global enter_el0
.type enter_el0, %function
enter_el0:
    msr elr_el1, x0
    msr sp_el0, x1
    mov x2, #0x340
    msr spsr_el1, x2
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x6, #0
    mov x7, #0
    mov x8, #0
    isb
    eret

.global switch_stack_and_finish
.type switch_stack_and_finish, %function
switch_stack_and_finish:
    mov x5, #1
    msr spsel, x5
    isb
    mov sp, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    bl finish_enter_el0
1:
    wfe
    b 1b

.global arch_fp_save
.type arch_fp_save, %function
arch_fp_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    str x1, [x0, #512]
    mrs x1, fpsr
    str x1, [x0, #520]
    ret

.global arch_fp_restore
.type arch_fp_restore, %function
arch_fp_restore:
    ldr x1, [x0, #512]
    msr fpcr, x1
    ldr x1, [x0, #520]
    msr fpsr, x1
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ret
