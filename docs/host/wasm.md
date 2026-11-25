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
python3 -m http.server 8000 --bind 127.0.0.1 --directory target/release/wasm-web
```

Open `http://localhost:8000/`, select a static ELF and enter one argument per
line. For a dynamically linked ELF, package its interpreter, libraries, files and
arguments on the host and select the resulting payload.

```sh
./target/release/spore build --wasm ./my-app -o my-app.spore --lib-dir ./libs -- --version
```

The CLI performs this packaging automatically on `spore run --wasm`. A worker runs the guest locally and reads ranges from the selected disk on demand.
To boot a system, select only the root disk, click Run and send commands through Console input. Stop terminates the worker and discards its snapshot. Browsers and Node can also import [the launcher](../../tools/wasm/run.mjs).

## Current scope

One CPU runs guest threads with 64 MiB of guest RAM. The shared Spore scheduler preempts threads at 100 Hz and the launcher yields while they sleep. The interpreter handles a subset
of A64 integer, branch, load/store, exclusive, scalar floating-point and SIMD instructions. It caches instruction
classes and address translations. It reports the guest PC on an unsupported instruction or
memory fault. The launcher defaults to a budget of 100 million guest instructions.

Linux syscalls use the native AArch64 dispatcher, including its seccomp checks. The appliance profile allows threads and denies additional processes. Batch apps without console input see EOF. Full-system boots use Spore’s TTY. The standalone browser page runs offline. JIT workloads remain unverified.
Ant runs JavaScript expressions from its ordinary AArch64 musl binary.

```sh
./target/release/spore run --wasm ant -- -e 'console.log(6 * 7)'
```

Claude Code is unverified.

`spore test wasm-guest` boots the integration disk with the native suite’s UDP echo and console fixtures.

`spore test wasm` builds an ordinary musl ELF and compares its output and exit status
under QEMU and Wasm. It checks pthreads, futex wakeups, timer preemption, thread-local storage and appliance policy. It also exercises cross-page accesses, permission changes,
unsupported instructions and the execution budget. `spore bench wasm` times an ARM64
integer loop using Node with an already compiled module and a fresh instance per run.
