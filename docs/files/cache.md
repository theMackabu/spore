# Caching paths and file pages

The VFS lookup cache remembers a resolved node by path and whether the lookup follows the
final symlink. Namespace changes can invalidate cached lookups. Metadata versions allow
stale nodes to be refreshed. [Lookup cache](../../kernel/fs/vfs/lookup_cache.c)

The page cache identifies regular-file pages by device, inode and offset. This lets file
reads and memory mappings use the same data. A write through a shared mapping marks the
page dirty. Sync writes modified bytes back and rearms tracking for later writes. Ext2
also caches disk blocks below the VFS. [Page cache and writeback](../../kernel/fs/vfs/page_cache.c) · [Mapped writers](../../kernel/mm/file_page.c) · [Ext2 block cache](../../kernel/fs/ext2/block_cache.c)

[Virtual memory](../memory/faults.md) · [Page cache tests](../../tests/test_page_cache.c)
