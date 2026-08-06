#!/usr/bin/python3

import argparse
import wfsverify

def corrupt_disk(disks):
    filesystems = [wfsverify.WfsState(disk) for disk in disks]

    for fs in filesystems:
        fs.clear_datablock_region()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--disks", nargs="+", help="list of disks")

    args = parser.parse_args()

    corrupt_disk(args.disks)

