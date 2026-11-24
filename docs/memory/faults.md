# What happens on a page fault

A process has both a list of virtual memory areas and a page table. The areas describe
which addresses are valid, their access rights and any file backing. The table records
the physical pages the CPU can currently access. [Memory areas](../../kernel/mm/vma.c) · [Virtual memory manager](../../kernel/mm/vmm.c)

On a missing translation, the kernel looks up the area and checks the access. It can supply
an anonymous page or load bytes from the mapped file. On a write to a private copy-on-write
page, it resolves the write for that process. An access it cannot resolve raises `SIGSEGV`.
[Process fault handling](../../kernel/proc/memory.c) · [Exception dispatch](../../kernel/arch/aarch64/exceptions.c)

[Shared file writeback](../files/cache.md) · [Linux mapping calls](../../kernel/abi/linux/shared/mm/mmap.c) · [Physical pages](pages.md)
