# Running an app directly from RAM

`spore run` can hand the raw kernel and app payload to QEMU's loader. The CLI selects
this path when `kernel/spore.bin` exists, the guest has one vCPU, no firmware variable
store was requested, and the memory size is an even number of MiB from 32 through 1024
written with an `M` suffix. Otherwise it builds a temporary EFI image.
[CLI selection](../../tools/src/spore.rs) · [QEMU loader](../../tools/src/runner/qemu.rs)

QEMU places the kernel, payload, RAM size and boot timestamp in guest memory. Early
startup constructs page tables and describes the payload as the boot module `/spore.app`.
The common kernel then unpacks it and starts the app.
[Boot information shared by both loaders](../../kernel/include/init/boot_info.h) · [Kernel boot](../kernel/boot.md)
