.text
.global _start
_start:
    mov x0, #1
    lsl x0, x0, #22
    msr fpcr, x0
    msr fpsr, xzr
    mov w0, #0x1000000
    add w0, w0, #1
    scvtf s0, w0
    fcvtzs w1, s0
    sub w1, w1, w0
    cmp w1, #1
    b.ne fail
    mrs x0, fpsr
    tbz x0, #4, fail
    msr fpcr, xzr
    fmov d0, #1.5
    fmov d1, #2.5
    fmov d2, #0.25
    fmadd d3, d0, d1, d2
    fmov d4, #4.0
    fcmp d3, d4
    b.ne fail
    fmov v0.2d, #2.0
    movi v1.4h, #1
    addv h2, v1.4h
    umov w0, v2.h[0]
    cmp w0, #4
    b.ne fail
    mov x0, #42
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93
    svc #0
