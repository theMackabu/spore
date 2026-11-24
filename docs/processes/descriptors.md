# File descriptors

Each process maps small integer descriptors to reference-counted open files. Duplicating
a descriptor shares the open file and its offset. Close-on-exec belongs to the descriptor,
so two references to one file can behave differently on exec. [Descriptor tables](../../kernel/proc/fdtable.c) · [Open files](../../kernel/proc/fd.c)

Reads and writes dispatch by open-file type. Regular files go through the VFS, while
pipes, sockets and terminals have their own buffering and wait rules. Advisory `flock`
tracks shared and exclusive locks on open files and wakes waiting callers on release.
[I/O dispatch](../../kernel/proc/io.c) · [File I/O](../../kernel/proc/file_io.c) · [File locks](../../kernel/proc/flock.c)

[Pipes and readiness](events.md) · [Sockets](../net/sockets.md) · [VFS](../files/vfs.md)
