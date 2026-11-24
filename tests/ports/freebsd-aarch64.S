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
    mov x3, #0x1002
    bl map_page
    mov x3, #0x1002
    movk x3, #0x1500, lsl #16
    bl map_page
    tst x0, #0x1fffff
    b.ne fail
    str x3, [x0]
    ldr x9, [x0]
    cmp x9, x3
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
map_page:
    mov x0, #0
    mov x1, #4096
    mov x2, #3
    mov x4, #-1
    mov x5, #0
    mov x8, #477
    svc #0
    b.cs fail
    ret
msg: .ascii "FreeBSD OK\n"
