#!/usr/bin/python3

import argparse
import wfsverify
import os
from stat import *

def test_eq(teststr, first, second):
    """Compare 'first' and 'second', print a message and exit if not equal."""
    if (first != second):
        print(f"{teststr}: found {first} expected {second}")
        exit(1)

def test_geq(teststr, first, second):
    """Compare 'first' and 'second', print a message and exit if first < second."""
    if first < second:
        print(f"{teststr}: {first} should not be less than {second}")
        exit(1)

def test_gt(teststr, first, second):
    """Compare 'first' and 'second', print a message and exit if first <= second."""
    if first <= second:
        print(f"{teststr}: {first} must be greater than {second}")
        exit(1)

def test_nonzero(teststr, found):
    """Ensure 'found' is not zero."""
    if (found == 0):
        print(f"{teststr}: unexpectedly zero or empty")
        exit(1)

def roundup(n, k):
    """Roundup n by k."""
    remain = n % k
    return n if remain == 0 else (n + (k - remain))

def is_allocated_inode(disk, inodep, inode):
    """Given an inode position and inode dict, verify it's allocation."""
    test_eq(f"inode num matches allocation num [{disk}]", inode['num'], inodep)
    for field in ['mode', 'uid', 'gid', 'atim', 'mtim', 'ctim', 'nlinks']:
        test_nonzero(f"inode field {field} [{disk}]", inode[field])

def verify_inodes(inode_list, filesystem):
    """Verify inodes in a filesystem and return the number of dirs and files."""
    dirs = 0
    files = 0
    for inodep in inode_list:
        inode = filesystem.read_inode(inodep)
        is_allocated_inode(filesystem.diskname(), inodep, inode)
        if S_ISDIR(inode['mode']):
            dirs += 1
        elif S_ISREG(inode['mode']):
            files += 1
        else:
            print("Unexpected inode mode")
            exit(1)
    return (dirs, files)

def verify_initial_fs_state(disk, inodes, blocks):
    """Verify empty filesystem after running mkfs. Ignore raid in superblock."""
    wfs = wfsverify.WfsState(disk)

    test_eq(f"sb: num inodes [{disk}]", wfs.get_sb_inodes(), inodes)
    test_eq(f"sb: num datablocks [{disk}]", wfs.get_sb_datablocks(), blocks)

    test_gt(f"add raid fields to superblock", wfs.get_ibit(), wfs.get_sb_size())
    test_eq(f"inode bitmap size [{disk}]",
                 wfs.get_dbit() - wfs.get_ibit(), inodes / 8)
    
    dbit_size = wfs.get_iblock_region() - wfs.get_dbit()
    max_dbit_size = roundup(wfs.get_sb_datablocks() / 8, wfs.blksize)
    test_geq(f"data bitmap has extra block [{disk}]", max_dbit_size, dbit_size)
    test_eq(f"inode region block-aligned [{disk}]",
                 wfs.get_iblock_region() % wfs.blksize, 0)
    
    test_eq(f"inode region size [{disk}]",
                 wfs.get_dblock_region() - wfs.get_iblock_region(),
                 inodes * wfs.blksize)

    # check root inode
    allocated_inodes = wfs.list_allocated_inodes()
    test_eq(f"num. inodes allocated [{disk}]", len(allocated_inodes), 1)
    
    # data bitmap should be empty
    allocated_datablocks = wfs.list_allocated_datablocks()
    test_eq(f"num. datablocks allocated [{disk}]", len(allocated_datablocks), 0)
    
    for inodep in allocated_inodes:
        inode = wfs.read_inode(inodep)
        is_allocated_inode(disk, inodep, inode)
        test_eq(f"inode size [{disk}]", inode['size'], 0)

def verify_mkfs(disks, inodes, blocks):
    """Verify disk list after mkfs with expected num inodes and data blocks."""
    for disk in disks:
        verify_initial_fs_state(disk, inodes, blocks)

    print("Success")

def verify_raid1v(disks, expected_dirs, expected_files, expected_blocks):
    # TODO raid1v verification?
    print("Correct")
    
