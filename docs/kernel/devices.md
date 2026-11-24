# Guest devices

The AArch64 platform maps QEMU's device registers before the kernel starts the drivers.
[Device mapping](../../kernel/arch/aarch64/platform.c) · [QEMU configuration](../host/qemu.md)

| Device | What passes through it |
| --- | --- |
| [PL011 UART](../../kernel/drivers/pl011.c) | Early boot output and the guest [serial terminal](terminal.md) |
| [Virtio console](../../kernel/drivers/virtio_console.c) | Kernel output when configured for it, or [app status records](../host/results.md) |
| [Virtio block](../../kernel/drivers/virtio_blk.c) | Reads and writes for the [ext2 disk](../files/ext2.md) |
| [Virtio network](../../kernel/drivers/virtio_net.c) | Ethernet frames exchanged with the [IPv4 stack](../net/ipv4.md) |

Kernel messages also accumulate in a circular buffer readable at `/proc/kmsg`.
[Kernel logging](../../kernel/lib/kprintf.c) · [Procfs](../files/procfs.md)
