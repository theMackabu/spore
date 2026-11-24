# TCP connections

A listening socket tracks half-open connections while the handshake completes. A completed
connection enters the listener's ready queue and wakes an accept waiter. Accept installs
the connected socket in the caller's descriptor table. [Handshake](../../kernel/net/socket/tcp/handshake.c) · [Listen and accept](../../kernel/net/socket/tcp/listener.c)

TCP keeps sent bytes until the peer acknowledges them. On receive, out-of-order segments
wait until the missing bytes arrive so the application reads a contiguous stream. Round-trip
samples update the retransmission timeout, and repeated retries back off. [Transmit](../../kernel/net/socket/tcp/transmit.c) · [Reordering](../../kernel/net/socket/tcp/reorder.c) · [Retransmission timing](../../kernel/net/socket/tcp/rto.c)

[Stream tests](../../tests/test_tcp_stream.c) · [Internet sockets](sockets.md)
