.macro save_registers
    push %r15
    push %r14
    push %r13
    push %r12
    push %rbp
    push %rbx
    push %r11
    push %rcx
    push %rdi
    push %rsi
    push %rdx
    push %r8
    push %r9
    push %r10
    push %rax
.endm
.macro restore_registers
    pop %rax
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rcx
    pop %r11
    pop %rbx
    pop %rbp
    pop %r12
    pop %r13
    pop %r14
    pop %r15
.endm
.bss
.balign 16
.skip 65536
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
    pushq $0x202
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
    save_registers
    mov %rsp, %rdi
    call port_syscall
    restore_registers
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
.global page_fault_entry
page_fault_entry:
    cld
    testb $3, 16(%rsp)
    jz kernel_fault
    push %rax
    mov 8(%rsp), %rax
    mov %rax, fault_error(%rip)
    pop %rax
    add $8, %rsp
    save_registers
    mov %rsp,%rdi
    mov %cr2,%rsi
    xor %edx,%edx
    testq $2, fault_error(%rip)
    jz 1f
    mov $1,%edx
1:  testq $16, fault_error(%rip)
    jz 2f
    mov $2,%edx
2:  call port_page_fault
    restore_registers
    iretq
kernel_fault:
    lea kernel_stack_top(%rip),%rsp
    mov $66,%edi
    mov $1,%esi
    call user_finish
.bss
.balign 8
fault_error: .quad 0

.text
.global timer_irq_entry
timer_irq_entry:
    cld
    testb $3,8(%rsp)
    jz kernel_timer
    save_registers
    mov %rsp,%rdi
    call port_timer_interrupt
    restore_registers
    iretq
kernel_timer:
    save_registers
    mov %rsp,%rbp
    and $-16,%rsp
    xor %edi,%edi
    call port_timer_interrupt
    mov %rbp,%rsp
    restore_registers
    iretq
