# Hosts and guest binaries

The Spore CLI runs on macOS, Linux and FreeBSD. `spore run` selects AArch64, x86-64 or RV64 from the ELF header. `spore build` records the CPU in the saved app image so `spore boot` can select it again.

Linux programs on all three CPUs use the same virtual memory, filesystem, socket, descriptor and scheduler code. The ports suite runs the same musl program on each CPU to check mapped io_uring queues, epoll, pthreads and timed waits. With userland enabled it also boots a disposable ext2 root to check fork, copy-on-write and waitpid.

```sh
./target/release/spore run ./server -p 8080
./target/release/spore test ports
```

The FreeBSD ELF adapter shares its syscall translations across the three CPUs. It supports a subset of FreeBSD calls, including mmap, mprotect, poll and wait4. Mach-O, the FreeBSD runtime linker and kqueue remain unimplemented.

The bundled full-system userland targets AArch64. With userland enabled, supply an ext2 root containing a matching /sbin/init to boot another CPU.

```sh
./target/release/spore boot --arch riscv64 --root root-riscv64.ext2 --snapshot
```

AArch64 uses the virt machine and GIC. RV64 uses virt and OpenSBI. x86-64 uses Q35, a local APIC and PCI virtio. RV64 and x86-64 currently run with one vCPU.
