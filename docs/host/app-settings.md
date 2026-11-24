# App storage and networking

Packaged files are read-only. Use `/tmp` or `/run` for scratch files. To keep data across
boots or image replacements, create an ext2 volume at a new path and pass it to each boot.

```sh
test ! -e data.ext2 && mke2fs -q -t ext2 -b 4096 data.ext2 4096
./target/release/spore boot server.img --data data.ext2 --forward 8080:8080
```

The guest mounts the volume at `/data`. [Build the example server](../app-images.md) · [Filesystem routing](../files/vfs.md)

The app starts with static IPv4 settings for QEMU user networking. Set `--ip`, `--netmask`,
`--gateway` and `--dns` at image build time. Explicit DNS supplies musl's
`/etc/resolv.conf` unless the image already includes it. At boot, `--forward HOST:GUEST`
binds a TCP port on host loopback. [Network settings](../../tools/src/app_network.rs) · [IPv4](../net/ipv4.md)
