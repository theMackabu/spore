# Nanos Compatibility and Performance Audit

Date: 2026-05-31

Reference tree: `/tmp/nanos`

This audit uses Nanos as a reference for Linux ABI shape and performance ideas,
not as code to copy. Spore keeps its run-to-completion kernel model and ports
only concepts that fit that model cleanly.

## Summary

Implemented safe wins:

- `brk(2)` grows the heap by extending VMAs and backing pages lazily on fault.
- User-copy helpers copy page-sized chunks instead of byte-by-byte loops.
- `pmm_alloc_page()` scans bitmap words with `ctz`.
- `sysinfo(2)`, `getrusage(2)`, `/proc/procinfo`, `/proc/<pid>/status`,
  `/proc/<pid>/stat`, `/proc/<pid>/statm`, `ps`, and `top` use shared process
  memory accounting for VSZ, RSS, minor faults, and major faults.
- Auxv includes Linux-shaped platform, HWCAP, execfn, credentials, random, and
  `AT_MINSIGSTKSZ` entries.
- PMM allocation counters are exposed through `/proc/meminfo`.
- Unsupported syscall and ioctl probes are counted per process without kernel
  log spam.
- `spore-run --mode bench` and `make benchmark` provide reproducible startup and
  filesystem timing canaries.
- `spore-run --mode rng` and `make rng-smoke` compare `/dev/urandom` samples
  across temporary-root boots.
- `/proc/fsstats` exposes VFS lookup/page-cache and ext2 directory/block-cache
  counters.
- VFS has a conservative positive full-path lookup cache, invalidated on
  filesystem mutation.
- File-backed `mmap`, executable/shared-library paging, and regular file reads
  share the VFS page-cache path.
- Shared file mappings write back through `msync(2)`, `munmap(2)`, and address
  space teardown.
- `MAP_FIXED_NOREPLACE`, `MAP_NORESERVE`, `MAP_GROWSDOWN`, file-backed
  `MAP_SHARED`, and anonymous `MAP_SHARED` are supported at Spore's current ABI
  level.
- `/dev/shm` exists on tmpfs, and `memfd_create(2)` returns tmpfs-backed fds
  that can be truncated, mapped shared, and removed from the namespace while
  still alive through fd/VMA references. `F_ADD_SEALS`/`F_GET_SEALS` enforce
  grow, shrink, write, future-write, and seal-seal semantics.
- Signal delivery supports masks, pending delivery, alt-stack delivery,
  nested handlers, `rt_sigreturn`, and `SA_RESTART` versus `EINTR` for
  restartable blocking waits.
- `/proc/loadavg` reports fixed-point 1/5/15 minute runnable-thread load averages.
- Socket-option coverage is pinned by integration tests and documented in
  `SOCKET_OPTION_AUDIT.md`.

Current verification:

- `meson test -C build --print-errorlogs`
- `meson compile -C build test_image.img spore-run`
- `make run-tests`
- `make run-shell-check`

## Exec and ELF Loading

Nanos maps executable file content through VM/page-cache machinery and treats
auxv/process setup as first-class execution state.

Spore now does the same architectural shape for the relevant paths: exec opens a
vnode, maps or faults file pages from the VFS page cache, handles `PT_INTERP`,
zero-fills BSS, and passes Linux-shaped auxv entries. The remaining future item
is ASLR, which should wait until signal frames and debugging tolerate randomized
layout.

## VMAs, mmap, mremap, brk, and Page Faults

Nanos treats the heap and file mappings as ranges backed on demand. Spore now
does lazy `brk`, file-backed demand faults, bounded multi-page `MAP_GROWSDOWN`
stack growth, `MAP_FIXED_NOREPLACE` collision detection, and `MAP_NORESERVE` as
the Linux-compatible reservation hint it is for this kernel.

`MAP_SHARED` has two supported forms:

- File-backed shared mappings share cached file pages and write dirty pages back
  via `msync`, `munmap`, or process teardown.
- Anonymous shared mappings are eagerly prefaulted and kept shared across `fork`
  by restoring writable shared PTEs after the normal COW clone.
- `/dev/shm` provides tmpfs-backed shared objects for libc `shm_open` style users.
- `memfd_create` accepts `MFD_CLOEXEC` and `MFD_ALLOW_SEALING`, and returns an
  ordinary read/write fd backed by an unlinked tmpfs node. Seals are enforced
  through `fcntl(F_ADD_SEALS/F_GET_SEALS)`, writes/truncates, and future shared
  writable mappings.

Remaining:

- Extend `MAP_GROWSDOWN` to rlimit-aware stack policy if a runtime needs deeper
  automatic stack expansion.
