# Release artifacts

The release workflow reads the version from Cargo.toml. A version bump on the release branches
can publish a new tag. The workflow skips a tag that already exists.

It builds the CLI, interactive and integration images, runs the host tests, and publishes
each boot image with its matching ext2 root. The Linux x86-64 CLI tarball includes the
kernel and EFI loader. `SHA256SUMS` covers the packaged artifacts.

[Release workflow](../../.github/workflows/release.yml) · [Build targets](build.md) · [Boot a shell](../interactive.md)
