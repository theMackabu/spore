# Files in memory

At app boot the kernel installs packaged files in ramfs. Read-only inodes point into
the boot payload. Writable files in scratch directories acquire backing pages as data is
written. Their contents last for the current boot. [Payload unpacking](../../kernel/init/payload.c) · [Backing pages](../../kernel/fs/ramfs/pages.c)

Ramfs also creates the guest's initial device nodes, including `/dev/null`, `/dev/zero`,
`/dev/console` and `/dev/ptmx`. A device tag tells the I/O code which operation to perform
when a process opens one. [Boot nodes](../../kernel/fs/ramfs/boot.c) · [File I/O](../../kernel/proc/fd/file_io.c)

[Filesystem routing](vfs.md) · [Terminal devices](../kernel/terminal.md) · [Persistent storage](ext2.md)
