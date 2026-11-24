# Reading live state through /proc

Reading `/proc/self/status` resolves `self` to the caller's PID and formats the process's
current state. Numeric PID directories expose entries such as `cmdline`, `cwd`, `exe`,
`fd` and `task`. Thread directories use the owning process and a live thread ID.
[Path lookup](../../kernel/fs/procfs/lookup.c) · [Process reads](../../kernel/fs/procfs/read.c)

System-wide entries include `/proc/meminfo`, `/proc/cpuinfo`, `/proc/mounts` and
`/proc/kmsg`. Programs such as `ps` and `htop` can inspect this state using ordinary file
operations. [Proc formatting](../../kernel/fs/procfs/format.c) · [Boot entries](../../kernel/fs/ramfs/boot.c)

[Process accounting](../../kernel/proc/sched/accounting.c) · [Filesystem routing](vfs.md)
