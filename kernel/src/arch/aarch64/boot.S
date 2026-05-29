.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    mrs x1, mpidr_el1
    and x1, x1, #0xff
    cbnz x1, 1f
    bl kernel_main

1:
    wfe
    b 1b
