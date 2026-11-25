.text
.global _start
_start:
    ldr x1, [sp, #16]
    ldrb w1, [x1]
    cmp w1, #'b'
    b.eq benchmark
    cmp w1, #'u'
    b.eq unknown
    cmp w1, #'l'
    b.eq loop
    cmp w1, #'x'
    b.eq noexec

    // The access at offset 4094 spans two distinct guest pages.
    mov x0, #0
    mov x1, #8192
    mov x2, #3
    mov x3, #0x22
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    tbnz x0, #63, fail
    add x1, x0, #4094
    mov x2, #-2
    str x2, [x1]
    ldr w3, [x1]
    adds w3, w3, #2
    b.ne fail
    b.cc fail
    cbnz x3, fail
    ldr x3, [x1]
    cmp x3, x2
    b.ne fail

    ldr x3, [sp, #16]
    ldrb w3, [x3]
    cmp w3, #'p'
    b.ne ok
    mov x1, #8192
    mov x2, #1
    mov x8, #226
    mov x9, x0
    svc #0
    cbnz x0, fail
    strb wzr, [x9]
    b fail

benchmark:
    mov x0, #0x400000
1:  add x1, x1, #7
    eor x2, x2, x1
    subs x0, x0, #1
    b.ne 1b
    b ok
unknown: .inst 0
loop: b loop
noexec: mov x1, sp
    br x1
ok: mov x0, #42
    b exit
fail: mov x0, #1
exit: mov x8, #93
    svc #0
