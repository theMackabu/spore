.section .text.boot, "ax"
.global smp_secondary_entry
.type smp_secondary_entry, %function
smp_secondary_entry:
    mov x21, x0
    cbz x21, 3f
    ldr x20, [x21, #0]
    ldr x19, [x21, #16]
    ldr x1, [x21, #8]
    cbz x1, 3f
    mov sp, x1

    ldr x2, [x21, #24]
    dsb sy
    ldr x3, [x21, #40]
    msr mair_el1, x3
    dsb sy
    ldr x3, [x21, #32]
    msr tcr_el1, x3
    dsb sy
    msr ttbr0_el1, x2
    msr ttbr1_el1, x2
    dsb sy
    isb
    dsb ishst
    tlbi vmalle1
    dsb ish
    isb
    dsb sy

    ldr x3, [x21, #48]
    orr x3, x3, #1
    msr sctlr_el1, x3
    dsb sy
    isb

    ldr x2, [x21, #56]
    adr x3, 1f
    add x3, x3, x2
    br x3

1:
    mov sp, x19
    mov x0, x20
    bl smp_secondary_main

3:
    wfe
    b 3b

.global smp_enter_trap_frame
.type smp_enter_trap_frame, %function
smp_enter_trap_frame:
    mov sp, x0
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
    eret
