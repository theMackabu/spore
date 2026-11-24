# Launch in QEMU

The runner starts an AArch64 `virt` machine with virtio networking. Saved images boot
with EFI firmware. Eligible `spore run` invocations use [direct boot](direct-boot.md).
App boots default to one vCPU and 128 MiB. Full-system boots default to four vCPUs and 8 GiB.
Use `--smp` and `--memory` to change them. [CLI options](../../tools/src/spore.rs) · [QEMU arguments](../../tools/src/runner/qemu.rs)

The serial port carries terminal I/O. On app boots, a second channel carries the first
instruction and exit events, so a program can write arbitrary bytes to stdout. On scripted
boots the host reads serial output and sends commands. [App results](results.md) · [Test harness](../../tools/src/runner/harness.rs)

[Guest devices](../kernel/devices.md) · [Attach storage or forward ports](app-settings.md)
