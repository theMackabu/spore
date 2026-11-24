# Unix-domain sockets

Guest processes can connect to a listener at a filesystem path or create an already
connected pair with `socketpair`. A listener queues connections until accept installs a
descriptor for the server. Mycelium and trustd both use local stream sockets.
[Unix sockets](../../kernel/net/socket/unix.c) · [Mycelium](../userland/mycelium.md) · [TLS verification](../userland/trust.md)

With `SCM_RIGHTS`, the sender passes references to open files in a message. The kernel
retains them while the message is queued and can install descriptors for them in the
receiver's table. [Descriptor passing](../../kernel/net/socket/rights.c) · [Message handling](../../kernel/abi/linux/aarch64/net/unix_msg.c)

[File descriptors](../processes/descriptors.md) · [Guest socket tests](../../userland/tests/integration/unix.c)
