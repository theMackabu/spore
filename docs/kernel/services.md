# Clocks, system information and randomness

`clock_gettime` uses the architectural counter for monotonic time and the boot epoch for wall
time. Process and thread CPU clocks report charged ticks. Sleep calls put a thread on a timed
wait. [Clocks](../../kernel/abi/linux/aarch64/time/clock.c) · [Sleep](../../kernel/abi/linux/shared/time/sleep.c) · [Scheduling](../processes/threads.md)

`uname` returns the configured system name, hostname and release. `sysinfo` reports uptime,
process count and memory totals. [System information](../../kernel/abi/linux/shared/system/info.c) · [Procfs](../files/procfs.md)

`getrandom`, `/dev/random` and `/dev/urandom` draw from the kernel's ChaCha generator.
Startup mixes in boot values, the CPU counter and hardware random words when RNDR succeeds.
The generator rekeys after producing output. [Generator](../../kernel/lib/random.c) · [Hardware entropy](../../kernel/arch/aarch64/entropy.c)
