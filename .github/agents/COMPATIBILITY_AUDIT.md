# Spore Compatibility Matrix

This file tracks Linux ABI gaps discovered by real workloads. The kernel also
counts unsupported syscalls and ioctls per process in `/proc/procinfo` and
`/proc/<pid>/status`; those counters are the source of truth when a workload
misbehaves.

## Current Runtime Surface

| Workload | Covered path | Current status |
| --- | --- | --- |
| Ant | dynamic exec, `brk`, `mremap`, signals, resolver, TLS file access | Runs as a modern-runtime canary; keep Ctrl-C and startup in shell-check. |
| curl / git remote helpers / spm fetchers | DNS, TCP, TLS, file-backed executable faults, socket options | Runtime paths are supported; TCP performance remains a separate tuning goal. |
| Python package scripts | shebang exec, dynamic loader, `faccessat`, path opens | Supported by current syscall/file surface; package scripts remain useful regressions. |
| nano / vim / htop | termios, window-size ioctls, `/proc`, raw/canonical TTY | Supported for editor/top usage; unsupported ioctl counters catch optional probes. |
| dash / make / compiler drivers | fork/exec/wait, process groups, scripts, `posix_spawn`-style children | Supported by current process model; compiler parallelism is memory-pressure sensitive. |
| Dropbear / OpenSSH client | DNS, TCP, tty raw mode, window-size ioctl, CSPRNG | Client path supported; server/pty-only ioctls are not promoted without need. |
| nnn-style file managers | inotify startup probes | Minimal inotify fd/watch ABI exists; event delivery is not implemented yet. |

## Known Intentional ENOSYS

| Syscall family | Reason |
| --- | --- |
| `io_uring_*` | No io_uring implementation. Modern libraries should fall back to poll/select. |
| `clone3` | Existing `clone` path is implemented; `clone3` is left as a probe-only unsupported syscall. |
| Large Linux namespace/cgroup APIs | Not part of Spore's current process/security model. |

## Compatibility Shims That Need Care

| Area | Current shape | Risk |
| --- | --- | --- |
| inotify | `inotify_init1`, add/rm watch, and empty reads exist; no event queue yet. | Apps that require real directory notifications still need implementation. |
| memfd seals | `memfd_create` returns an unlinked tmpfs-backed fd with `F_ADD_SEALS`/`F_GET_SEALS`. Grow, shrink, write, future-write, and seal-seal are enforced; adding `F_SEAL_WRITE` while shared writable mappings exist returns `EBUSY`. | Existing shared mappings can continue after `F_SEAL_FUTURE_WRITE`, matching the intended Linux shape; keep this in integration coverage. |
| signal restart | Read/socket/pipe/inotify/child/futex waits restart where appropriate. | Sleep, poll, and epoll restart rules are intentionally conservative. |

## Update Procedure

1. Run the workload.
2. Inspect `/proc/procinfo` or `/proc/<pid>/status` for unsupported syscall/ioctl
   counts and last unsupported IDs.
3. Add a small Linux-shaped implementation only when the workload needs it.
4. Add a guest integration or shell-check canary for the new ABI surface.
