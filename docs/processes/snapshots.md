# Starting a child from a memory snapshot

In the interactive kernel, a snapshot captures the caller's address space and virtual
memory areas and returns an ID. Spawning from that ID clones the saved memory, takes
process metadata and descriptors from the spawning parent, and starts the child at the
requested entry point with its argument.

The parent can reap the child through the same wait path used for ordinary processes.
On the host, `boot --snapshot` discards disk writes after an [interactive session](../interactive.md).

[Snapshot implementation](../../kernel/proc/memory/snapshot.c) · [Process lifecycle](lifecycle.md) · [Virtual memory](../memory/faults.md)
