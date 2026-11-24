# Spore

Spore boots an AArch64 Linux program in its own QEMU virtual machine.

With Nix and direnv set up, run Ant from the checkout.

```sh
direnv allow
cargo build --release
./target/release/spore run ant
```

The first run downloads Ant and opens its JavaScript REPL.

[Run your own app](docs/app-images.md) · [Boot a shell](docs/interactive.md) · [Explore the subsystems](docs/index.md)
