# Spore

Spore is a small toy C23 operating system (kernel + userland) for AArch64, built to run in QEMU.

```sh
make
make run
```

Run host tests with `make test` and guest integration tests with `make run-tests`.

Both images boot through the pinned AArch64 UEFI build of Limine. The FAT
image carries Limine, its configuration, and the higher-half kernel ELF.
Spore mounts the separate ext2 disk and loads `/sbin/init` from it. The
kernel targets QEMU `virt` at EL1 with four-level paging. Limine supplies
the physical memory map, direct map, and parked secondary CPUs.
