# Spore engineering

To follow one program from the host into the guest, start with the [image builder](host/build.md),
then follow [kernel boot](kernel/boot.md), [ELF loading](kernel/elf.md), and [app exit](host/results.md).

| Where to start | What it covers |
| --- | --- |
| [App images](app-images.md) | Package a program, forward a port, attach storage |
| [Hosts and guest binaries](ports.md) | Host requirements, guest architectures, executable and syscall support |
| [Interactive boot](interactive.md) | Start a shell and keep files between boots |
| [Host tools](host/build.md) | Build images, launch QEMU, test and release |
| [Kernel boot and platform](kernel/boot.md) | Limine, CPUs, interrupts, devices and the first process |
| [Linux ABI](kernel/abi.md) | How guest library calls reach the kernel |
| [Processes](processes/lifecycle.md) | Threads, signals, confinement, descriptors and IPC |
| [Memory](memory/pages.md) | Allocation, address spaces and page faults |
| [Filesystems](files/vfs.md) | Path routing, storage, caching and change events |
| [Networking](net/ipv4.md) | Ethernet, IPv4, internet sockets and local sockets |
| [Userland](../userland/README.md) | Services, shell, packages and local commands |
