# Clocks, system information and randomness

`clock_gettime` uses the architectural counter for monotonic time and the boot epoch for wall
time. Process and thread CPU clocks report charged ticks. Sleep calls put a thread on a timed
wait. [Clocks](../../kernel/abi/linux/shared/time/clock.c) · [Sleep](../../kernel/abi/linux/shared/time/sleep.c) · [Scheduling](../processes/threads.md)

`uname` returns the configured system name, hostname and release. `sysinfo` reports uptime,
process count and memory totals. [System information](../../kernel/abi/linux/shared/system/info.c) · [Procfs](../files/procfs.md)

`getrandom`, `/dev/random` and `/dev/urandom` draw from the kernel's ChaCha generator.
Startup mixes in boot values, the CPU counter and hardware random words when RNDR succeeds.
The generator rekeys after producing output. [Generator](../../kernel/lib/random.c) · [Hardware entropy](../../kernel/arch/aarch64/entropy.c)

## Linux sandbox interfaces

`PR_SET_NO_NEW_PRIVS` survives fork, thread creation and exec. Spore tracks Linux effective,
permitted, inheritable and bounding capability sets. The Linux syscall entry points on
AArch64, x86-64 and RISC-V evaluate seccomp filters before translating syscall numbers.
The supported classic BPF instructions load a word from `seccomp_data`, mask it, jump,
and return allow, errno or kill-process. Spore rejects unsupported filter programs at
installation. [Seccomp](../../kernel/proc/policy/seccomp.c) · [Capabilities](../../kernel/proc/policy/capabilities.c)

A single-threaded unprivileged process with `no_new_privs` and matching real, effective
and saved IDs can create a user namespace and map its own UID and GID once. Writing
`deny` to `/proc/self/setgroups` precedes the GID mapping. A private UTS namespace
isolates hostname and domain name. Spore rejects nested user namespaces and unsupported
namespace combinations. Bubblewrap's mount, PID, IPC and network isolation remain
unimplemented. [User namespaces](../../kernel/proc/lifecycle/user_namespace.c) · [UTS namespaces](../../kernel/proc/lifecycle/uts.c)
