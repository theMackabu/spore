# Delivering a signal

A domain holds signal dispositions and process-directed pending signals. Individual threads
hold their own masks and pending signals. On delivery the kernel checks the disposition.
For a user handler it builds an AArch64 signal frame containing the saved context, then
resumes the thread at the handler. Signal return restores the interrupted context.
[Signal handling](../../kernel/proc/signal.c) · [Signal frame](../../kernel/arch/aarch64/signal_frame.c)

A foreground terminal interrupt and an unresolvable memory fault both reach this path.
A fatal signal in the single-app profile goes back to the host as an [app result](../host/results.md).
[Terminal input](../kernel/terminal.md) · [Page faults](../memory/faults.md)
