.text
.global _start
_start:
#ifdef FAULT
    ud2
#endif
    cmpq $2, (%rsp)
    jne fail
    mov 16(%rsp), %rsi
    mov $1, %edi
    mov $4, %edx
    mov $1, %eax
    syscall
    cmp $4, %rax
    jne fail
    mov $42, %edi
    jmp exit
fail:
    mov $1, %edi
exit:
    mov $60, %eax
    syscall
