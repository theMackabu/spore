# Spore Mount Plan

## Goal

Add a real mount layer so Spore can mount block devices and disk image files at
runtime, especially under `/mnt`. This is the VFS and device plumbing needed
before FAT images become useful outside the baked root filesystem.

## Stage 1: VFS Mount Table and `/mnt`

- Add `/mnt` to the base filesystem image.
- Replace fixed mount-prefix routing with a real VFS mount table.
- Track mount entries with:
  - source device or backing file
  - target path
  - filesystem type
  - flags such as read-only
  - private filesystem driver state
- Resolve paths by longest mountpoint prefix, then dispatch to that filesystem's
  VFS operations.
- Preserve existing mounts:
  - `/`
  - `/tmp`
  - `/run`
  - `/proc`
  - `/sys`
  - `/dev`
- Add `/proc/mounts` output from the mount table instead of hand-written static
  entries.
- Add invalidation rules so mounting or unmounting flushes path lookup cache
  entries affected by that mountpoint.
- Start with single-core/run-to-completion semantics; no locks beyond existing
  kernel invariants.

## Stage 3: `mount` / `umount` Interface

- Add a Linux-shaped `mount(2)` syscall if practical; otherwise add a small
  Spore syscall first and keep the userland command syntax Linux-like.
- Add `umount2(2)` or a minimal `umount` syscall path.
- Add `/bin/mount` and `/bin/umount`.
- Support basic forms:

```sh
mount -t fat16 /dev/blk/boot /mnt/boot
mount -t fat32 /dev/loop0 /mnt/img
umount /mnt/img
```

- Validate target path exists and is a directory.
- Reject mounting over busy mountpoints unless a later forced/lazy unmount mode is
  explicitly implemented.
- Enforce filesystem policy through existing capability checks. Mounting should
  require a privileged capability/root path, not become a sandbox escape.
- Update `statfs`, `stat`, `open`, `readdir`, `rename`, and `unlink` behavior so
  crossing mount boundaries is coherent.

## Stage 4: Loop Image Mounting

- Add a loop block-device abstraction backed by a regular file.
- Provide either:
  - `/dev/loop0`, `/dev/loop1`, ...
  - or an internal mount option that creates a transient loop device.
- Support:

```sh
mount -o loop -t fat32 image.img /mnt/img
```

- Loop device reads/writes go through VFS file IO to the backing file.
- Prevent recursive or unsafe mounts, for example mounting an image file from a
  filesystem that is itself backed by the same loop chain.
- Track dirty loop-backed writes and flush them on `sync`, `fsync`, and `umount`.
- Add integration tests that create a small image, mount it, read files, write a
  file, unmount, remount, and verify persistence.

## Done

- `/mnt` exists.
- Existing pseudo filesystems and root mount are represented in one mount table.
- `mount` and `umount` work for block devices.
- Loop-mounted disk images work from regular files.
- `/proc/mounts` reflects runtime mount state.
- Existing ext2, tmpfs, devfs, procfs, sysfs, and userland regressions still pass.

