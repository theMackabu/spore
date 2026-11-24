.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    mov x1, sp
    msr spsel, #1
    mov sp, x1
    bl limine_boot_info
    bl kernel_main
1:
    wfe
    b 1b
