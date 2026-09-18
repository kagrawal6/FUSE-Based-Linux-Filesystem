#include <time.h>
#include <sys/stat.h>
#include <stdint.h>

#define BLOCK_SIZE (512)
#define MAX_NAME (28)
#define MAX_DISKS (32)

#define D_BLOCK (6)
#define IND_BLOCK (D_BLOCK + 1)
#define N_BLOCKS (IND_BLOCK + 1)

#define BKROUNDUP(sz) (((sz) + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1))
#define NBROUNDUP(x) (((x) + 31) & ~31)

/* RAID modes stored in the superblock */
#define RAID_0  0
#define RAID_1  1
#define RAID_1V 2

/*
  The fields in the superblock should reflect the structure of the filesystem.
  `mkfs` writes the superblock to offset 0 of the disk image.
  The disk image will have this format:

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0    ^                   ^
i_bitmap_ptr        i_blocks_ptr

*/

/* Superblock */
struct wfs_sb {
    size_t num_inodes;
    size_t num_data_blocks;
    off_t i_bitmap_ptr;
    off_t d_bitmap_ptr;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    /* Extend after this line */
    int raid_mode;                      /* RAID_0, RAID_1, or RAID_1V */
    int num_disks;                      /* number of disks in the array */
    uint64_t disk_ids[MAX_DISKS];       /* ordered disk IDs (same on every disk) */
    uint64_t this_disk_id;              /* unique ID for this physical disk */
};

/* Inode */
struct wfs_inode {
    int num;     /* Inode number */
    mode_t mode; /* File type and mode */
    uid_t uid;   /* User ID of owner */
    gid_t gid;   /* Group ID of owner */
    off_t size;  /* Total size, in bytes */
    int nlinks;  /* Number of links */

    time_t atim; /* Time of last access */
    time_t mtim; /* Time of last modification */
    time_t ctim; /* Time of last status change */

    off_t blocks[N_BLOCKS];
};

/* Directory entry */
struct wfs_dentry {
    char name[MAX_NAME];
    int num;
};
