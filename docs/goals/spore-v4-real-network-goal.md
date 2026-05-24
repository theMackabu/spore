# Spore v4 Goal: real packet networking and packet-level egress policy

## Goal

Replace the v3 UDP demo shortcut with a real packet path. The current v3 state
has useful syscall and policy scaffolding, but UDP payloads are looped through an
fd-backed in-kernel buffer while virtio-net is only proven by a TX smoke frame.
v4 makes networking honest: virtio-net RX/TX queues, Ethernet, ARP, IPv4, ICMP,
UDP to a real host endpoint, then egress policy enforced immediately before a
packet enters the TX queue.

This goal is networking hardening, not a broad userland or process model goal.
The one userland addition is a tiny `/bin/ping` utility to exercise ICMP from
EL0. Threads, futex, filesystem, bootloader, shell, and existing policy
semantics must keep passing unchanged.

## End state

- QEMU virtio-net has working RX and TX queues.
- The kernel receives Ethernet frames from QEMU user networking or a local tap
  harness and dispatches them without spinning.
- ARP resolution works for the configured gateway/host peer.
- IPv4 parses and emits valid headers/checksums.
- ICMP echo requests receive echo replies.
- A static musl `/bin/ping` sends ICMP echo requests and reports replies through
  the normal socket/syscall path.
- UDP sockets send packets to, and receive packets from, a real host UDP echo
  endpoint.
- `recvfrom` blocks the calling thread and wakes on packet arrival.
- Egress policy is checked on the final resolved packet tuple
  `(protocol, dst_ip, dst_port)` immediately before TX enqueue.
- The v3 adversarial egress cases still pass, now over the real packet path.

## Hard boundary

No fake loopback success path. A UDP demo passes only if bytes leave through
virtio-net and return from an external peer. Keeping a small in-kernel loopback
helper for unit tests is fine, but the gate demos must use real packets.

## Architecture direction

Keep the kernel single-core and run-to-completion. IRQ handlers may acknowledge
virtio interrupts, harvest RX buffers, and mark waiters runnable, but they must
not reschedule in the middle of arbitrary EL1 work.

Separate the network stack into small layers:

- `drivers/virtio_net.c`: device, queues, packet buffers, RX refill, TX submit.
- `net/ethernet.c`: Ethernet II framing and dispatch.
- `net/arp.c`: ARP table, requests, replies, timeout/retry policy.
- `net/ipv4.c`: IPv4 parse/build, checksum, local address/gateway config.
- `net/icmp.c`: echo reply.
- `net/udp.c`: socket demux, send, receive queues, waiter wakeups.
- `sys/socket.c` or existing syscall code: ABI glue only.

The socket fd table remains per-domain. Socket packet queues are kernel objects
referenced by fds, inherited on fork through the existing fd refcount rules.

## Harness decision

Default to QEMU user-mode networking first because it is zero-host-setup on
macOS. Use the usual QEMU guest-side addresses:

- guest: `10.0.2.15`
- gateway: `10.0.2.2`
- DNS/reserved for later: QEMU defaults

Add a host UDP echo helper script for the run target, preferably on a fixed port
like `5555`. If QEMU user networking cannot route host-to-guest replies in the
needed shape, add a tap/slirp helper target as an alternate harness, but keep the
default local path macOS-friendly.

## Phase A: virtio-net RX queue

Bring up RX for the existing virtio-mmio net device.

- Negotiate the minimal feature set deliberately.
- Allocate RX descriptors and packet buffers.
- Refill RX buffers after consumption.
- Handle the virtio-net interrupt or poll from scheduler-safe points as an
  intermediate step.
- Expose a tiny kernel counter/log for RX frames and TX frames.

Gate tag: `v4a-virtio-rx`

Validation:

- QEMU logs show RX and TX queue setup.
- Kernel receives at least one host-originated frame.
- Existing `meson test -C build` and `meson compile -C build run-tests` remain
  green.

## Phase B: Ethernet + ARP

Implement Ethernet II dispatch and ARP resolution.

- Parse Ethernet destination/source/type.
- Maintain a tiny ARP table.
- Send ARP requests for unresolved IPv4 next hops.
- Answer ARP requests for the guest IPv4 address.
- Queue or fail UDP sends while ARP is unresolved; keep the behavior explicit.

Gate tag: `v4b-arp`

Validation:

- Guest resolves the QEMU gateway/host peer MAC.
- Kernel logs ARP request/reply flow.
- A host-side packet capture or QEMU log shows correct ARP frames.

## Phase C: IPv4 + ICMP

Implement enough IPv4 to prove bidirectional packet correctness.

