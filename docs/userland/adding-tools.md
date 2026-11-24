# Add a guest command or demo

1. Put a human-facing command in `userland/bin/NAME/`. Model its Meson target on
   [logger](../../userland/bin/logger/meson.build). Include [spore.h](../../userland/lib/spore/spore.h)
   for helpers such as `usage` and `eprintf`.
2. Register the directory in the [userland build](../../userland/meson.build) and add a
   source-to-guest path such as `userland/bin/NAME /bin/NAME` to the [image manifest](../../userland/image.manifest).
3. Run `./build/spore build --userland` and `./build/spore boot --snapshot` to try it.

Commands conventionally return 0 for success, 1 for an ordinary failure and 2 for usage
errors. Send diagnostics to stderr.

For a confinement demo, follow the [writer example](../../userland/demos/writer/main.c)
and its [policy manifest](../../userland/demos/writer/manifest). Install it under
`/home/spore/demos/` and update the [shell checks](../../tools/src/runner/commands.rs) if they
exercise it. Put harness-only instruments under `userland/libexec/spore-test/` and invoke
them by their full guest path. The [integration suite](../host/checks.md) has its own image manifest.
