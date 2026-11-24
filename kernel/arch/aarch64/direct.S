.section .text.direct, "ax"
.macro address reg, symbol
    adrp \reg, \symbol
    add \reg, \reg, :lo12:\symbol
.endm

.equ TCR_4K_48BIT_VA_40BIT_PA, 0x2b5103510
.global direct_start
.type direct_start, %function
direct_start:
    msr daifset, #15
    msr spsel, #1
    address x0, __bss_start
    address x1, __bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:  address x0, direct_stack_top
    mov sp, x0
    bl direct_build_tables
    address x0, direct_low_root
    msr ttbr0_el1, x0
    address x0, direct_high_root
    msr ttbr1_el1, x0
    mov x0, #0xff
    msr mair_el1, x0
    ldr x0, =TCR_4K_48BIT_VA_40BIT_PA
    msr tcr_el1, x0
    dsb sy
    isb
    tlbi vmalle1
    dsb sy
    isb
    mrs x0, sctlr_el1
    orr x0, x0, #1
    orr x0, x0, #4
    orr x0, x0, #0x1000
    msr sctlr_el1, x0
    isb
    ldr x0, =direct_high
    br x0
direct_high:
    address x0, direct_stack_top
    mov sp, x0
    address x0, direct_empty_root
    ldr x1, =direct_start
    sub x0, x0, x1
    mov x1, #0x40200000
    add x0, x0, x1
    msr ttbr0_el1, x0
    isb
    tlbi vmalle1
    dsb sy
    isb
    bl direct_boot_info
    bl kernel_main
3:  wfe
    b 3b

.section .bss, "aw", %nobits
.balign 16
.skip 65536
direct_stack_top:
