# Spore v3 Goal: threads, futex, networking, and live egress policy

## Goal

Turn Spore from a confined interactive single-threaded agent OS into a confined
concurrent agent OS with a real network boundary. Static musl programs should be
able to create pthreads, block and wake on futexes, use minimal sockets, and be
confined by a live egress policy that is enforced before packets leave the
domain.

This goal builds on `own-boot`: QEMU boots through EDK2 into Spore's own
`BOOTAA64.EFI`, then the kernel launches `spsh`. Do not revisit the bootloader
or userland layout except for adding test/demo binaries needed by this goal.

## End state

- Multiple threads per domain work for stock static musl pthread programs.
- `clone(CLONE_VM | CLONE_THREAD | ...)` is supported for the musl pthread case.
- `futex` wait/wake is implemented enough for musl mutexes and condition
  variables.
- Thread exit, `exit_group`, `gettid`, `set_tid_address`, and basic robust-list
  cleanup behave correctly.
- QEMU `virt` networking is live through a small virtio-net path.
- Minimal IPv4 networking works, at least ARP, ICMP, UDP, and the socket syscalls
  required by a static musl UDP client.
- The egress capability field becomes active: a domain can be allowed or denied
  by protocol, destination address/CIDR, and port.
- Existing validations remain green: host tests, `run-tests`, shell/coreutils,
  writable `/tmp`, CWD, and all `/demos` policy fixtures.

## Design direction

Do threads/futex before networking. Networking introduces new blocking I/O and
wakeup paths; those are easier to get right once the scheduler can already park
and wake threads on futex-style wait queues. Keep the kernel single-core and
run-to-completion for this goal. IRQs may set flags and wake waiters, but the
kernel must not become generally preemptible inside syscalls.

Domains remain the isolation and policy unit. Threads are execution units inside
a domain. Address space, VMA list, fd table, filesystem root/CWD, capability set,
memory budget, CPU budget, and egress policy stay domain-owned. A thread owns
only its saved EL0 context, kernel stack, TID, TLS pointer, scheduler state, and
wait reason.

## Phase A: thread model

Extend the current domain/thread split so a domain may own multiple threads.
Implement the pthread-shaped subset of `clone` on AArch64:

- Accept shared address space cases with `CLONE_VM`.
- Support the musl pthread shape: shared VM, shared fd table, child stack, TLS,
  parent/child TID pointers, and clear-child-TID on exit.
- Reject unsupported thread semantics with `-ENOSYS` or `-EINVAL`, rather than
  silently approximating.
- Keep `fork`/CoW behavior unchanged when `CLONE_VM` is not set.

Gate tag: `v3a-threads`

Validation:

- Host tests for thread table allocation, domain thread ownership, and TID
  lifecycle.
- QEMU demo: one domain creates two kernel-scheduled EL0 threads that both print
  and exit.
- Existing `run-tests` still passes unchanged.

## Phase B: futex core

Implement enough `futex` for stock static musl pthread mutexes and condition
variables:

- `FUTEX_WAIT`
- `FUTEX_WAKE`
- `FUTEX_WAIT_PRIVATE`
- `FUTEX_WAKE_PRIVATE`
- timeout handling if musl hits it, otherwise return `-ENOSYS` for unsupported
  commands with a log line.

Futex wait queues are keyed by `(domain, user_va)`. Validate and copy the futex
word through the existing user-copy/faulting path before sleeping. Waking a futex
marks blocked threads runnable; scheduling still occurs only at safe points.

Gate tag: `v3b-futex`

Validation:

- Host tests for futex-key hashing/listing and wake ordering.
- QEMU demo: static musl pthread mutex/condvar program starts N threads, waits
  on a condition variable, wakes all, and joins cleanly.
- Existing fork/CoW and snapshot regression still pass.

## Phase C: thread lifecycle and cancellation-adjacent cleanup

Harden thread exit semantics:

- `exit` exits only the calling thread.
- `exit_group` terminates all threads in the domain.
- `wait4` remains process/domain-oriented; thread joins happen in userland using
  futex clear-child-TID.
- `set_tid_address` and clear-child-TID wake the futex waiters expected by musl.
- `set_robust_list` stores the pointer and performs minimal robust mutex cleanup
  on thread exit; no signal delivery.

Gate tag: `v3c-pthreads`

Validation:

