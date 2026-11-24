# Terminals

The serial TTY connects the guest shell to the host terminal. In canonical mode it gathers
input into lines and handles editing and echo. With signal processing enabled, Ctrl-C
sends an interrupt to the foreground process group. [TTY input](../../kernel/tty/tty.c) · [Signal delivery](../processes/signals.md)

A program can also open `/dev/ptmx` to allocate a pseudoterminal. Its peer opens the
corresponding `/dev/pts/N` slave. The two ends buffer input and output and wake blocked
readers or writers as data or space becomes available. [PTY implementation](../../kernel/tty/pty.c) · [Terminal ioctl handlers](../../kernel/abi/linux/shared/fd/ioctl.c)

[Guest shell](../userland/shell.md) · [Device nodes](../files/ramfs.md)
