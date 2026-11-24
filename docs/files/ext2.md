# Files on disk

The kernel mounts ext2 through byte-read and byte-write callbacks backed by virtio block.
Directories map names to inode numbers. Each inode records metadata and the block pointers
needed to locate its file data. Allocation updates the group's bitmaps and free counts.
[Mount](../../kernel/fs/ext2/mount.c) · [Directory records](../../kernel/fs/ext2/dir.c) · [Block mapping](../../kernel/fs/ext2/block_map.c)

Spore-built roots can also contain `/.spore-inodes`. With that store active, the allocator
uses an initial allowance of classic inodes, then allocates inode records inside the store.
Freed dynamic slots can be reused. [Inode allocation](../../kernel/fs/ext2/alloc.c) · [Inode benchmark](../../tests/bench_ext2_inodes.c)

The interactive image uses ext2 at `/`. An app can attach its own ext2 volume at `/data`.
[App data disks](../host/app-settings.md) · [Caching and writeback](cache.md)
