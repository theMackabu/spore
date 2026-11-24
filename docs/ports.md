# Hosts and guest binaries

The CLI runs on macOS, Linux and FreeBSD. On FreeBSD 15.1 arm64, install `qemu-nox11` to run a local AArch64 executable with a prebuilt CLI.

| Guest | Executables | Devices and services |
| --- | --- | --- |
| AArch64 Linux | Static ELF and dynamically linked musl apps | App images, full-system shell, storage and networking |
| AArch64 FreeBSD | FreeBSD-branded static ELF | Process entry and auxiliary vector, console and vector I/O, process IDs, basic memory mapping, protection and random bytes |
| x86-64 Linux and FreeBSD | Freestanding static ELF with console output and exit | Q35, COM1 console, COM2 workload status |
| RV64 Linux and FreeBSD | Freestanding static ELF with console output and exit | QEMU virt, OpenSBI debug console and reset, virtio workload status |

`spore run` selects the guest CPU from the ELF header. The x86-64 and RV64 ports handle Linux `write`, `exit` and `exit_group`, and FreeBSD `write` and `exit`. They use one CPU, 256 MiB by default, a 64 KiB user stack and a 4 GiB user address range. They accept arguments after `--` and report exit status and fatal faults to the host. Filesystems, dynamic linking, and networking for those two ports remain future work. Saved app images currently target AArch64. Mach-O, Capsicum and the FreeBSD runtime linker are unsupported.

Build and run a small RV64 example from the development shell.

```sh
clang --target=riscv64-unknown-linux-musl -nostdlib -static -fuse-ld=lld \
  tests/ports/linux-riscv64.S -o /tmp/spore-rv64
./target/release/spore run /tmp/spore-rv64 -- PASS
```

The program prints `PASS` and exits with status 42. To boot either kernel's serial monitor or run the portability suite, use the following commands.

```sh
./target/release/spore boot --arch x86_64
./target/release/spore boot --arch riscv64
./target/release/spore test ports
```

In the monitor, `memory` prints free KiB and `poweroff` exits QEMU.
