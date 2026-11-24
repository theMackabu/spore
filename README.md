# Spore

Spore is a small C23 and Rust operating system for AArch64, built to run in QEMU.

The Nix development shell includes Rust 1.100.0, the bare-metal AArch64 soft-float target for the kernel, and the AArch64 Linux musl target for guest programs.

Build the native CLI once in the development shell. From the checkout:

```sh
make
./build/spore boot                         # interactive Spore shell
./build/spore run ant                      # Ant REPL as the only guest process
./build/spore run ant -- -e 'console.log(6 * 7)'
./build/spore run ./my-aarch64-program -- argument
```

`boot` builds the interactive system image on demand and saves changes in `build/run-root.ext2`. Use `boot --snapshot` to start from the current base image and discard guest writes, or `boot --reset` to replace the persistent disk with that base. Rebuilding the base leaves your persistent disk intact. To boot a downloaded system image, pass both files explicitly with `spore boot IMAGE --root ROOT.ext2`.

`run` accepts a local AArch64 Linux ELF or a prebuilt binary name from the checkout. A local file takes precedence. The CLI packages and starts it directly as PID 1. Arguments after `--` go to the guest. Use `./build/spore --help` for VM and image options.

| Command | Purpose |
|---|---|
| `spore build`, `spore build --tests` | Build the interactive or integration image and its base root disk |
| `spore test host` | Run the native Meson tests |
| `spore test guest` | Boot the guest integration suite |
| `spore test shell` | Exercise the interactive shell and terminal |
| `spore test rng` | Compare random samples across two boots |
| `spore test appliance` | Verify app launch, lifecycle, networking, and persistence |
| `spore bench guest`, `spore bench inodes`, `spore bench appliance` | Measure the full guest, ext2 inode operations, or app boot and memory |

The interactive and integration images boot through the pinned AArch64 UEFI build of Limine. The FAT
image carries Limine, its configuration, and the higher-half kernel ELF.
Spore mounts the separate ext2 disk and loads `/sbin/init` from it. The
kernel targets QEMU `virt` at EL1 with four-level paging. Limine supplies
the physical memory map, direct map, and parked secondary CPUs.

## App images

Use `build` to save an image and `boot` to launch it repeatedly. For example, this packages the project's prebuilt Ant runtime and serves the supplied script on localhost.

```sh
./build/spore build ant -o build/server.img \
  --exec /app/ant --cwd /app --env REVISION=one \
  --file tests/appliance/server.js=/app/server.js -- /app/server.js
./build/spore boot build/server.img -p 8080
```

From another terminal, `curl http://127.0.0.1:8080/` returns `one`. This example also accepts `/quit` for a clean exit.

The FAT image contains Limine, the kernel, and one payload with the executable, arguments, environment, cwd, network settings, and files. The builder inspects ELF metadata and recursively packages the interpreter and shared libraries. Use `--lib-dir DIR` for additional AArch64 libraries and `--file HOST=/guest/path` for assets, plugins loaded through `dlopen`, or configuration. A missing required library fails the build. Files in the payload form a read-only root. The guest provides writable scratch space in `/tmp` and `/run`.

To keep application data across image replacements, create an ext2 volume once at a **new path**, then pass that same volume to each boot.

```sh
test ! -e build/data.ext2 && mke2fs -q -t ext2 -b 4096 build/data.ext2 4096
./build/spore boot build/server.img --data build/data.ext2 --forward 8080:8080
```

The volume is mounted at `/data`. Static IPv4 settings default to QEMU user networking. Configure them at build time with `--ip`, `--gateway`, `--netmask`, and `--dns`. Explicit DNS configuration also supplies musl's `/etc/resolv.conf`, unless the image already declares that file. Port forwarding binds to host loopback.

Threads and futexes are available. The app profile rejects creation of additional processes, snapshot spawning, and replacement of the running executable. The host returns the app's exit code, `128 + signal` for a fatal guest signal, or `125` for a boot or VM error. Use `--result result.json` to distinguish outcomes with overlapping numeric codes. The kernel sends the result on a dedicated virtio console, independently of application output.

### Appliance regression and measurements

`spore test appliance` runs static and dynamic musl probes and boots two different Ant images against the same ext2 volume. `spore bench appliance` also sweeps 32, 64, and 128 MiB. The default regression configuration is **one vCPU and 128 MiB**.

Measured on Apple Silicon with macOS 27.0.1, QEMU 11.1.3, HVF, and a fresh firmware variable store per boot. All three runs used the same 15 MiB image and one vCPU.

| Guest RAM | Cold boot to first app instruction fetch | First successful host HTTP response | Steady Ant RSS | Guest RAM in use |
|---|---:|---:|---:|---:|
| 32 MiB | Firmware stopped | — | — | — |
| 64 MiB | Firmware stopped | — | — | — |
| 128 MiB | 174 ms | 387 ms | 4,812 KiB | 55,404 KiB |

The bundled EDK2 firmware asserts a 128 MiB minimum before loading Limine. The entry timestamp measures QEMU launch to host observation of the kernel resolving the first EL0 instruction fetch. HTTP timing starts with the host CLI launch. The memory sample follows five successful requests and a half-second settling period. Guest RAM in use is configured RAM minus guest-reported free RAM, including the firmware and boot payload. These are local measurements from one sweep.
