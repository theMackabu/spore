# Mycelium services

In the interactive image, `/sbin/init` runs Mycelium. It reads `/etc/environment` and the
unit files, starts `multi-user.target`, then opens the interactive console after the boot
banner. It reaps children, checks timers and watchdogs, and applies each service's restart policy.
[Init loop](../../userland/bin/init/main.c) · [Service lifecycle](../../userland/bin/init/service.c) · [Boot units](../../userland/etc/mycelium/multi-user.target)

`myc` sends requests to the manager over `/run/mycelium.sock`. From the shell, try
`myc list-dependencies multi-user.target`, `myc status network.service` or
`myc logs hostname.service`. Services can direct output to the journal or console.
The `logger` command appends a tagged message to `/var/log/messages` and `/var/log/syslog`.
Use `sudo shutdown` to power off. [Control socket](../../userland/bin/init/control.c) · [Logger](../../userland/bin/logger/main.c)

[Network service](network.md) · [Console shell](shell.md) · [TLS service](trust.md)
