# Physical memory

At boot the allocator takes usable ranges from the boot memory map and reserves space for its own metadata.
A bitmap locates free 4 KiB pages. Reference counts let mappings share a page until its last
owner releases it. Allocation can ask the [file page cache](../files/cache.md) to reclaim pages.
[Physical allocator](../../kernel/mm/pmm.c) · [Allocator tests](../../tests/test_pmm.c)

Object pools divide pages into fixed-size kernel objects. Page rings provide byte buffers
for [pipes](../processes/events.md) and [TCP streams](../net/tcp.md), allocating backing pages
as data arrives. [Object pools](../../kernel/mm/pool.c) · [Page rings](../../kernel/mm/page_ring.c)

[Virtual mappings and page faults](faults.md) · [Kernel boot](../kernel/boot.md)
