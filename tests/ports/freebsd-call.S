.text
.global freebsd_call
freebsd_call:
#ifdef __aarch64__
    mov x8,x6
    svc #0
    cneg x0,x0,cs
#elif defined(__x86_64__)
    mov 8(%rsp),%rax
    mov %rcx,%r10
    syscall
    jnc 1f
    neg %rax
1:
#else
    mv t0,a6
    ecall
    beqz t0,1f
    neg a0,a0
1:
#endif
    ret
