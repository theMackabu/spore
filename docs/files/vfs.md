# Where a guest path goes

The virtual filesystem resolves a path to a node with a backend, device and inode. The
same read or stat call can reach a different backend depending on the boot profile.
[Routing rules](../../kernel/fs/vfs/route.c) · [VFS operations](../../kernel/fs/vfs/vfs.c)

| Path | App image | Interactive image |
| --- | --- | --- |
| Ordinary files under `/` | Read-only payload in [ramfs](ramfs.md) | Writable [ext2 root](ext2.md) |
| `/tmp`, `/run`, `/dev/shm` | Writable memory | Writable memory |
| `/data` with an app data disk | Root of the attached ext2 volume | Ordinary path on the root filesystem |
| `/dev`, `/proc`, `/sys` | Kernel-provided entries | Kernel-provided entries |

[Procfs](procfs.md) builds live process entries during lookup. The VFS also maintains
[path and page caches](cache.md) and reports [filesystem changes](notify.md) to watchers.
