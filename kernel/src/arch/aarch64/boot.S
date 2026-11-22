.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    // Keep Limine's stack, but select the EL1 stack before entering C.
    mov x1, sp
    msr spsel, #1
    mov sp, x1
    bl kernel_main
1:
    wfe
    b 1b
