# Linux system calls

AArch64, RV64 and x86-64 feed a common Linux dispatcher. CPU entry code reads the guest registers and translates architecture-specific syscall numbers and argument layouts. The dispatcher applies process policy and calls the same handlers for memory, file descriptors, processes and sockets. It returns `EPERM` for a denied call and records unknown calls before returning `ENOSYS`. [Dispatcher](../../kernel/abi/linux/shared/dispatch.c) · [Guest support](../ports.md)

| Guest operation | Kernel subsystem |
| --- | --- |
| Open a path or read a descriptor | [VFS](../files/vfs.md) and [descriptor I/O](../processes/descriptors.md) |
| Map memory or execute an ELF | [Virtual memory](../memory/faults.md) and [loader](elf.md) |
| Clone, signal or wait for a task | [Processes](../processes/lifecycle.md) |
| Send a message or wait for a socket | [Sockets](../net/sockets.md) and [readiness](../processes/events.md) |
| Read clocks, system information or random bytes | [Time and kernel services](services.md) |

Spore-specific calls cover snapshots, policy, CPU budgets, shutdown and network configuration.
[Linux syscall numbers by CPU](../../kernel/abi/linux/syscalls.def) · [Spore call numbers](../../kernel/abi/linux/shared/numbers.h) · [Confinement](../processes/confinement.md)
