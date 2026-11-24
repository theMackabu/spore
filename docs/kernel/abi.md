# Linux AArch64 system calls

When a guest program issues a supervisor call, Spore reads its syscall number and arguments
from the saved registers. The dispatcher checks the process policy, calls the handler and
returns its result to user space. A denied call returns `EPERM`. An unknown call is counted
and returns `ENOSYS`. [Dispatcher](../../kernel/abi/linux/aarch64/syscall.c) · [User buffer checks](../../kernel/abi/linux/aarch64/usercopy.c)

| Guest operation | Kernel subsystem |
| --- | --- |
| Open a path or read a descriptor | [VFS](../files/vfs.md) and [descriptor I/O](../processes/descriptors.md) |
| Map memory or execute an ELF | [Virtual memory](../memory/faults.md) and [loader](elf.md) |
| Clone, signal or wait for a task | [Processes](../processes/lifecycle.md) |
| Send a message or wait for a socket | [Sockets](../net/sockets.md) and [readiness](../processes/events.md) |
| Read clocks, system information or random bytes | [Time and kernel services](services.md) |

Spore-specific calls cover snapshots, policy, CPU budgets, shutdown and network configuration.
[Call numbers](../../kernel/abi/linux/aarch64/numbers.h) · [Confinement](../processes/confinement.md)