def verify_raid1(disks, expected_dirs, expected_files, expected_blocks):
    """Verify wfs formatted as raid1."""
    filesystems = [wfsverify.WfsState(disk) for disk in disks]
    all_blocks = [(filesystem.list_allocated_inodes(),
                   filesystem.list_allocated_datablocks(), filesystem)
                  for filesystem in filesystems]

    # check each filesystem has the same number of allocated inodes and datablocks
    for (inode_list, datablock_list, fs) in all_blocks:
        test_eq(f"allocated inodes on {fs.diskname()}",
                len(inode_list), (expected_files + expected_dirs))
        test_eq(f"allocated datablocks on {fs.diskname()}",
                len(datablock_list), expected_blocks)

    # ensure inode allocations match their bitmap position on each disk
    inode_lists = [(inode_list, fs) for (inode_list, datablock_list, fs) in all_blocks]

    # verify inodes on first disk
    (inode_list, ref_fs) = inode_lists[0]
    (dirs, files) = verify_inodes(inode_list, ref_fs)

    # compare inode regions on all disks
    ref_region = ref_fs.read_inode_region()
    for (_, fs) in inode_lists[1:]:
        comp_region = fs.read_inode_region()
        if ref_region != comp_region:
            print(f"raid1 inode regions must be identical {ref_fs.diskname()} {fs.diskname()}")
            exit(1)

    # compare data block regions on all disks
    ref_region = ref_fs.read_datablock_region()
    for (_, fs) in inode_lists[1:]:
        comp_region = fs.read_datablock_region()
        if ref_region != comp_region:
            print(f"raid1 datablock regions must be identical {ref_fs.diskname()} {fs.diskname()}")
            exit(1)

    print("Correct")

def verify_raid0(disks, expected_dirs, expected_files, expected_blocks, altblocks):
    """Verify wfs formatted as raid1."""
    filesystems = [wfsverify.WfsState(disk) for disk in disks]
    all_blocks = [(filesystem.list_allocated_inodes(),
                   filesystem.list_allocated_datablocks(), filesystem)
                  for filesystem in filesystems]

    # check each filesystem has the same number of allocated inodes and datablocks
    total_datablocks = 0
    for (inode_list, datablock_list, fs) in all_blocks:
        test_eq(f"allocated inodes on {fs.diskname()}",
                len(inode_list), (expected_files + expected_dirs))
        total_datablocks += len(datablock_list)

    if (altblocks != expected_blocks):
        if (total_datablocks != expected_blocks and total_datablocks != altblocks):
            print(f"total allocated datablocks on all disks: found {total_datablocks} expected either {expected_blocks} or {altblocks}.")
            exit(1)
    else:
        test_eq("total allocated datablocks on all disks",
                total_datablocks, expected_blocks)

    # ensure inode allocations match their bitmap position on each disk
    inode_lists = [(inode_list, fs) for (inode_list, datablock_list, fs) in all_blocks]

    # verify inodes on all the disks
    # slightly relaxed -- each disk must have all dir and file inodes
    # however, inodes can be different to accomodate indirect block
    for (inode_list, ref_fs) in inode_lists:
        (dirs, files) = verify_inodes(inode_list, ref_fs)

        test_eq(f"wfs directory inodes", dirs, expected_dirs)
        test_eq(f"wfs regular file inodes", files, expected_files)

    # TODO verify allocated data blocks are non-zero on each disk
    # not a big deal though
    print("Correct")

def unimplemented(mode):
    print(f'{mode} verification not implemented')
    exit()
    
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", help="verify mode: mkfs, raid0, raid1, raid1v")
    parser.add_argument("--inodes", help="expected number of inodes")
    parser.add_argument("--blocks", help="expected number of data blocks")
    parser.add_argument("--altblocks", help="some tests have an alternate number of acceptable data blocks")
    parser.add_argument("--dirs", help="expected number of directories")
    parser.add_argument("--files", help="expected number of regular files")
    parser.add_argument("--disks", nargs="+", help="list of disks")

    args = parser.parse_args()

    if args.mode == 'mkfs':
        verify_mkfs(args.disks, int(args.inodes), int(args.blocks))
    elif args.mode == 'raid1':
        verify_raid1(args.disks, int(args.dirs), int(args.files), int(args.blocks))
    elif args.mode == 'raid0':
        verify_raid0(args.disks, int(args.dirs), int(args.files), int(args.blocks), int(args.altblocks))
    elif args.mode == 'raid1v':
        verify_raid1v(args.disks, int(args.dirs), int(args.files), int(args.blocks))
    else:
        unimplemented(args.mode)