- Parse IPv4 headers, lengths, protocol, and checksum.
- Drop malformed or unsupported packets with counters.
- Emit valid IPv4 headers/checksums.
- Reply to ICMP echo requests.
- Add `/bin/ping` as a normal static musl userland tool; it should use the
  kernel's ICMP/socket path rather than a private test hook.

Gate tag: `v4c-ipv4-icmp`

Validation:

- Host can ping the guest if the chosen harness supports it, or an injected ICMP
  echo request receives a valid echo reply.
- Guest `/bin/ping 10.0.2.2` reports at least one successful reply from the
  QEMU gateway/host peer, or the harness documents why guest-originated ICMP is
  not available and runs an equivalent packet-level ICMP echo test.
- Malformed checksum/length unit tests reject bad packets.

## Phase D: real UDP sockets

Replace the fd-backed UDP echo shortcut with UDP packet send/receive.

- `sendto` builds UDP over IPv4 and submits via virtio-net.
- `connect` stores the peer, but does not bypass policy or routing checks.
- `recvfrom` blocks the calling thread when no datagram is queued.
- Incoming UDP packets demux to bound/connected sockets.
- Socket receive queues have bounded capacity and drop/count overflow.

Gate tag: `v4d-real-udp`

Validation:

- Static musl `udp-echo 10.0.2.2 5555 hi` gets `udp echo: hi` from a host
  process, not from in-kernel loopback.
- Blocking `recvfrom` sleeps and wakes on packet arrival.
- Existing v3 pthread/futex/filesystem/policy regressions stay green.

## Phase E: packet-level egress enforcement

Move the authoritative egress check to the final packet send point.

- Check `(protocol, dst_ip, dst_port)` after connect/sendto/routing resolution
  and immediately before TX enqueue.
- Syscall-level checks may remain as early rejection, but they are not trusted as
  the only guard.
- Log denied packet tuple, domain id, fd, syscall origin if known, and reason.
- Keep child capability subset checks.

Gate tag: `v4e-packet-egress`

Validation:

- Allowed `net:udp:10.0.2.2:5555` succeeds to the host echo server.
- Same host, wrong port is denied before TX.
- CIDR in-subnet succeeds and out-of-subnet is denied before TX.
- `connect` to allowed peer then `sendto` explicit forbidden peer is denied.
- `connect` to forbidden peer is denied and does not mutate socket peer state.
- Child requesting egress outside the parent policy is rejected.
- TX counters prove denied attempts do not enqueue packets.

## Phase F: DNS/TCP decision point

Stop after UDP is real and policy is packet-level. Then decide whether to add
DNS and TCP in the same goal or split them.

Default recommendation: split. UDP plus packet-level policy is already a
meaningful security milestone. DNS/TCP will drag in stream state, timeouts,
retransmission, and more socket options.

Gate tag: `v4`

Validation:

```text
/ $ udp-echo 10.0.2.2 5555 hi
udp echo: hi
/ $ confine net:none udp-send 10.0.2.2 5555 hi
sendto: Operation not permitted
/ $ confine net:udp:10.0.2.2:5555 udp-send 10.0.2.2 5555 hi
sent 2 bytes
/ $ confine net:udp:10.0.2.0/24:5555 udp-send 10.0.3.2 5555 hi
sendto: Operation not permitted
/ $ ping 10.0.2.2
64 bytes from 10.0.2.2: icmp_seq=1
/ $ echo shell alive
shell alive
```

Required regression set:

- `meson compile -C build`
- `meson test -C build`
- `meson compile -C build run-tests`
- Boot to `spsh`
- Coreutils, redirection, writable `/tmp`, CWD
- v1/v1.5/v2/v3 regression sequence
- v3 egress pressure suite
- real host UDP echo path
- ICMP/ARP packet smoke
- `/bin/ping` guest-originated ICMP demo

## Fork-latency side quest

Keep this in the goal as instrumentation, not a blocking networking feature.

The current profile after v3 hardening showed roughly:

```text
fork+wait=6453us snapshot-spawn+reap=6166us samples=4
```

Add a repeatable `fork-prof` test mode that records:

- fork clone cost only, separate from wait/reap where possible
- snapshot spawn cost only
- page-table size and resident pages
- timer tick count during each sample
- HVF vs TCG numbers

Do not optimize based on vibes. First make the measurement trustworthy, then
decide whether the cost is page-table walk, scheduler yield/tick, QEMU/HVF, TLB
flush, or logging noise.

## Out of scope

- TCP unless explicitly approved after Phase F.
- DNS unless explicitly approved after Phase F.
- TLS/HTTP.
- Dynamic linking.
- SMP or kernel preemption.
- Real hardware networking.
- Replacing QEMU user networking with a host-specific mandatory tap setup.
- New filesystem or shell features.
