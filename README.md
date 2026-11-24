# spore

Spore boots an AArch64 Linux program as the only process in a QEMU virtual machine.
You can also build a full system with an interactive shell.

## Run an app

With Nix and direnv set up, run these from the checkout.

```sh
direnv allow
make
./build/spore run ant
```

This opens the Ant JavaScript REPL. Spore downloads Ant on first use.
To run your own AArch64 Linux binary, give its path. Arguments after `--` go to the app.

```sh
./build/spore run ./my-server -p 8080 -- --port 8080
```

## Save an image

```sh
./build/spore build ant -o build/ant.img
./build/spore boot build/ant.img
```

## Boot a shell

```sh
./build/spore build --userland
./build/spore boot
```

The shell keeps its files between boots. Use `./build/spore boot --snapshot` for a disposable session.

Run `./build/spore --help` for all commands. See [app images](docs/app-images.md) for networking, storage, and benchmarks.
