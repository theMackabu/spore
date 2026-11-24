# The life of a process

Spore calls the state shared by a process's threads a *domain*. It holds the address space,
open files, credentials, working directory, signal actions and resource limits. Each thread
has its own registers, signal mask and wait state. [Domain layout](../../kernel/include/proc/domain.h) · [Threads](threads.md)

In the interactive image, fork creates a child with a copy-on-write address space. Exec
installs the new program and closes descriptors marked close-on-exec. When a child exits,
its parent can collect the status with `wait4`. The [app profile](../host/results.md) allows threads
within its single process. [Process creation](../../kernel/proc/lifecycle/clone.c) · [Child reaping](../../kernel/proc/lifecycle/child.c)

[Signals](signals.md) · [Snapshots](snapshots.md) · [Credentials and confinement](confinement.md) · [File descriptors](descriptors.md)
