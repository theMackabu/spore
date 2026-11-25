.arch armv8.1-a+lse
.text
.global _start
_start:
    sub sp, sp, #16
    mov x5, sp
    mov x2, #4
    mov x3, #9
    stp x2,x3,[x5]
    mov x0,#4
    mov x1,#9
    mov x2,#6
    mov x3,#11
    caspal x0,x1,x2,x3,[x5]
    cmp x0,#4
    b.ne fail
    cmp x1,#9
    b.ne fail
    ldaxp x0,x1,[x5]
    cmp x0,#6
    b.ne fail
    cmp x1,#11
    b.ne fail
    add x2,x0,x1
    stlxp w4,x2,x1,[x5]
    cbnz w4,fail
    mov x3,#25
    ldaddal x3,x4,[x5]
    cmp x4,#17
    b.ne fail
    mov w0,#55
    mov w1,#100
    casab w0,w1,[x5]
    cmp w0,#42
    b.ne fail
    ldr x0,[x5]
    b exit
fail:
    mov x0,#1
exit:
    mov x8,#93
    svc #0
