# CPUs and interrupts

The boot CPU discovers the other CPUs from Limine and starts them parked. Once the first
process exists, it releases them to install exception vectors, initialize their timers and
schedule threads. Each CPU tracks its current thread. A shared kernel lock serializes
kernel work across CPUs. [CPU startup](../../kernel/arch/aarch64/smp.c) · [Scheduling](../processes/threads.md)

User code runs at EL0. Exception entry saves its registers in a trap frame. A supervisor
call enters the [Linux ABI](abi.md). An instruction or data abort goes to the
[page fault handler](../memory/faults.md). Timer and serial interrupts take the IRQ path.
[Exception vectors](../../kernel/arch/aarch64/vectors.S) · [Exception dispatch](../../kernel/arch/aarch64/exceptions.c) · [Interrupt controller and timer](../../kernel/arch/aarch64/timer.c)
