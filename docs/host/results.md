# How an app exits

The app is the guest's only process. It can create threads and use futexes. The app profile
rejects process creation, snapshot spawning and replacement of the executable. [Process lifecycle](../processes/lifecycle.md)

Before poweroff the kernel flushes the filesystem and sends a result over the virtio console.
A failed flush changes the result to a boot error. The CLI maps normal exit to the app's
code, a fatal signal to `128 + signal`, and boot or VM failure to `125`. Use
`--result result.json` to distinguish outcomes that share a numeric code.
[Kernel status writer](../../kernel/init/status.c) · [Host result reader](../../tools/src/runner/app.rs)

[App images](../app-images.md) · [Boot measurements](measurements.md)
