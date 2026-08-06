# FUSE-Based Linux Filesystem

A user-space filesystem built with [FUSE](https://github.com/libfuse/libfuse) that implements a traditional block-based layout with integrated RAID 0 and RAID 1 (including verified mirroring). Inspired by classic designs such as FFS/ext2 and modern systems like Btrfs and ZFS that integrate RAID at the filesystem layer.

## Objectives

- Understand how common filesystem operations are implemented
- Implement a traditional block-based filesystem (superblock, bitmaps, inodes, data blocks)
- Build a user-level filesystem using FUSE

## Overview

The filesystem supports basic operations: creating files and directories, reading and writing, listing directories, deleting entries, and retrieving attributes. Storage is always RAID-backed — either striping (RAID 0) or mirroring (RAID 1 / RAID 1v). Metadata is always mirrored across disks.

There are two main programs:

| Program | Role |
|---------|------|
| `mkfs` | Formats one or more disk image files into an empty filesystem |
| `wfs` | Mounts the formatted images at a mount point via FUSE |

On-disk structures are defined in `wfs.h` and must remain compatible with the provided layout (except for the allowed superblock extensions described below).

## Background

### RAID

RAID combines multiple disks into one logical volume for performance and/or redundancy:

| Mode | Behavior |
|------|----------|
| **RAID 0** | Stripes data across disks (performance; no redundancy) |
| **RAID 1** | Mirrors data across disks (redundancy) |
| **RAID 5 / 10** | Parity or stripe+mirror combinations (not used here) |

This project implements **RAID 0**, **RAID 1**, and **RAID 1v** at the filesystem layer. Only data blocks are striped in RAID 0; metadata always uses mirroring.

### FUSE

FUSE lets you implement a filesystem in userspace by registering callbacks in a `struct fuse_operations`. The kernel forwards VFS operations (getattr, read, write, mkdir, …) to those callbacks.

```c
static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char *argv[]) {
    /* Filter program-specific args, then pass FUSE options + mount point */
    return fuse_main(argc, argv, &ops, NULL);
}
```

Arguments meant for your program (disk images) must be separated from FUSE options before calling `fuse_main`.

## Filesystem Layout

The disk resembles a simple FFS/ext2-style layout:

1. **Superblock** — filesystem metadata (and RAID mode / disk order)
2. **Inode bitmap** — which inodes are allocated
3. **Data block bitmap** — which data blocks are allocated
4. **Inodes** — one block (512 B) each; always block-aligned
5. **Data blocks** — 512 B each

Superblock and bitmaps are stored continuously at the start of the disk with no padding. Inodes and data blocks are always aligned to 512 bytes — do not pack multiple inodes into one block.

### Files and directories

- **Regular files**: data blocks hold file contents. Each inode has a fixed number of direct block pointers plus one indirect block for larger files.
- **Directories**: data blocks hold directory entries. Directories do **not** use the indirect block (capacity is limited to direct blocks). New files and directories start empty.

### Core operations

- **Create** — allocate an inode via the inode bitmap; add a directory entry in the parent
- **Write** — map the file offset to data block(s), allocate new blocks via the data bitmap as needed; writes may span blocks
- **Read** — map the offset to data block(s) and copy into the caller buffer
- **Unlink / rmdir** — free data blocks and the inode; remove the parent directory entry (directories are presumed empty for `rmdir`)

Block size is always **512 bytes**.

## Part 1 — `mkfs`

Initializes disk image(s) to an empty mountable filesystem (superblock + root inode).

```text
./mkfs -r <mode> -d <disk> [-d <disk> ...] -i <num_inodes> -b <num_data_blocks>
```

Example:

```bash
./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
```

Creates an empty filesystem with 32 inodes and data blocks rounded up to the nearest multiple of 32 (here 224) on each disk. If an image is too small, `mkfs` exits with `-1`. A RAID mode (`-r`) is required.

## Part 2 — `wfs`

Mounts the RAID filesystem at a mount point:

```text
./wfs disk1 disk2 [FUSE options] mount_point
```

Pass FUSE options (e.g. `-f`, `-s`) and the mount point through to `fuse_main`. Assume `-s` (single-threaded) is always used. Prefer `-f` while debugging so logs appear in the foreground terminal.

### Features

- Create empty files and directories
- Read and write files up to the size supported by the indirect data block
- Read directories (`ls`)
- Remove files and empty directories
- Get attributes (`stat`), filling: `st_uid`, `st_gid`, `st_atime`, `st_mtime`, `st_mode`, `st_size`
- RAID 0, RAID 1, and RAID 1v

### Error codes

| Condition | Return |
|-----------|--------|
| Path does not exist | `-ENOENT` |
| Name already exists on create | `-EEXIST` |
| Out of space on create/write | `-ENOSPC` |

## RAID Modes

### RAID 0 (striping)

Created with `-r 0`. Applied **only to data blocks**. Metadata is always mirrored (RAID 1). Stripe unit is one 512 B block: the first data block of a file goes to disk 1, the next to disk 2, and so on. Derive disk index and on-disk offset from the file’s logical block number and the number of disks.

### RAID 1 (mirroring)

Created with `-r 1`. All data and metadata are identical across disks.

### RAID 1v (verified mirroring)

Created with `-r 1v`. Same on-disk layout as RAID 1. On every read, compare copies across drives and return the majority value; on a tie, prefer the disk with the lower mount index.

### Superblock extensions

Extend `struct wfs_sb` (add fields only at the **end**) to record:

- RAID mode used at format time
- Disk order information needed for RAID 0

Do not change other metadata structures in `wfs.h`.

### Mount rules

- Mount with the **same number of disks** used at format time; otherwise exit non-zero (e.g. “not enough disks”)
- Disk **order** at mount may differ; mounting must succeed if the correct set of images is given
- Disk **filenames** are not identifiers — rename must still work

Examples:

```bash
./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
./wfs disk1 disk2 -f -s mnt          # OK

./mkfs -r 1 -d disk1 -d disk2 -d disk3 -i 32 -b 200
./wfs disk1 disk2 -f -s mnt          # invalid: wrong disk count

./mkfs -r 0 -d disk1 -d disk2 -d disk3 -i 32 -b 200
./wfs disk3 disk1 disk2 -f -s mnt    # OK (order does not matter)

./mkfs -d disk1 -i 32 -b 200         # invalid: no RAID mode
```

## Build and run

```bash
make

# Create zeroed disk images (see create_disk.sh; at least 2 disks required)
./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
mkdir -p mnt
./wfs disk1 disk2 -f -s mnt
```

In another terminal:

```bash
stat mnt
mkdir mnt/a
echo asdf > mnt/x
cat mnt/x
ls mnt
```

Unmount with the provided helper:

```bash
./umount.sh mnt
```

`create_disk.sh` builds a 1 MB zeroed image named `disk` (useful locally; tests may use other sizes — do not hard-code 1 MB). The Makefile is the grading/build entry point; ensure `make` succeeds.

### Debugging tips

- Inspect images with `xxd -e -g 4 <disk> | less` before/after `mkfs`
- Run with `-f` and print at the start of each FUSE callback
- Under gdb: `gdb --args ./wfs disk1 disk2 -f -s mnt`
- Prefer `mmap` of each disk image for simpler structure access

## Implementation notes

- Valid names: letters, digits, underscore (`_`); max length **28**; paths use `/`
- Directories may contain blank directory entries within their reported size; free all directory data blocks on `rmdir`, but you need not shrink directory blocks when unlinking files
- Allocation/free policy for blocks is up to you; tests check counts, not exact placement
- Design helpers such as `allocate_inode()` that use the bitmaps and return pointers or errors

## References

- [FUSE API documentation (HMC)](https://www.cs.hmc.edu/~geoff/classes/hmc.cs135.201001/homework/fuse/fuse_doc.html)
- [FUSE tutorial (NMSU)](https://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/html/index.html)
- [libfuse Doxygen](http://libfuse.github.io/doxygen/index.html)
- [FUSE examples](https://github.com/fuse4x/fuse/tree/master/example)
- [OSTEP: RAID](https://pages.cs.wisc.edu/~remzi/OSTEP/file-raid.pdf)
- errno macros: `/usr/include/asm-generic/errno-base.h`
