# Spore Socket Option Audit

Spore keeps HTTPS, SSH, package-manager, DNS, and Python socket behavior behind
the existing socket syscalls and egress checks. This matrix records the option
surface currently implemented in the kernel and pinned by integration tests.

## SOL_SOCKET

| Option | Status |
| --- | --- |
| `SO_REUSEADDR` | Stored and returned. |
| `SO_REUSEPORT` | Stored and returned. |
| `SO_DONTROUTE` | Stored and returned. |
| `SO_BROADCAST` | Enforced for UDP broadcast sends. |
| `SO_SNDBUF` / `SO_RCVBUF` | Stored with kernel-side caps. |
| `SO_KEEPALIVE` | Stored and returned; TCP keepalive timers use it. |
| `SO_RCVLOWAT` / `SO_SNDLOWAT` | Stored/returned; receive low-water affects polling. |
| `SO_ACCEPTCONN`, `SO_DOMAIN`, `SO_PROTOCOL`, `SO_TYPE`, `SO_ERROR` | Returned for Linux userland probes. |
| `SO_LINGER` | Stored/returned; abortive close is tested. |
| `SO_RCVTIMEO` / `SO_SNDTIMEO` | Stored and used by blocking receive/send paths. |
| `SO_PEERCRED` | Supported for AF_UNIX peer credential queries. |

## IPPROTO_IP

| Option | Status |
| --- | --- |
| `IP_TOS` | Stored and returned. |
| `IP_TTL` | Stored and returned. |
| `IP_MTU_DISCOVER` | Stored and returned for compatibility. |
| `IP_BIND_ADDRESS_NO_PORT` | Stored and returned for compatibility. |

## IPPROTO_TCP

| Option | Status |
| --- | --- |
| `TCP_NODELAY` | Stored and returned. |
| `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, `TCP_KEEPCNT` | Stored and returned; keepalive timers consult them. |
| `TCP_USER_TIMEOUT` | Stored and returned; timeout behavior is tested. |
| `TCP_FASTOPEN`, `TCP_FASTOPEN_CONNECT` | Accepted as compatibility no-ops. Spore does not implement TCP Fast Open. |

## Regression Coverage

`userland/tests/integration/main.c` exercises the option surface above through
`socket_options_regression()` plus targeted TCP/UDP tests for broadcast,
receive-low-water polling, linger reset, timeouts, SIGPIPE, and loopback/refused
connection behavior.

## Remaining Work

- Add options only when a real workload records an unsupported option through the
  syscall/ioctl counters or fails at runtime.
- Keep egress enforcement at packet send time; socket options must not create a
  second policy path.
