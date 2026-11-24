# Scheduling threads

On a scheduling pass each CPU scans thread slots starting after the current thread. It picks
a runnable thread that is free to run on that CPU and restores its saved context. A blocked
thread records what it is waiting for. Completing that wait installs the result and makes
the thread runnable again. [Scheduler](../../kernel/proc/sched/thread.c) · [Wait states](../../kernel/proc/sched/wait.c)

Timer ticks account for CPU use, enforce domain CPU budgets and wake timed waits. A futex
lets threads in the same domain sleep on a user-space word. A wake matches its address
and bitset, and requeue can move waiters to another address. [Accounting](../../kernel/proc/sched/accounting.c) · [Futexes](../../kernel/proc/sched/futex.c)

[CPU startup](../kernel/cpus.md) · [I/O readiness](events.md) · [Process lifecycle](lifecycle.md)
