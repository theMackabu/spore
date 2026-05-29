.section .text.boot, "ax"
.global smp_secondary_entry
.type smp_secondary_entry, %function
smp_secondary_entry:
    mov x20, x0
    cmp x20, #8
    b.hs 3f

    mrs x6, CurrentEL
    lsr x6, x6, #2
    adrp x5, smp_ap_el
    add x5, x5, :lo12:smp_ap_el
    add x5, x5, x20
    strb w6, [x5]
    adrp x5, smp_ap_parked
    add x5, x5, :lo12:smp_ap_parked
    add x5, x5, x20
    mov w6, #1
    strb w6, [x5]
    dsb sy

    adrp x1, smp_secondary_stacks
    add x1, x1, :lo12:smp_secondary_stacks
    mov x2, #0x10000
    madd x1, x20, x2, x1
    add sp, x1, x2

    adrp x1, smp_boot_ttbr_pa
    add x1, x1, :lo12:smp_boot_ttbr_pa
    ldr x2, [x1]
    mov w6, #2
    strb w6, [x5]
    dsb sy
    adrp x1, smp_boot_mair
    add x1, x1, :lo12:smp_boot_mair
    ldr x3, [x1]
    msr mair_el1, x3
    mov w6, #3
    strb w6, [x5]
    dsb sy
    adrp x1, smp_boot_tcr
    add x1, x1, :lo12:smp_boot_tcr
    ldr x3, [x1]
    msr tcr_el1, x3
    mov w6, #4
    strb w6, [x5]
    dsb sy
    msr ttbr0_el1, x2
    msr ttbr1_el1, x2
    mov w6, #5
    strb w6, [x5]
    dsb sy
    isb
    dsb ishst
    tlbi vmalle1
    dsb ish
    isb
    mov w6, #6
    strb w6, [x5]
    dsb sy

    mrs x3, sctlr_el1
    and x4, x3, #1
    adrp x1, smp_ap_sctlr_m
    add x1, x1, :lo12:smp_ap_sctlr_m
    add x1, x1, x20
    strb w4, [x1]
    mov w6, #7
    strb w6, [x5]
    dsb sy
    adrp x1, smp_boot_sctlr
    add x1, x1, :lo12:smp_boot_sctlr
    ldr x3, [x1]
    orr x3, x3, #1
    msr sctlr_el1, x3
    mov w6, #8
    strb w6, [x5]
    dsb sy
    isb
    mov w6, #9
    strb w6, [x5]
    dsb sy

    adrp x1, smp_kernel_va_offset
    add x1, x1, :lo12:smp_kernel_va_offset
    ldr x2, [x1]
    adr x3, 1f
    add x3, x3, x2
    br x3

1:
    adrp x5, smp_ap_parked
    add x5, x5, :lo12:smp_ap_parked
    add x5, x5, x20
    mov w6, #10
    strb w6, [x5]
    dsb sy
    adrp x1, smp_secondary_stacks
    add x1, x1, :lo12:smp_secondary_stacks
    mov x2, #0x10000
    madd x1, x20, x2, x1
    add sp, x1, x2
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
