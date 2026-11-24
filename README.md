# Spore

Spore boots a program in its own QEMU virtual machine. AArch64 Linux has the full runtime.

With Nix and direnv set up, run Ant from the checkout.

```sh
direnv allow
cargo build --release
./target/release/spore run ant
```

The first run downloads Ant and opens its JavaScript REPL.

[Supported hosts and guest binaries](docs/ports.md) · [Run your own app](docs/app-images.md) · [Boot a shell](docs/interactive.md) · [Explore the subsystems](docs/index.md)
