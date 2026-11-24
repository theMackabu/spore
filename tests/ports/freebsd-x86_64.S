.text
.global _start
_start:
    cmpq $2, (%rdi)
    jne fail
    mov 16(%rdi), %rsi
    mov $999, %eax
    syscall
    jnc fail
    cmp $78, %rax
    jne fail
    mov $1, %edi
    mov $4, %edx
    mov $4, %eax
    syscall
    jc fail
    cmp $4, %rax
    jne fail
    mov $42, %edi
    jmp exit
fail:
    mov $1, %edi
exit:
    mov $1, %eax
    syscall
