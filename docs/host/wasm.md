# Run Spore in WebAssembly

Spore compiles its kernel to WebAssembly. A Rust interpreter and JIT execute the guest’s ARM64 instructions. Boot, exec, page faults, filesystems, scheduling and syscalls use Spore’s kernel implementations.

## Run an app

From the development shell, run

```sh
cargo build --release
./target/release/spore build --wasm
./target/release/spore run --wasm ant -- -e 'console.log(6 * 7)'
```

Pass a package name, an AArch64 Linux ELF or a static AArch64 FreeBSD ELF. The CLI packages a dynamic Linux executable with its interpreter and libraries. The app profile permits threads and restricts the guest to one process. Piped CLI input arrives through a guest pipe.

To save an app and run it later, use

```sh
./target/release/spore build --wasm ./my-app -o my-app.spore --lib-dir ./libs -- --version
./target/release/spore boot --wasm my-app.spore --result result.json
```

The result records the workload exit, signal or failure and the time to its first instruction fetch. Pass `--data ./data.ext2` to mount a writable ext2 or ext2+ volume at `/data`.

## Boot the full system

```sh
./target/release/spore build --userland
./target/release/spore boot --wasm
```

The default root is a disposable snapshot of `target/release/root.ext2`. Select another base with `--root`. Pass `--persist ./my-root.ext2` to seed and reuse a writable copy. Supply an ESP before `--root` to expose its bytes through `/dev/blk/boot`. Ctrl+] disconnects the host console.

CLI guests default to one CPU and 64 MiB of RAM. Use `--memory 512M` or `--smp 4` to change the configuration. Virtual CPUs have independent registers and scheduler slots and take turns executing on one host worker.

## Networking

`--net` enables outbound IPv4. A port forward also enables networking.

```sh
./target/release/spore run --wasm ant -p 8080 --env REVISION=wasm \
  --file tests/appliance/server.js=/app/server.js -- /app/server.js
```

In another terminal, `curl http://127.0.0.1:8080/` returns `wasm`. Request `/quit` to stop the server. The host uses QEMU with `-machine none` to provide Ethernet and user-mode networking. Offline boots execute the Wasm module directly.

## Browser

```sh
./target/release/spore serve --wasm
```

Open the printed URL. The guest boots from `target/release/root.ext2` with 1 GiB of RAM. Use `?memory=512` to boot with 512 MiB. The server builds missing boot artifacts on first use. It briefly boots a disposable snapshot to collect startup disk pages and serves them in one compressed response. Later disk reads fetch missing ranges on demand.

Reloading reboots the guest with your saved disk changes. Use the same browser profile, address, port and base image to reopen the disk. Files in your home directory persist. The guest recreates `/tmp` at boot. Open `/?snapshot` for a disposable session starting from the base image. Drop a local ELF, packaged app or root disk onto the terminal to start it.

The DevTools console shows guest configuration, kernel messages and CPU exceptions with the guest program counter. Run `console.table(spore.stats())` in the page console for free guest memory in KiB and CPU counters refreshed about once a second. The instruction count and rate cover completed execution slices across all guest CPUs. JIT counters report compiled traces and guest instructions executed by those traces.

The local server enables outbound IPv4 through its WebSocket Ethernet relay by default. Pass `--offline` to boot without networking. For static hosting, serve `target/release/wasm-web/` with a `root.ext2` beside it on a server supporting HTTP byte ranges. Use `?app=./my-app.spore` to launch a hosted app instead. The JavaScript API accepts custom block and Ethernet devices, a cancellation signal and an instruction budget. [Launcher](../../tools/wasm/run.mjs)

## Verification

| Command | Coverage |
|---|---|
| `spore test wasm` | QEMU differential tests, musl and FreeBSD executables, memory protections, self-modifying code, signals, TLS, SMP, crypto, app policy and persistent boot |
| `spore test wasm-guest` | The full guest integration disk, UDP echo and blocked console reads |
| `spore test wasm-shell` | Installed commands, shell, TLS, compiler and shutdown |
| `spore test wasm-neon` | 285 upstream SIMDe test executables compiled for ARMv8-A |
| `spore bench wasm` | Spore’s interpreter and JIT on the same ARM64 integer loop |
| `spore bench kwemu` | Matched Spore and unkernel runs using the same guest disk and binaries |

Ant and Bun 1.4.5 executed JavaScript. Claude Code 2.1.332 printed its version and help, and displayed its welcome screen with 512 MiB of guest RAM. The interactive check ended at the offline connectivity screen.

The hosted CPU advertises the same FP, Advanced SIMD, CRC32, AES, PMULL, SHA1, SHA2 and LSE atomic capabilities as native AArch64 Spore. JIT blocks check guest code bytes before reuse and use permission-checked guest translations for memory access. [SIMDe suite revision](https://github.com/simd-everywhere/simde/tree/fdf984310d6d6b2fa5426fcf66bc227e04609c3f) · [kwemu measurements](wasm-benchmarks.md)
