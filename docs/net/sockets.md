# Internet sockets

The Linux socket handlers decode the address family, socket type, flags and user buffers.
An IPv4 socket gets an open file in the process's descriptor table. The kernel checks
egress policy when sending to a destination. [Socket ABI](../../kernel/abi/linux/aarch64/net/socket.c) · [Confinement](../processes/confinement.md)

UDP binds a local port, assigning an ephemeral one when requested. Received datagrams
queue on the destination socket and retain their message boundaries. A connected UDP
socket records its default peer. ICMP echo uses a datagram socket too.
[UDP ports](../../kernel/net/socket/udp.c) · [Datagram queues](../../kernel/net/socket/datagram.c)

[TCP streams](tcp.md) · [Packet routing](ipv4.md) · [Socket options](../../kernel/abi/linux/aarch64/net/options.c) · [Blocking socket I/O](../../kernel/net/socket/wait.c)
