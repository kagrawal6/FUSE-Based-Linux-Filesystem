#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include "wfs.h"

/*
 * Initialize disk image(s) to an empty filesystem with root inode allocated.
 * Usage: ./mkfs -r <0|1|1v> -d disk ... -i <inodes> -b <data_blocks>
 * Returns 0 on success, 1 on usage error, -1 on runtime error.
 */
int main(int argc, char *argv[])
{
    int raid_mode = -1;
    int disk_count = 0;
    int num_inodes = 0;
    int num_data_blocks = 0;
    char *disk_paths[MAX_DISKS];
    int opt;

    optind = 1;
    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch (opt) {
        case 'r':
            if (strcmp(optarg, "0") == 0) {
                raid_mode = RAID_0;
            } else if (strcmp(optarg, "1") == 0) {
                raid_mode = RAID_1;
            } else if (strcmp(optarg, "1v") == 0) {
                raid_mode = RAID_1V;
            } else {
                fprintf(stderr, "Error: Invalid raid mode.\n");
                return 1;
            }
            break;
        case 'd':
            if (disk_count >= MAX_DISKS) {
                return 1;
            }
            disk_paths[disk_count++] = optarg;
            break;
        case 'i':
            num_inodes = NBROUNDUP(atoi(optarg));
            break;
        case 'b':
            num_data_blocks = NBROUNDUP(atoi(optarg));
            break;
        default:
            return 1;
        }
    }

    if (raid_mode < 0) {
        fprintf(stderr, "Error: No raid mode specified.\n");
        return 1;
    }
    if (disk_count < 2 || num_inodes == 0 || num_data_blocks == 0) {
        return 1;
    }

    struct wfs_sb sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_inodes = (size_t)num_inodes;
    sb.num_data_blocks = (size_t)num_data_blocks;
    sb.raid_mode = raid_mode;
    sb.num_disks = disk_count;

    /* Superblock and bitmaps are contiguous (no padding between them). */
    sb.i_bitmap_ptr = (off_t)sizeof(struct wfs_sb);
    sb.d_bitmap_ptr = sb.i_bitmap_ptr + (off_t)(sb.num_inodes / 8);
    /* Inodes and data blocks are block-aligned. */
    sb.i_blocks_ptr = (off_t)BKROUNDUP(sb.d_bitmap_ptr + sb.num_data_blocks / 8);
    sb.d_blocks_ptr = sb.i_blocks_ptr + (off_t)(sb.num_inodes * BLOCK_SIZE);

    off_t total_size = sb.d_blocks_ptr + (off_t)(sb.num_data_blocks * BLOCK_SIZE);

    /* Stable unique IDs so mount order can be reconstructed after rename. */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < disk_count; i++) {
        uint64_t id = ((uint64_t)time(NULL) << 16) ^ ((uint64_t)rand() << 8) ^ (uint64_t)(i + 1);
        if (id == 0) {
            id = (uint64_t)(i + 1);
        }
        sb.disk_ids[i] = id;
    }

    size_t inode_bitmap_size = sb.num_inodes / 8;
    size_t data_bitmap_size = sb.num_data_blocks / 8;

    for (int d = 0; d < disk_count; d++) {
        int fd = open(disk_paths[d], O_RDWR);
        if (fd < 0) {
            return -1;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return -1;
        }
        if (total_size > st.st_size) {
            close(fd);
            return -1;
        }

        sb.this_disk_id = sb.disk_ids[d];

        if (lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
            close(fd);
            return -1;
        }

        char *i_bitmap = calloc(1, inode_bitmap_size);
        if (!i_bitmap) {
            close(fd);
            return -1;
        }
        /* Root inode (inode 0) is allocated. */
        i_bitmap[0] |= 0x01;

        if (lseek(fd, sb.i_bitmap_ptr, SEEK_SET) < 0 ||
            write(fd, i_bitmap, inode_bitmap_size) != (ssize_t)inode_bitmap_size) {
            free(i_bitmap);
            close(fd);
            return -1;
        }
        free(i_bitmap);

        char *d_bitmap = calloc(1, data_bitmap_size);
        if (!d_bitmap) {
            close(fd);
            return -1;
        }
        if (lseek(fd, sb.d_bitmap_ptr, SEEK_SET) < 0 ||
            write(fd, d_bitmap, data_bitmap_size) != (ssize_t)data_bitmap_size) {
            free(d_bitmap);
            close(fd);
            return -1;
        }
        free(d_bitmap);

        /* Each inode occupies a full 512-byte block. */
        char zero_block[BLOCK_SIZE];
        memset(zero_block, 0, sizeof(zero_block));
        if (lseek(fd, sb.i_blocks_ptr, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        for (size_t i = 0; i < sb.num_inodes; i++) {
            if (write(fd, zero_block, BLOCK_SIZE) != BLOCK_SIZE) {
                close(fd);
                return -1;
            }
        }

        struct wfs_inode root;
        memset(&root, 0, sizeof(root));
        root.num = 0;
        root.mode = S_IFDIR | 0755;
        root.uid = getuid();
        root.gid = getgid();
        root.size = 0;
        root.nlinks = 2; /* . and .. */
        root.atim = time(NULL);
        root.mtim = root.atim;
        root.ctim = root.atim;
        for (int i = 0; i < N_BLOCKS; i++) {
            root.blocks[i] = -1;
        }

        if (lseek(fd, sb.i_blocks_ptr, SEEK_SET) < 0 ||
            write(fd, &root, sizeof(root)) != (ssize_t)sizeof(root)) {
            close(fd);
            return -1;
        }

        close(fd);
    }

    return 0;
}
