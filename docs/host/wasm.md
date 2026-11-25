# Run ARM64 programs in WebAssembly

Spore’s experimental Wasm backend runs ARM64 Linux apps and static FreeBSD executables and boots the full Spore userland from an ext2 or ext2+ root disk.
The kernel and Rust instruction interpreter share one WebAssembly module. Guest instruction
fetches, loads and stores use Spore’s user page tables. Linux syscalls enter Spore’s shared
dispatcher. The host provides console output, time, entropy and block I/O.

Build and run from the development shell.

```sh
cargo build --release
./target/release/spore build --wasm
./target/release/spore run --wasm ./busybox.static -- echo hello
```

Use Alpine’s **aarch64 busybox-static** package for the example. Extract
`bin/busybox.static` and pass that executable to the runner. Spore also runs static PIE
executables. The CLI installs the executable under its filename in `/app`, which lets BusyBox
select its applet. [Alpine package](https://pkgs.alpinelinux.org/package/v3.23/main/aarch64/busybox-static)

Pass a writable ext2 or ext2+ volume to mount it at `/data`. Writes persist in the supplied volume.

```sh
./target/release/spore run --wasm ./busybox.static --data ./data.ext2 -- ls /data
```

## Full userland

```sh
./target/release/spore build --userland
./target/release/spore boot --wasm
```

The kernel launches `/sbin/init` from `target/release/root.ext2`. To select another root, pass `--root path/to/root.ext2`. Each boot keeps disk writes in a private in-memory snapshot and discards them on exit. Type commands at the guest shell or press Ctrl+] to disconnect.

## Networking in Node

`--net` gives the guest outbound IPv4 networking. A port forward also enables networking.

```sh
./target/release/spore run --wasm ant -p 8080 --env REVISION=wasm \
  --file tests/appliance/server.js=/app/server.js -- /app/server.js
```

The CLI uses QEMU’s user-mode network backend as an Ethernet bridge. The helper uses `-machine none`. Spore executes guest instructions in the Rust interpreter. The listener binds to loopback. In another terminal, `curl http://127.0.0.1:8080/` returns `wasm`, and `curl http://127.0.0.1:8080/quit` stops the guest.

## Browser

The build also produces a standalone browser runner in `target/release/wasm-web/`. Serve that directory locally.

```sh
./target/release/spore serve --wasm
```

Pass `--net` to enable a same-origin WebSocket Ethernet relay backed by the host’s user-mode network. Open the printed loopback URL, select a static ELF and enter one argument per
line. For a dynamically linked ELF, package its interpreter, libraries, files and
arguments on the host and select the resulting payload.

```sh
./target/release/spore build --wasm ./my-app -o my-app.spore --lib-dir ./libs -- --version
```

The CLI performs this packaging automatically on `spore run --wasm`. A worker runs the guest locally and reads ranges from the selected disk on demand.
To boot a system, select only the root disk, click Run and type into the terminal. Select **Keep disk changes in this browser** to save changed pages in origin-private storage. Reopen the same disk file on the same origin to resume. A fresh snapshot is the default. Browsers and Node can also import [the launcher](../../tools/wasm/run.mjs).

## Current scope

The default is one guest CPU with 64 MiB of RAM. `--smp 4` runs four virtual CPUs in turn on one host worker. Each has its own register bank and scheduler slot. Set `--memory 256M` (or Guest RAM in the browser) for larger workloads. The shared Spore scheduler preempts threads at 100 Hz and the launcher yields while they sleep. The guest receives the same FP, Advanced SIMD, CRC32, AES, PMULL, SHA1, SHA2 and LSE atomic feature bits as native AArch64 Spore. It caches instruction
classes and address translations. Its Rust JIT compiles frequently executed integer blocks to WebAssembly and checks the instruction bytes before reusing compiled code. Guest instruction and memory faults enter the signal subsystem. Fatal faults include the guest PC. The launcher defaults to a budget of 100 million guest instructions.

Linux syscalls use the native AArch64 dispatcher, including its seccomp checks. The appliance profile allows threads and denies additional processes. Piped CLI input uses a guest pipe and delivers EOF when the host writer closes. Full-system boots use Spore’s TTY. The standalone browser terminal works offline. Serving with `--net` connects the guest through the local relay. Guest code generation is tested by rewriting and executing an ARM64 function after changing page permissions.
Ant runs JavaScript expressions from its ordinary AArch64 musl binary.

```sh
./target/release/spore run --wasm ant -- -e 'console.log(6 * 7)'
```

Bun 1.4.5 executes a JavaScript expression, and Claude Code 2.1.332 starts far enough to print its version with 512 MiB of guest RAM. Interactive Claude sessions have not been verified on Wasm.

`spore test wasm-guest` boots the integration disk with the native suite’s UDP echo and console fixtures. `spore test wasm-shell` exercises the installed utilities, TLS, the compiler, shell and shutdown in a disposable full-userland boot.

`spore test wasm-neon` compiles the pinned upstream SIMDe ARM NEON corpus for ARMv8-A and runs every test executable through Spore.

`spore test wasm` builds an ordinary musl ELF and compares its output and exit status
under QEMU and Wasm. It checks pthreads, futex wakeups, timer preemption, thread-local storage and appliance policy. It also exercises cross-page accesses, permission changes,
unsupported instructions and the execution budget. `spore bench wasm` times an ARM64
integer loop with the interpreter and JIT on Node, with an already compiled kernel module and a fresh instance per run.