- Stock static musl pthread test creates, joins, and tears down at least 8
  threads.
- A child thread calling `exit` does not kill its domain.
- Main thread calling `exit_group` kills all sibling threads.

## Phase D: virtio-net device bring-up

Bring up virtio-net on QEMU `virt` using virtio-mmio:

- Discover/configure the virtio-net MMIO device.
- Negotiate only the minimal feature set needed for simple RX/TX.
- Allocate descriptor rings and packet buffers.
- Handle RX interrupts or poll at scheduler-safe points as a temporary step, but
  the phase gate should use IRQ-driven wakeup if practical.

Gate tag: `v3d-virtio-net`

Validation:

- QEMU logs show virtio-net feature negotiation and queue setup.
- A kernel-only network smoke test transmits a raw Ethernet frame visible on the
  host capture path or receives one injected by QEMU.
- Existing userland-visible behavior remains unchanged.

## Phase E: tiny IPv4 stack and UDP sockets

Add the smallest network stack that is useful for agents:

- Ethernet II
- ARP
- IPv4
- ICMP echo reply
- UDP
- Blocking receive integrated with the scheduler

Implement minimal socket syscalls for static musl UDP clients:

- `socket`
- `bind`
- `connect`
- `sendto`
- `recvfrom`
- `getsockname` if musl asks
- `setsockopt` stubs for harmless options
- `poll` or `ppoll` only if the demo/client actually needs it

Gate tag: `v3e-udp`

Validation:

- QEMU network harness starts with user-mode networking or a tap backend.
- Guest responds to ping if ICMP is included in the harness.
- Static musl UDP client sends to and receives from a host UDP echo endpoint.
- Blocking `recvfrom` sleeps the thread instead of spinning the domain.

## Phase F: live egress policy

Activate the reserved egress capability field:

- Policy covers protocol, destination CIDR/address, and port range.
- Deny happens before enqueueing a packet or mutating socket state that implies
  external communication.
- Denied syscalls return `-EPERM` and log syscall number, domain, destination,
  and policy reason.
- Child domains may only receive egress permissions that are a subset of the
  parent domain's egress permissions.

Gate tag: `v3f-egress`

Validation:

- `confine net:none ./udp-send HOST PORT hi` fails with `EPERM`.
- `confine net:udp:HOST:PORT ./udp-send HOST PORT hi` succeeds.
- A domain confined to one CIDR cannot send to another.
- A child spawn requesting broader egress than its parent is rejected.
- Shell remains alive after egress denials and budget kills.

## Final gate

Final tag: `v3`

Validation script should demonstrate:

```text
/ $ pthread-demo
threads=8 counter=800000 PASS
/ $ udp-echo HOST PORT hi
udp echo: hi
/ $ confine net:none ./udp-send HOST PORT hi
sendto: Operation not permitted
/ $ confine net:udp:HOST:PORT ./udp-send HOST PORT hi
sent 2 bytes
/ $ confine net:udp:HOST:PORT ./udp-recv HOST PORT
recv: ok
/ $ echo shell alive
shell alive
```

Required regression set:

- `meson compile -C build`
- `meson test -C build`
- `meson compile -C build run-tests`
- Boot to `spsh` through the `own-boot` path.
- Coreutils, redirection, writable `/tmp`, CWD.
- Existing `/demos` policy fixtures.
- v1/v1.5/v2 regression sequence.
- New pthread/futex demo.
- New UDP/network demo.
- New egress allow/deny demo.

## Out of scope

- SMP.
- General kernel preemption inside syscalls.
- Fully lockful kernel conversion.
- Dynamic linking.
- Full POSIX signal delivery and signal frames.
- TCP, TLS, DNS, or an HTTP stack unless UDP proves insufficient for the chosen
  demo.
- Virtio-net performance tuning.
- Real hardware networking.
- Raw `-kernel` boot or removing EDK2.

## Open questions before implementation

- Network backend for the default local harness: QEMU user-mode networking is
  easiest, while tap gives better packet visibility but needs more host setup.
- Whether to include ICMP in the first network gate or keep Phase D raw and Phase
  E UDP-only.
- Whether `poll`/`ppoll` belongs in Phase E or should wait until a concrete musl
  network demo traps on it.
- How strict egress denial should be by default: return `-EPERM` for ordinary
  confined programs, kill in strict manifest mode.
