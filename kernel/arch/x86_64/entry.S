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
.text
.global port_return_to_user
port_return_to_user:
    mov %rdi,%rsp
    restore_registers
    swapgs
    iretq
.global nmi_entry,spurious_entry
nmi_entry:
    save_registers
    mov %rsp,%rbp
    and $-16,%rsp
    call x86_tlb_interrupt
    mov %rbp,%rsp
    restore_registers
spurious_entry:
    iretq
.global syscall_entry
syscall_entry:
    swapgs
    mov %rsp,%gs:8
    mov %gs:0,%rsp
    pushq $0x1b
    pushq %gs:8
    push %r11
    pushq $0x23
    push %rcx
    save_registers
    mov %rsp, %rdi
    call port_syscall
    restore_registers
    swapgs
    iretq
.global fault_entry, fault_error_entry, illegal_entry, divide_entry
fault_error_entry:
    add $8,%rsp
fault_entry:
    pushq $11
    jmp user_exception
illegal_entry:
    pushq $4
    jmp user_exception
divide_entry:
    pushq $8
user_exception:
    cld
    testb $3,16(%rsp)
    jz kernel_fault
    swapgs
    xchg %rax,(%rsp)
    mov %rax,%gs:8
    pop %rax
    save_registers
    mov %rsp,%rdi
    mov %gs:8,%rsi
    mov 120(%rsp),%rdx
    call port_user_exception
    restore_registers
    swapgs
    iretq
.global page_fault_entry
page_fault_entry:
    cld
    testb $3, 16(%rsp)
    jz kernel_fault
    swapgs
    xchg %rax,(%rsp)
    mov %rax,%gs:8
    pop %rax
    save_registers
    mov %rsp,%rdi
    mov %cr2,%rsi
    xor %edx,%edx
    testq $2,%gs:8
    jz 1f
    mov $1,%edx
1:  testq $16,%gs:8
    jz 2f
    mov $2,%edx
2:  call port_page_fault
    restore_registers
    swapgs
    iretq
kernel_fault:
    jmp platform_poweroff
.text
.global timer_irq_entry
timer_irq_entry:
    cld
    testb $3,8(%rsp)
    jz kernel_timer
    swapgs
    save_registers
    mov %rsp,%rdi
    call port_timer_interrupt
    restore_registers
    swapgs
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
