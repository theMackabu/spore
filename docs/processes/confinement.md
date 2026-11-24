# Credentials and confinement

A domain tracks real, effective and saved user and group IDs, plus supplementary groups.
Exec uses file ownership and mode when applying new credentials. Descriptor limits and
CPU budgets also belong to the domain. [Credentials](../../kernel/proc/domain.c) · [Resource limits](../../kernel/abi/linux/shared/proc/limits.c)

In the guest shell, `confine fs:/tmp /bin/ls /tmp` runs one command with a filesystem
policy. Other profiles limit system calls, mapped pages or outgoing network endpoints.
The kernel checks a new profile against any existing restrictions before installing it.
[Policy parser](../../kernel/proc/manifest.rs) · [Enforcement](../../kernel/proc/policy.c) · [Guest policy checks](../../userland/tests/integration/policy.c)

[Shell commands](../userland/shell.md) · [Process lifecycle](lifecycle.md)
