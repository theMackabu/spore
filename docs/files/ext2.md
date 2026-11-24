# Files on disk

The kernel mounts ext2 through byte-read and byte-write callbacks backed by virtio block.
Directories map names to inode numbers. Each inode records metadata and the block pointers
needed to locate its file data. Allocation updates the group's bitmaps and free counts.
[Mount](../../kernel/fs/ext2/mount.c) · [Directory records](../../kernel/fs/ext2/dir.c) · [Block mapping](../../kernel/fs/ext2/block_map.c)

The interactive image mounts its root at `/`. An app can attach an ext2 volume at `/data`.
[App data disks](../host/app-settings.md) · [Caching and writeback](cache.md)

## ext2+

**ext2+ is Spore’s filesystem extension for growing the inode table on demand.** Classic ext2 fixes the number of inodes when the disk is formatted. On an ext2+ root, creating more small files can use free data blocks for additional inode records. File contents and directories still use the ext2 block layout. [Allocator](../../kernel/fs/ext2/alloc.c) · [Block mapping](../../kernel/fs/ext2/block_map.c)

The Spore image builder seeds a root file named `/.spore-inodes` and sets the superblock incompatibility bit `0x80000000`. That bit tells filesystem tools that the image requires ext2+ support. Spore hides the store from root directory listings. [Image builder](../../tools/src/image/mod.rs) · [Mount](../../kernel/fs/ext2/mount.c) · [Directory records](../../kernel/fs/ext2/records.rs)

In version 1, the first block of the store holds a 64-byte header. It records the inode size, dynamic inode number base, capacity, next unused slot and free-list state. Dynamic inode numbers start at `0x40000000`. Slot `i` lives at byte offset `block_size + i × inode_size`. Directory entries use the resulting inode number in the usual way. [Disk structures](../../kernel/fs/ext2/internal.h) · [Inode I/O](../../kernel/fs/ext2/inode.c)

The allocator tries classic slots for the first 64 new allocations. For dynamic allocations it reuses a freed slot first. When it runs out of unused slots, it grows the store by 256 records on a Spore-built root. Both the allocation cursor and the free list persist across mounts. [Allocation and reuse](../../kernel/fs/ext2/alloc.c) · [Filesystem tests](../../tests/test_ext2.c) · [Inode benchmark](../../tests/bench_ext2_inodes.c)
