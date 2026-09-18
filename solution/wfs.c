#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <stddef.h>
#include "wfs.h"

static int num_disks;
static int disk_fds[MAX_DISKS];
static char *disk_maps[MAX_DISKS];
static size_t disk_sizes[MAX_DISKS];
static struct wfs_sb *sb; /* superblock on disk 0 (after reorder) */
static int raid_mode;

static char *disk_at(int disk)
{
    return disk_maps[disk];
}

static unsigned char *inode_bitmap(int disk)
{
    return (unsigned char *)(disk_at(disk) + sb->i_bitmap_ptr);
}

static unsigned char *data_bitmap(int disk)
{
    return (unsigned char *)(disk_at(disk) + sb->d_bitmap_ptr);
}

static struct wfs_inode *inode_at(int disk, int inum)
{
    return (struct wfs_inode *)(disk_at(disk) + sb->i_blocks_ptr + (off_t)inum * BLOCK_SIZE);
}

static int bit_test(const unsigned char *bm, int bit)
{
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bit_set(unsigned char *bm, int bit)
{
    bm[bit / 8] |= (unsigned char)(1 << (bit % 8));
}

static void bit_clear(unsigned char *bm, int bit)
{
    bm[bit / 8] &= (unsigned char)~(1 << (bit % 8));
}

static void mirror_metadata_range(off_t offset, size_t len)
{
    for (int d = 1; d < num_disks; d++) {
        memcpy(disk_at(d) + offset, disk_at(0) + offset, len);
    }
}

static void sync_inode(int inum)
{
    off_t off = sb->i_blocks_ptr + (off_t)inum * BLOCK_SIZE;
    mirror_metadata_range(off, BLOCK_SIZE);
}

static void sync_inode_bitmap(void)
{
    mirror_metadata_range(sb->i_bitmap_ptr, sb->num_inodes / 8);
}

static void sync_data_bitmap_all(void)
{
    /* RAID1: bitmaps identical. RAID0: caller updates per-disk bits; still
       mirror only when we intentionally wrote disk 0's bitmap for RAID1. */
    for (int d = 1; d < num_disks; d++) {
        memcpy(data_bitmap(d), data_bitmap(0), sb->num_data_blocks / 8);
    }
}

static int allocate_inode(void)
{
    unsigned char *bm = inode_bitmap(0);
    for (size_t i = 0; i < sb->num_inodes; i++) {
        if (!bit_test(bm, (int)i)) {
            bit_set(bm, (int)i);
            sync_inode_bitmap();

            struct wfs_inode *ino = inode_at(0, (int)i);
            memset(ino, 0, sizeof(*ino));
            ino->num = (int)i;
            for (int b = 0; b < N_BLOCKS; b++) {
                ino->blocks[b] = -1;
            }
            sync_inode((int)i);
            return (int)i;
        }
    }
    return -ENOSPC;
}

static void free_inode(int inum)
{
    struct wfs_inode *ino = inode_at(0, inum);
    memset(ino, 0, sizeof(*ino));
    sync_inode(inum);
    bit_clear(inode_bitmap(0), inum);
    sync_inode_bitmap();
}

/* Map a logical data-block index to (disk, offset within that disk). */
static void map_data_block(size_t log_idx, int *disk_out, off_t *off_out)
{
    off_t off = sb->d_blocks_ptr + (off_t)log_idx * BLOCK_SIZE;
    if (raid_mode == RAID_0) {
        *disk_out = (int)(log_idx % (size_t)num_disks);
        *off_out = off;
    } else {
        *disk_out = 0;
        *off_out = off;
    }
}

static int allocate_data_block(void)
{
    if (raid_mode == RAID_0) {
        for (size_t i = 0; i < sb->num_data_blocks; i++) {
            int d = (int)(i % (size_t)num_disks);
            if (!bit_test(data_bitmap(d), (int)i)) {
                bit_set(data_bitmap(d), (int)i);
                memset(disk_at(d) + sb->d_blocks_ptr + (off_t)i * BLOCK_SIZE, 0, BLOCK_SIZE);
                return (int)i;
            }
        }
        return -1;
    }

    unsigned char *bm = data_bitmap(0);
    for (size_t i = 0; i < sb->num_data_blocks; i++) {
        if (!bit_test(bm, (int)i)) {
            bit_set(bm, (int)i);
            sync_data_bitmap_all();
            for (int d = 0; d < num_disks; d++) {
                memset(disk_at(d) + sb->d_blocks_ptr + (off_t)i * BLOCK_SIZE, 0, BLOCK_SIZE);
            }
            return (int)i;
        }
    }
    return -1;
}

static void free_data_block(size_t log_idx)
{
    if (raid_mode == RAID_0) {
        int d = (int)(log_idx % (size_t)num_disks);
        bit_clear(data_bitmap(d), (int)log_idx);
        return;
    }
    bit_clear(data_bitmap(0), (int)log_idx);
    sync_data_bitmap_all();
}

static size_t ptr_to_index(off_t ptr)
{
    return (size_t)((ptr - sb->d_blocks_ptr) / BLOCK_SIZE);
}

static off_t index_to_ptr(size_t idx)
{
    return sb->d_blocks_ptr + (off_t)idx * BLOCK_SIZE;
}

/* Read one data block (512 bytes) with RAID semantics into dst. */
static void read_data_block(size_t log_idx, void *dst)
{
    if (raid_mode == RAID_0) {
        int d;
        off_t off;
        map_data_block(log_idx, &d, &off);
        memcpy(dst, disk_at(d) + off, BLOCK_SIZE);
        return;
    }

    if (raid_mode == RAID_1) {
        memcpy(dst, disk_at(0) + sb->d_blocks_ptr + (off_t)log_idx * BLOCK_SIZE, BLOCK_SIZE);
        return;
    }

    /* RAID 1v: majority vote across disks; ties -> lowest index. */
    unsigned char copies[MAX_DISKS][BLOCK_SIZE];
    for (int d = 0; d < num_disks; d++) {
        memcpy(copies[d], disk_at(d) + sb->d_blocks_ptr + (off_t)log_idx * BLOCK_SIZE, BLOCK_SIZE);
    }

    int best = 0;
    int best_count = -1;
    for (int d = 0; d < num_disks; d++) {
        int count = 0;
        for (int e = 0; e < num_disks; e++) {
            if (memcmp(copies[d], copies[e], BLOCK_SIZE) == 0) {
                count++;
            }
        }
        if (count > best_count) {
            best_count = count;
            best = d;
        }
    }
    memcpy(dst, copies[best], BLOCK_SIZE);
}

static void write_data_block(size_t log_idx, const void *src)
{
    if (raid_mode == RAID_0) {
        int d;
        off_t off;
        map_data_block(log_idx, &d, &off);
        memcpy(disk_at(d) + off, src, BLOCK_SIZE);
        return;
    }

    for (int d = 0; d < num_disks; d++) {
        memcpy(disk_at(d) + sb->d_blocks_ptr + (off_t)log_idx * BLOCK_SIZE, src, BLOCK_SIZE);
    }
}

/* Get/create the log index for file block `file_blk` of an inode. */
static int inode_get_block(struct wfs_inode *ino, int file_blk, int create)
{
    if (file_blk < D_BLOCK) {
        if (ino->blocks[file_blk] == -1) {
            if (!create) {
                return -1;
            }
            int idx = allocate_data_block();
            if (idx < 0) {
                return -ENOSPC;
            }
            ino->blocks[file_blk] = index_to_ptr((size_t)idx);
            sync_inode(ino->num);
        }
        return (int)ptr_to_index(ino->blocks[file_blk]);
    }

    int indir_index = file_blk - D_BLOCK;
    if (indir_index >= (int)(BLOCK_SIZE / sizeof(off_t))) {
        return -EFBIG;
    }

    if (ino->blocks[IND_BLOCK] == -1) {
        if (!create) {
            return -1;
        }
        int idx = allocate_data_block();
        if (idx < 0) {
            return -ENOSPC;
        }
        ino->blocks[IND_BLOCK] = index_to_ptr((size_t)idx);
        /* Zero the indirect block (already zeroed on allocate). */
        sync_inode(ino->num);
    }

    off_t indirect_ptrs[BLOCK_SIZE / sizeof(off_t)];
    read_data_block(ptr_to_index(ino->blocks[IND_BLOCK]), indirect_ptrs);

    if (indirect_ptrs[indir_index] == 0 || indirect_ptrs[indir_index] == (off_t)-1) {
        if (!create) {
            return -1;
        }
        int idx = allocate_data_block();
        if (idx < 0) {
            return -ENOSPC;
        }
        indirect_ptrs[indir_index] = index_to_ptr((size_t)idx);
        write_data_block(ptr_to_index(ino->blocks[IND_BLOCK]), indirect_ptrs);
        /* Keep inode metadata (mtimes etc.) in sync; block pointer already set. */
        sync_inode(ino->num);
    }

    return (int)ptr_to_index(indirect_ptrs[indir_index]);
}

static void inode_free_all_blocks(struct wfs_inode *ino)
{
    for (int i = 0; i < D_BLOCK; i++) {
        if (ino->blocks[i] != -1) {
            free_data_block(ptr_to_index(ino->blocks[i]));
            ino->blocks[i] = -1;
        }
    }
    if (ino->blocks[IND_BLOCK] != -1) {
        off_t indirect_ptrs[BLOCK_SIZE / sizeof(off_t)];
        read_data_block(ptr_to_index(ino->blocks[IND_BLOCK]), indirect_ptrs);
        for (size_t i = 0; i < BLOCK_SIZE / sizeof(off_t); i++) {
            if (indirect_ptrs[i] != 0 && indirect_ptrs[i] != (off_t)-1) {
                free_data_block(ptr_to_index(indirect_ptrs[i]));
            }
        }
        free_data_block(ptr_to_index(ino->blocks[IND_BLOCK]));
        ino->blocks[IND_BLOCK] = -1;
    }
    sync_inode(ino->num);
}

/* Split path into parent path and final component. */
static int split_path(const char *path, char *parent, char *name)
{
    if (strcmp(path, "/") == 0) {
        return -1;
    }
    const char *last = strrchr(path, '/');
    if (!last) {
        return -ENOENT;
    }
    if (last == path) {
        strcpy(parent, "/");
    } else {
        size_t plen = (size_t)(last - path);
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    strncpy(name, last + 1, MAX_NAME);
    name[MAX_NAME - 1] = '\0';
    if (name[0] == '\0') {
        return -ENOENT;
    }
    return 0;
}

static int dentry_empty(const struct wfs_dentry *de)
{
    return de->name[0] == '\0';
}

/* Look up name in directory inode; return child inode number or -ENOENT. */
static int dir_lookup(struct wfs_inode *dir, const char *name)
{
    if (!S_ISDIR(dir->mode)) {
        return -ENOTDIR;
    }
    int nentries = (int)(dir->size / sizeof(struct wfs_dentry));
    for (int i = 0; i < nentries; i++) {
        int file_blk = i / (BLOCK_SIZE / (int)sizeof(struct wfs_dentry));
        int ent_off = i % (BLOCK_SIZE / (int)sizeof(struct wfs_dentry));
        int log_idx = inode_get_block(dir, file_blk, 0);
        if (log_idx < 0) {
            continue;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
        if (!dentry_empty(de) && strncmp(de->name, name, MAX_NAME) == 0) {
            return de->num;
        }
    }
    return -ENOENT;
}

static int path_to_inode(const char *path)
{
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int cur = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok != NULL; tok = strtok_r(NULL, "/", &save)) {
        struct wfs_inode *dir = inode_at(0, cur);
        int next = dir_lookup(dir, tok);
        if (next < 0) {
            return next;
        }
        cur = next;
    }
    return cur;
}

static int dir_add_entry(struct wfs_inode *dir, const char *name, int child_inum)
{
    int nentries = (int)(dir->size / sizeof(struct wfs_dentry));
    int slots_per_block = BLOCK_SIZE / (int)sizeof(struct wfs_dentry);

    /* Reuse a blank slot if present. */
    for (int i = 0; i < nentries; i++) {
        int file_blk = i / slots_per_block;
        int ent_off = i % slots_per_block;
        int log_idx = inode_get_block(dir, file_blk, 0);
        if (log_idx < 0) {
            continue;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
        if (dentry_empty(de)) {
            memset(de, 0, sizeof(*de));
            strncpy(de->name, name, MAX_NAME - 1);
            de->num = child_inum;
            write_data_block((size_t)log_idx, block);
            dir->mtim = dir->ctim = time(NULL);
            sync_inode(dir->num);
            return 0;
        }
    }

    /* Append a new entry (may allocate a new directory data block). */
    int new_index = nentries;
    int file_blk = new_index / slots_per_block;
    if (file_blk >= D_BLOCK) {
        return -ENOSPC;
    }
    int ent_off = new_index % slots_per_block;
    int log_idx = inode_get_block(dir, file_blk, 1);
    if (log_idx < 0) {
        return log_idx;
    }

    char block[BLOCK_SIZE];
    read_data_block((size_t)log_idx, block);
    struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
    memset(de, 0, sizeof(*de));
    strncpy(de->name, name, MAX_NAME - 1);
    de->num = child_inum;
    write_data_block((size_t)log_idx, block);

    dir->size = (off_t)(nentries + 1) * (off_t)sizeof(struct wfs_dentry);
    dir->mtim = dir->ctim = time(NULL);
    sync_inode(dir->num);
    return 0;
}

static int dir_remove_entry(struct wfs_inode *dir, const char *name)
{
    int nentries = (int)(dir->size / sizeof(struct wfs_dentry));
    int slots_per_block = BLOCK_SIZE / (int)sizeof(struct wfs_dentry);

    for (int i = 0; i < nentries; i++) {
        int file_blk = i / slots_per_block;
        int ent_off = i % slots_per_block;
        int log_idx = inode_get_block(dir, file_blk, 0);
        if (log_idx < 0) {
            continue;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
        if (!dentry_empty(de) && strncmp(de->name, name, MAX_NAME) == 0) {
            memset(de, 0, sizeof(*de));
            write_data_block((size_t)log_idx, block);
            dir->mtim = dir->ctim = time(NULL);
            sync_inode(dir->num);
            return 0;
        }
    }
    return -ENOENT;
}

static int create_node(const char *path, mode_t mode, int is_dir)
{
    char parent[1024], name[MAX_NAME];
    if (split_path(path, parent, name) < 0) {
        return -ENOENT;
    }

    int parent_inum = path_to_inode(parent);
    if (parent_inum < 0) {
        return parent_inum;
    }
    struct wfs_inode *pdir = inode_at(0, parent_inum);
    if (!S_ISDIR(pdir->mode)) {
        return -ENOTDIR;
    }
    if (dir_lookup(pdir, name) >= 0) {
        return -EEXIST;
    }

    int inum = allocate_inode();
    if (inum < 0) {
        return inum;
    }

    struct wfs_inode *ino = inode_at(0, inum);
    ino->num = inum;
    ino->mode = mode;
    ino->uid = getuid();
    ino->gid = getgid();
    ino->size = 0;
    ino->nlinks = is_dir ? 2 : 1;
    ino->atim = ino->mtim = ino->ctim = time(NULL);
    for (int i = 0; i < N_BLOCKS; i++) {
        ino->blocks[i] = -1;
    }
    sync_inode(inum);

    int rc = dir_add_entry(pdir, name, inum);
    if (rc < 0) {
        free_inode(inum);
        return rc;
    }
    if (is_dir) {
        pdir->nlinks++;
        sync_inode(parent_inum);
    }
    return 0;
}

static int wfs_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(*stbuf));
    int inum = path_to_inode(path);
    if (inum < 0) {
        return -ENOENT;
    }
    struct wfs_inode *ino = inode_at(0, inum);
    stbuf->st_uid = ino->uid;
    stbuf->st_gid = ino->gid;
    stbuf->st_atim.tv_sec = ino->atim;
    stbuf->st_mtim.tv_sec = ino->mtim;
    stbuf->st_ctim.tv_sec = ino->ctim;
    stbuf->st_mode = ino->mode;
    stbuf->st_size = ino->size;
    stbuf->st_nlink = (nlink_t)ino->nlinks;
    stbuf->st_blocks = (ino->size + 511) / 512;
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)dev;
    if (!S_ISREG(mode)) {
        mode = S_IFREG | (mode & 0777);
    }
    return create_node(path, mode, 0);
}

static int wfs_mkdir(const char *path, mode_t mode)
{
    return create_node(path, S_IFDIR | (mode & 0777), 1);
}

static int wfs_unlink(const char *path)
{
    char parent[1024], name[MAX_NAME];
    if (split_path(path, parent, name) < 0) {
        return -ENOENT;
    }
    int parent_inum = path_to_inode(parent);
    if (parent_inum < 0) {
        return parent_inum;
    }
    struct wfs_inode *pdir = inode_at(0, parent_inum);
    int inum = dir_lookup(pdir, name);
    if (inum < 0) {
        return inum;
    }
    struct wfs_inode *ino = inode_at(0, inum);
    if (S_ISDIR(ino->mode)) {
        return -EISDIR;
    }

    inode_free_all_blocks(ino);
    free_inode(inum);
    return dir_remove_entry(pdir, name);
}

static int wfs_rmdir(const char *path)
{
    char parent[1024], name[MAX_NAME];
    if (split_path(path, parent, name) < 0) {
        return -ENOENT;
    }
    int parent_inum = path_to_inode(parent);
    if (parent_inum < 0) {
        return parent_inum;
    }
    struct wfs_inode *pdir = inode_at(0, parent_inum);
    int inum = dir_lookup(pdir, name);
    if (inum < 0) {
        return inum;
    }
    struct wfs_inode *ino = inode_at(0, inum);
    if (!S_ISDIR(ino->mode)) {
        return -ENOTDIR;
    }

    /* Presume empty: still free any allocated dir data blocks. */
    int nentries = (int)(ino->size / sizeof(struct wfs_dentry));
    for (int i = 0; i < nentries; i++) {
        int slots = BLOCK_SIZE / (int)sizeof(struct wfs_dentry);
        int file_blk = i / slots;
        int ent_off = i % slots;
        int log_idx = inode_get_block(ino, file_blk, 0);
        if (log_idx < 0) {
            continue;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
        if (!dentry_empty(de)) {
            return -ENOTEMPTY;
        }
    }

    inode_free_all_blocks(ino);
    free_inode(inum);
    pdir->nlinks--;
    sync_inode(parent_inum);
    return dir_remove_entry(pdir, name);
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;
    int inum = path_to_inode(path);
    if (inum < 0) {
        return inum;
    }
    struct wfs_inode *ino = inode_at(0, inum);
    if (S_ISDIR(ino->mode)) {
        return -EISDIR;
    }
    if (offset >= ino->size) {
        return 0;
    }
    if (offset + (off_t)size > ino->size) {
        size = (size_t)(ino->size - offset);
    }

    size_t done = 0;
    while (done < size) {
        off_t pos = offset + (off_t)done;
        int file_blk = (int)(pos / BLOCK_SIZE);
        int blk_off = (int)(pos % BLOCK_SIZE);
        size_t chunk = BLOCK_SIZE - (size_t)blk_off;
        if (chunk > size - done) {
            chunk = size - done;
        }
        int log_idx = inode_get_block(ino, file_blk, 0);
        if (log_idx < 0) {
            memset(buf + done, 0, chunk);
        } else {
            char block[BLOCK_SIZE];
            read_data_block((size_t)log_idx, block);
            memcpy(buf + done, block + blk_off, chunk);
        }
        done += chunk;
    }
    ino->atim = time(NULL);
    sync_inode(inum);
    return (int)done;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    (void)fi;
    int inum = path_to_inode(path);
    if (inum < 0) {
        return inum;
    }
    struct wfs_inode *ino = inode_at(0, inum);
    if (S_ISDIR(ino->mode)) {
        return -EISDIR;
    }

    size_t done = 0;
    while (done < size) {
        off_t pos = offset + (off_t)done;
        int file_blk = (int)(pos / BLOCK_SIZE);
        int blk_off = (int)(pos % BLOCK_SIZE);
        size_t chunk = BLOCK_SIZE - (size_t)blk_off;
        if (chunk > size - done) {
            chunk = size - done;
        }
        int log_idx = inode_get_block(ino, file_blk, 1);
        if (log_idx < 0) {
            return log_idx == -1 ? -ENOSPC : log_idx;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        memcpy(block + blk_off, buf + done, chunk);
        write_data_block((size_t)log_idx, block);
        done += chunk;
    }

    if (offset + (off_t)size > ino->size) {
        ino->size = offset + (off_t)size;
    }
    ino->mtim = ino->ctim = time(NULL);
    sync_inode(inum);
    return (int)done;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
    (void)offset;
    (void)fi;
    int inum = path_to_inode(path);
    if (inum < 0) {
        return inum;
    }
    struct wfs_inode *dir = inode_at(0, inum);
    if (!S_ISDIR(dir->mode)) {
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int nentries = (int)(dir->size / sizeof(struct wfs_dentry));
    int slots = BLOCK_SIZE / (int)sizeof(struct wfs_dentry);
    for (int i = 0; i < nentries; i++) {
        int file_blk = i / slots;
        int ent_off = i % slots;
        int log_idx = inode_get_block(dir, file_blk, 0);
        if (log_idx < 0) {
            continue;
        }
        char block[BLOCK_SIZE];
        read_data_block((size_t)log_idx, block);
        struct wfs_dentry *de = (struct wfs_dentry *)block + ent_off;
        if (!dentry_empty(de)) {
            filler(buf, de->name, NULL, 0);
        }
    }
    dir->atim = time(NULL);
    sync_inode(inum);
    return 0;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

static int open_and_map(const char *path, int index)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    disk_fds[index] = fd;
    disk_maps[index] = map;
    disk_sizes[index] = (size_t)st.st_size;
    return 0;
}

static int reorder_disks(void)
{
    /* Read IDs from each mapped disk and sort into canonical order. */
    struct wfs_sb *first = (struct wfs_sb *)disk_maps[0];
    int n = first->num_disks;
    if (n != num_disks) {
        fprintf(stderr, "Error: not enough disks.\n");
        return -1;
    }

    char *maps_tmp[MAX_DISKS];
    int fds_tmp[MAX_DISKS];
    size_t sizes_tmp[MAX_DISKS];
    int used[MAX_DISKS];
    memset(used, 0, sizeof(used));

    for (int want = 0; want < n; want++) {
        uint64_t want_id = first->disk_ids[want];
        /* disk_ids should be identical on all disks; use any disk's copy. */
        int found = -1;
        for (int d = 0; d < num_disks; d++) {
            if (used[d]) {
                continue;
            }
            struct wfs_sb *s = (struct wfs_sb *)disk_maps[d];
            if (s->this_disk_id == want_id) {
                found = d;
                break;
            }
        }
        if (found < 0) {
            /* Fallback: try matching against every disk's disk_ids list. */
            for (int src = 0; src < num_disks && found < 0; src++) {
                struct wfs_sb *s = (struct wfs_sb *)disk_maps[src];
                want_id = s->disk_ids[want];
                for (int d = 0; d < num_disks; d++) {
                    if (used[d]) {
                        continue;
                    }
                    struct wfs_sb *t = (struct wfs_sb *)disk_maps[d];
                    if (t->this_disk_id == want_id) {
                        found = d;
                        break;
                    }
                }
            }
        }
        if (found < 0) {
            fprintf(stderr, "Error: not enough disks.\n");
            return -1;
        }
        used[found] = 1;
        maps_tmp[want] = disk_maps[found];
        fds_tmp[want] = disk_fds[found];
        sizes_tmp[want] = disk_sizes[found];
    }

    for (int i = 0; i < n; i++) {
        disk_maps[i] = maps_tmp[i];
        disk_fds[i] = fds_tmp[i];
        disk_sizes[i] = sizes_tmp[i];
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s disk1 disk2 [FUSE options] mount_point\n", argv[0]);
        return 1;
    }

    /* Disks are arguments before the first FUSE option (starts with '-'),
       except the final argument which is always the mount point when no
       dash-options are present. */
    int first_fuse = -1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            first_fuse = i;
            break;
        }
    }

    char *disk_paths[MAX_DISKS];
    if (first_fuse < 0) {
        num_disks = argc - 2; /* all but prog and mount */
        if (num_disks < 1) {
            return 1;
        }
        for (int i = 0; i < num_disks; i++) {
            disk_paths[i] = argv[1 + i];
        }
    } else {
        num_disks = first_fuse - 1;
        if (num_disks < 1) {
            return 1;
        }
        for (int i = 0; i < num_disks; i++) {
            disk_paths[i] = argv[1 + i];
        }
    }

    if (num_disks > MAX_DISKS) {
        return 1;
    }

    for (int i = 0; i < num_disks; i++) {
        if (open_and_map(disk_paths[i], i) < 0) {
            fprintf(stderr, "Error: cannot open disk %s\n", disk_paths[i]);
            return -1;
        }
    }

    if (reorder_disks() < 0) {
        return -1;
    }

    sb = (struct wfs_sb *)disk_maps[0];
    raid_mode = sb->raid_mode;
    if (sb->num_disks != num_disks) {
        fprintf(stderr, "Error: not enough disks.\n");
        return -1;
    }

    /* Build argv for fuse_main: program name + FUSE options + mount point. */
    char *fuse_argv[64];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    if (first_fuse < 0) {
        fuse_argv[fuse_argc++] = argv[argc - 1];
    } else {
        for (int i = first_fuse; i < argc; i++) {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }

    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
