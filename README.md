# FUSE-Based Linux Filesystem

A user-space, block-based filesystem built with FUSE. It stores data on multiple disk image files and supports **RAID 0** (striping), **RAID 1** (mirroring), and **RAID 1v** (verified mirroring). The on-disk layout is similar to classic systems like FFS/ext2: superblock, bitmaps, inodes, and data blocks.

For a deep dive into design, concepts, and internals, see [project description.md](project%20description.md).

## Requirements

- Linux (or a Linux VM/container)
- `gcc`, `make`, `pkg-config`
- libfuse development headers (`libfuse-dev` on Debian/Ubuntu)
- Python 3 (for the test suite)

## Build

```bash
cd solution
make
```

This produces `mkfs` (format disk images) and `wfs` (mount the filesystem).

## Run

```bash
cd solution

# Create at least two zeroed disk images (1MB each by default)
./create_disk.sh

# Format with RAID 1, 32 inodes, ~200 data blocks (rounded up to a multiple of 32)
./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200

# Mount (foreground + single-threaded — recommended while developing)
mkdir -p mnt
./wfs disk1 disk2 -f -s mnt
```

In a second terminal:

```bash
cd solution
stat mnt
mkdir mnt/docs
echo hello > mnt/docs/note.txt
cat mnt/docs/note.txt
ls -la mnt/docs
```

Unmount:

```bash
./umount.sh mnt
# or: fusermount -u mnt
```

### Other RAID modes

```bash
./mkfs -r 0  -d disk1 -d disk2 -d disk3 -i 32 -b 200   # RAID 0 striping
./mkfs -r 1  -d disk1 -d disk2 -i 32 -b 200             # RAID 1 mirroring
./mkfs -r 1v -d disk1 -d disk2 -d disk3 -i 32 -b 200    # RAID 1v verified mirror
```

Mount always needs the **same number** of disks used at format time. Argument order does not matter.

## Tests

```bash
cd solution && make
cd ../tests
./run-tests.sh          # all tests
./run-tests.sh -t 10    # one test
./run-tests.sh -c       # continue after failures
```

## Project layout

| Path | Purpose |
|------|---------|
| `solution/` | Source (`mkfs.c`, `wfs.c`, `wfs.h`), Makefile, helpers |
| `tests/` | Automated test suite |
| `project description.md` | Detailed design and concepts write-up |
| `disk-layout.svg` / `.pdf` | On-disk layout diagram |
