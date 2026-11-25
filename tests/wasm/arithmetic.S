.text
.global _start
_start:
    mov x0, #-1
    mov x1, #1
    mov x2, #0x8000000000000000
    mov x3, #0x80000000
    mov x4, #0
    mov w5, #4096
loop:
    adds x6, x0, x1
    eor x4, x4, x6
    add x4, x4, x2, lsr #21
    b.cc nocarry
    add x4, x4, #9
nocarry:
    subs w7, w3, w1
    eor x4, x4, x7, ror #13
    b.vc nooverflow
    eor x4, x4, x2
nooverflow:
    add x0, x6, x2, asr #5
    sub x1, x4, x3, lsr #3
    movk x2, #0x8417, lsl #32
    eon x2, x2, x1, lsl #3
    madd w3, w1, w3, w4
    tst w4, w3
    b.eq next
    msub x4, x1, x6, x4
next:
    subs w5, w5, #1
    b.ne loop
    sub sp, sp, #16
    mov x0, #16
    mov x1, sp
hex:
    and w3, w4, #15
    add w3, w3, #65
    strb w3, [x1], #1
    ror x4, x4, #4
    subs x0, x0, #1
    b.ne hex
    mov x0, #1
    mov x1, sp
    mov x2, #16
    mov x8, #64
    svc #0
    mov x0, #42
    mov x8, #93
    svc #0
