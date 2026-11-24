.text
.global _start
_start:
    ldr x9, [x0]
    cmp x9, #2
    b.ne fail
    mov x8, #999
    svc #0
    b.cc fail
    cmp x0, #78
    b.ne fail
    mov x8, #4
    mov x0, #1
    adr x1, msg
    mov x2, #11
    svc #0
    b.cs fail
    cmp x0, #11
    b.ne fail
    mov x0, #42
    b exit
fail: mov x0, #1
exit: mov x8, #1
    svc #0
msg: .ascii "FreeBSD OK\n"
