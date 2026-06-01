# Spore FAT Plan

## Goal

Add FAT16/FAT32 filesystem support so Spore can read and write interoperable disk
images and volumes. FAT is the practical interchange filesystem; it is much
simpler than ext3 because there is no journal and no Unix permission model.

This plan assumes the mount infrastructure in `MOUNT.md` exists or is landing in
parallel.

## Stage 2: Read-Only FAT16/FAT32

- Implement a FAT driver with VFS operations for:
  - probe
  - mount
  - lookup
  - readdir
  - read
  - stat/statfs
- Support FAT16:
  - BIOS Parameter Block parsing
  - fixed root directory
  - FAT chain walking
  - 8.3 short names
- Support FAT32:
  - FAT32 BPB fields
  - root directory as a cluster chain
  - FSInfo sector parsing when present
  - larger cluster numbers and FAT entries
- Decide long filename support early:
  - minimal first pass can expose 8.3 names only
  - practical user-facing support should implement VFAT long filename entries
- Reject unsupported or corrupt images cleanly instead of panicking.
- Add host tests using generated FAT16/FAT32 images with known files.

## Stage 5: Writable FAT

- Add FAT chain allocation and free.
- Add directory entry allocation and deletion.
- Add file create, write, truncate, unlink, and rename.
- Update file size and timestamps.
- Implement FAT32 FSInfo free-cluster hints if present, but do not trust them
  blindly.
- Flush dirty FAT sectors, directory sectors, and data clusters through the block
  cache.
- Keep write ordering conservative:
  - allocate cluster
  - write data
  - update FAT chain
  - update directory entry
- Support `sync`, `fsync`, and clean `umount`.
- Add crash-adjacent tests where possible:
  - write file
  - unmount
  - remount
  - verify data and directory structure

## Stage 6: Partition Probing

- Add partition discovery before mounting whole disks by default.
- Start with MBR:
  - primary partitions
  - FAT type IDs as hints, not the only source of truth
- Add GPT later:
  - protective MBR
  - primary GPT header
  - partition entries
- Expose partitions as block devices, for example:

```text
/dev/blk/vda
/dev/blk/vda1
/dev/blk/vda2
```

- Allow explicit whole-device mounts for images that contain a bare FAT volume.
- Add tests for:
  - bare FAT image
  - MBR-partitioned FAT image
  - invalid/corrupt partition table

## Done

- FAT16 and FAT32 read-only mounts work.
- Writable FAT persists changes across unmount/remount.
- Loop-mounted FAT images work through the mount layer.
- MBR partitions are exposed as mountable block devices.
- FAT failures return errno-shaped errors and never corrupt unrelated mounts.
- Existing ext2/VFS, tmpfs, procfs/devfs/sysfs, mycelium, networking, and userland
  regressions remain green.

