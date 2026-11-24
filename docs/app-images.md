# Run an app

Pass an AArch64 Linux executable to `run`. Arguments after `--` go to the program.
This example forwards host TCP port 8080 to the same guest port.

```sh
./target/release/spore run ./my-server -p 8080 -- --port 8080
```

Use `build` to save an image. This one packages Ant with the [sample HTTP server](../tests/appliance/server.js).

```sh
./target/release/spore build ant -o server.img \
  --exec /app/ant --cwd /app --env REVISION=one \
  --file tests/appliance/server.js=/app/server.js -- /app/server.js
./target/release/spore boot server.img -p 8080
```

In another terminal, `curl http://127.0.0.1:8080/` returns `one`. Request `/quit` to stop the server.

[How files enter the image](host/build.md) · [Data volumes and networking](host/app-settings.md) · [Exit status](host/results.md) · [Benchmarks](host/measurements.md)
