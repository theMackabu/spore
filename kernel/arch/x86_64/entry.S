.bss
.balign 16
.skip 16384
.global kernel_stack_top
kernel_stack_top:
user_sp: .quad 0
.text
.global enter_ring3
enter_ring3:
    lgdt (%rdx)
    mov $0x28, %ax
    ltr %ax
    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es
    pushq $0x1b
    mov %rsi, %rax
    cmpb $0, user_freebsd(%rip)
    je 1f
    sub $8, %rax
1:  push %rax
    pushq $2
    pushq $0x23
    push %rdi
    xor %eax, %eax
    xor %ecx, %ecx
    xor %edx, %edx
    xor %ebx, %ebx
    xor %ebp, %ebp
    xor %edi, %edi
    cmpb $0, user_freebsd(%rip)
    cmovne %rsi, %rdi
    xor %esi, %esi
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    iretq
.global syscall_entry
syscall_entry:
    mov %rsp, user_sp(%rip)
    lea kernel_stack_top(%rip), %rsp
    pushq $0x1b
    pushq user_sp(%rip)
    push %r11
    pushq $0x23
    push %rcx
    push %rdi
    push %rsi
    push %rdx
    push %r8
    push %r9
    push %r10
    push %rax
    mov %rsp, %rdi
    cmpb $0, user_freebsd(%rip)
    jne 1f
    call linux_x86_64_syscall
    jmp 2f
1:  call freebsd_x86_64_syscall
2:  pop %rax
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rsi
    pop %rdi
    iretq
.global fault_entry, illegal_entry, divide_entry
divide_entry:
    mov $8, %esi
    jmp finish
illegal_entry:
    mov $4, %esi
    jmp finish
fault_entry:
    mov $11, %esi
finish:
    cld
    lea kernel_stack_top(%rip), %rsp
    mov $83, %edi
    call user_finish
