import sys

class WfsState:
    blksize = 512
    superblock = [('inodes', 8), ('datablocks', 8), ('ibit', 8), ('dbit', 8),
                  ('iblocks', 8), ('dblocks', 8)]
    inode = [('num', 4), ('mode', 4), ('uid', 4), ('gid', 4), ('size', 8),
             ('nlinks', 8), ('atim', 8), ('mtim', 8), ('ctim', 8), ('blocks', 64)]

    def __init__(self, disk):
        self.disk = disk
        self.sb = self.read_superblock()

    def diskname(self):
        return self.disk

    def read_struct(self, loc, struct):
        """Read a struct from disk and parse into a dict."""
        with open(self.disk, "rb") as diskf:
            diskf.seek(loc)
            dat = diskf.read(sum(size for _, size in struct))
            return {
                name: int.from_bytes(dat[offset:offset + size], sys.byteorder)
                for offset, (name, size) in zip(
                        [sum(s for _, s in struct[:i]) for i in range(len(struct))],
                        struct)
            }

    def list_allocations(self, bitmap):
        """Return a list of all the allocated positions in a bitmap."""
        return [bytep * 8 + bitp for bytep, byte in enumerate(bitmap)
                for bitp in range(8) if byte & (1 << bitp)]

    def list_allocated_inodes(self):
        """Return a list of allocated inodes."""
        with open(self.disk, "rb") as diskf:
            diskf.seek(self.get_ibit())
            ibit = diskf.read(int(self.get_sb_inodes() / 8))
            return self.list_allocations(ibit)

    def list_allocated_datablocks(self):
        """Return a list of allocated data blocks."""
        with open(self.disk, "rb") as diskf:
            diskf.seek(self.get_dbit())
            dbit = diskf.read(int(self.get_sb_datablocks() / 8))
            return self.list_allocations(dbit)

    def read_inode(self, inodep):
        """Read an inode from disk and return a dict of its fields."""
        pos = self.get_iblock_region() + (inodep * self.blksize)
        return self.read_struct(pos, self.inode)

    def read_superblock(self):
        """Read a superblock from disk and return a dict of its fields."""
        return self.read_struct(0, self.superblock)

    def read_inode_region(self):
        """Read and return the entire inode region of the disk."""
        with open(self.disk, "rb") as diskf:
            diskf.seek(self.get_iblock_region())
            return diskf.read(self.get_sb_inodes() * self.blksize)

    def read_datablock_region(self):
        """Read and return the entire data region of the disk."""
        with open(self.disk, "rb") as diskf:
            diskf.seek(self.get_dblock_region())
            return diskf.read(self.get_sb_datablocks() * self.blksize)

    def clear_datablock_region(self):
        """Overwrite the entire data region of the disk with zeros."""
        with open(self.disk, "r+b") as diskf:
            diskf.seek(self.get_dblock_region() + self.blksize)
            diskf.write(b'\x00' * ((self.get_sb_datablocks() - 1) * self.blksize))

    def get_sb_inodes(self):
        """Return the total number of inodes in the filesystem."""
        return self.sb['inodes']

    def get_sb_datablocks(self):
        """Return the number of data blocks on this disk."""
        return self.sb['datablocks']

    def get_ibit(self):
        """Return the offset of the inode bitmap."""
        return self.sb['ibit']

    def get_dbit(self):
        """Return the offset of the data bitmap."""
        return self.sb['dbit']

    def get_iblock_region(self):
        """Return the offset of the inode region."""
        return self.sb['iblocks']

    def get_dblock_region(self):
        """Return the offset of the data block region."""
        return self.sb['dblocks']

    def get_sb_size(self):
        """Return the size of the superblock."""
        return sum(size for _, size in self.superblock)
        