- Promote memfd backing from "unlinked tmpfs node" to a distinct anonymous object
  type if later cleanup/accounting needs that separation.
- Add `MAP_FIXED_NOREPLACE` and `MAP_NORESERVE` host-side model tests in addition
  to the guest integration canaries.

## PMM/VMM Fast Paths

Spore now avoids the two worst slow paths from the original audit: byte-wise
user copies and bit-by-bit page allocation scans. `/proc/meminfo` exposes PMM
allocation attempts, successes, failures, and bitmap words scanned so allocator
regressions are visible.

Remaining:

- Audit contiguous allocation paths separately.
- Consider batching TLB/cache maintenance if profiles show it matters.

## Page Cache, Block, VFS, and ext2

Spore now has three useful observability points: benchmark canaries, `/proc/fsstats`,
and a conservative VFS lookup cache. File reads and file-backed page faults use
the same VFS page-cache loading path, which keeps mmap and regular reads coherent
for dirty shared pages.

Remaining:

- Use `/proc/fsstats` to tune lookup-cache size.
- Decide whether invalidation should become subtree-granular instead of global.
- Add block-cache and directory-iteration host tests once the ext2 harness can
  assert stats without depending on incidental traversal details.

## Scheduler, Wait/Wake, Poll, Select, and Epoll

Spore already has blocking `poll`, `ppoll`, `pselect6`, and `epoll` integrated
with sockets, pipes, AF_UNIX, TTY, and process waits. This audit did not change
the scheduler model.

Remaining:

- Consider per-object waiter lists if broad wakeups become measurable.
- Keep foreground-process Ctrl-C behavior covered by run-shell-check.

## Signals, sigreturn, and sigaltstack

Nanos has mature Linux signal-frame behavior. Spore now covers the parts modern
userland was reaching:

- `rt_sigprocmask` stores and applies the per-thread mask.
- Blocked signals become pending and are delivered when unblocked.
- Signal frames save/restore the previous mask through `rt_sigreturn`.
- `sigaltstack` stores per-thread alt-stack state and reports `SS_ONSTACK`.
- `SA_ONSTACK` handlers run on the alternate stack.
- Nested handlers work.
- Blocking reads on restartable wait reasons return `EINTR` without `SA_RESTART`
  and retry with `SA_RESTART`.
- Futex waits are restartable under `SA_RESTART`, matching the pthread/runtime
  path Spore exercises.

Remaining:

- Broaden restart handling only when a specific runtime proves it needs another
  wait reason. Sleep, poll, and epoll restart rules are intentionally not claimed
  here.
- Add portable host tests for signal-frame layout if the syscall split makes that
  practical.

## Process Metadata, procfs, Resource Limits, and sysinfo

Spore now has one authoritative memory-accounting helper for VSZ, RSS, and fault
counts. `/proc`, `ps`, `top`, `getrusage`, and `sysinfo` consume the same data.
`/proc/loadavg` uses the same thread table to report runnable load and task
counts.

Remaining:

- Audit `/proc/<pid>/stat` fields against htop, fastfetch, and procps after the
  next procfs compatibility pass.

## TTY, termios, and ioctl

Run-shell-check covers the historically sharp paths: large shell/editor paste,
raw/canonical mode return, and Ant Ctrl-C through the program handler rather than
a shell workaround. Unsupported ioctl counters are visible in `/proc` so future
vendored tool upgrades can be audited without noisy kernel logs.

Remaining:

- Promote repeated harmless ioctl probes to documented no-ops only after a real
  tool proves it expects that shape.

## Timers, Clocks, Random, and sysconf

Spore has EFI-seeded realtime, monotonic clocks, `clock_getres`, `getrandom`, and
a boot-to-boot RNG smoke canary.

Remaining:

- Audit `times`, `sysconf`, and CPU frequency reporting against Ant and fastfetch.
- Move rng-smoke into CI when QEMU is available there.

## Syscall Compatibility

Known compatibility probes return quiet `ENOSYS` and increment per-process
counters. That keeps modern runtime probing observable without turning the kernel
log into noise. `COMPATIBILITY_AUDIT.md` is the checked-in syscall/ioctl matrix;
keep it updated from `/proc/procinfo` and `/proc/<pid>/status` counters whenever
new workloads are tested.

## Networking and Socket Semantics

Socket-option compatibility was audited against the current network stack and
expanded in the integration suite. `SOCKET_OPTION_AUDIT.md` lists supported,
documented-no-op, and intentionally unsupported options.

Remaining:

- Replace any remaining busy-spin ARP behavior with wait/wake integration.
