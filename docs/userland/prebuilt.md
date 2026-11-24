# Building the guest package set

The package manifest pins archive URLs and SHA-256 hashes and lists the files to extract.
The first build downloads the archives. Later builds reuse verified copies in
`.cache/packages/`, including after a clean build directory. The image manifest assigns
guest paths and shared-library links. [Package pins](../../userland/prebuilt/packages.json) · [Extractor](../../userland/prebuilt/unpack.py) · [Image manifest](../../userland/image.manifest)

Some dependencies are build inputs only. Curl and trustd link against mbedTLS 4.
The separate apk-tools build uses mbedTLS 3 and zlib. Alpine supplies wide-character
ncurses and the xterm-256color description. The host unpacks Void's narrow ncurses
library with zstd. [SDK assembly](../../userland/prebuilt/sdk.py) · [Guest build](../../userland/meson.build)

[Installed programs](packages.md) · [Image builder](../host/build.md)
