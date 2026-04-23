// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ufs.c
// Read-only UFS-1 (BSD FFS) walker.  The superblock captures the cylinder
// group geometry and block-address scaling; reads are done on demand by
// translating inode number -> cylinder group -> fragment -> byte offset,
// and direct/indirect block chains -> fragment -> byte offset.  No
// persistent snapshot of the filesystem is loaded at open time — A/UX
// volumes can reach 35 000+ inodes, but `cp -r` touches each one at most
// once, so per-inode disk reads are the right granularity.

#include "image_ufs.h"
#include "image.h"
#include "storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Disk-read helper (byte-granular over 512-aligned blocks) ------------

static int disk_read_bytes(image_t *img, uint64_t off, void *buf, size_t n) {
    uint8_t *dst = buf;
    size_t done = 0;
    uint8_t blk[STORAGE_BLOCK_SIZE];
    while (done < n) {
        uint64_t abs = off + done;
        uint64_t block_off = abs & ~(uint64_t)(STORAGE_BLOCK_SIZE - 1);
        size_t in_block = (size_t)(abs - block_off);
        size_t take = STORAGE_BLOCK_SIZE - in_block;
        if (take > n - done)
            take = n - done;
        if (in_block == 0 && take == STORAGE_BLOCK_SIZE) {
            if (disk_read_data(img, (size_t)block_off, dst + done, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
        } else {
            if (disk_read_data(img, (size_t)block_off, blk, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
            memcpy(dst + done, blk + in_block, take);
        }
        done += take;
    }
    return 0;
}

// ---- Big-endian helpers --------------------------------------------------

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int16_t be16s(const uint8_t *p) {
    return (int16_t)be16(p);
}

// ---- Superblock offsets ---------------------------------------------------
// Layout matches 4.3BSD-Tahoe struct fs; field offsets are stable in every
// A/UX UFS the project cares about.

#define SB_OFF_SBLKNO   8
#define SB_OFF_CBLKNO   12
#define SB_OFF_IBLKNO   16
#define SB_OFF_DBLKNO   20
#define SB_OFF_CGOFFSET 24
#define SB_OFF_CGMASK   28
#define SB_OFF_NCG      44
#define SB_OFF_BSIZE    48
#define SB_OFF_FSIZE    52
#define SB_OFF_FRAG     56
#define SB_OFF_FSBTODB  100
#define SB_OFF_NINDIR   116
#define SB_OFF_INOPB    120
#define SB_OFF_IPG      184
#define SB_OFF_FPG      188
#define SB_OFF_MAGIC    1372

// ---- Dinode offsets ------------------------------------------------------
// 4.3BSD-Tahoe dinode.  Size is 128 bytes on disk.
#define DI_OFF_MODE  0
#define DI_OFF_NLINK 2
#define DI_OFF_UID   4
#define DI_OFF_GID   6
// di_qsize at offset 8 is a struct quad { long val[2]; }.  val[0] is
// di_rdev; val[1] is di_size (Tahoe convention).
#define DI_OFF_RDEV   8
#define DI_OFF_SIZE   12
#define DI_OFF_ATIME  16
#define DI_OFF_MTIME  24
#define DI_OFF_CTIME  32
#define DI_OFF_DB     40 // 12 x int32 = 48 bytes
#define DI_OFF_IB     88 // 3 x int32 = 12 bytes
#define DI_OFF_FLAGS  100
#define DI_OFF_BLOCKS 104
#define DI_OFF_GEN    108
#define DI_SIZE       128

#define UFS_NDADDR 12
#define UFS_NIADDR 3

// S_IFMT bits as BSD writes them in di_mode.
#define UFS_IFMT   0xF000
#define UFS_IFDIR  0x4000
#define UFS_IFREG  0x8000
#define UFS_IFLNK  0xA000
#define UFS_IFCHR  0x2000
#define UFS_IFBLK  0x6000
#define UFS_IFIFO  0x1000
#define UFS_IFSOCK 0xC000

// ---- Volume state --------------------------------------------------------

struct ufs_volume {
    image_t *img;
    uint64_t partition_off;
    uint64_t partition_size;

    uint32_t bsize; // file system block size (e.g. 8192)
    uint32_t fsize; // fragment size (e.g. 1024)
    uint32_t frag; // frags per block (e.g. 8)
    uint32_t ncg; // number of cylinder groups
    uint32_t ipg; // inodes per cylinder group
    uint32_t fpg; // fragments per cylinder group
    uint32_t iblkno; // inode-block offset within cg (in frags)
    int32_t cgoffset;
    int32_t cgmask;
    uint32_t nindir; // block pointers per indirect block
    uint32_t inopb; // inodes per fs block (bsize / DI_SIZE)
    uint32_t fsbtodb; // shift: frags to 512-byte sectors
};

struct ufs_dir_iter {
    ufs_volume_t *vol;
    uint32_t ino;
    uint8_t *dir_buf; // buffered copy of the directory contents
    uint64_t dir_size; // logical size
    uint64_t cursor; // byte offset within dir_buf
};

// ---- Small utilities -----------------------------------------------------

// Read raw bytes from the partition (relative to partition start).
static int read_partition(ufs_volume_t *vol, uint64_t off, void *buf, size_t n) {
    if (off + n > vol->partition_size)
        return -EIO;
    return disk_read_bytes(vol->img, vol->partition_off + off, buf, n);
}

// Compute the starting fragment number of cylinder group `c`.
static uint64_t cgstart(const ufs_volume_t *vol, uint32_t c) {
    uint64_t base = (uint64_t)vol->fpg * (uint64_t)c;
    uint32_t cg_skew = (uint32_t)c & (~(uint32_t)vol->cgmask);
    base += (uint64_t)vol->cgoffset * cg_skew;
    return base;
}

// Byte offset of the dinode for `ino` within the partition.
static uint64_t inode_byte_offset(const ufs_volume_t *vol, uint32_t ino) {
    uint32_t cg = ino / vol->ipg;
    uint32_t within = ino % vol->ipg;
    uint64_t frag = cgstart(vol, cg) + vol->iblkno;
    return frag * (uint64_t)vol->fsize + (uint64_t)within * DI_SIZE;
}

// Load one dinode into `di[DI_SIZE]`.
static int load_dinode(ufs_volume_t *vol, uint32_t ino, uint8_t *di) {
    if (ino < UFS_ROOT_INO || ino >= vol->ncg * vol->ipg)
        return -ENOENT;
    return read_partition(vol, inode_byte_offset(vol, ino), di, DI_SIZE);
}

// ---- Block address resolution -------------------------------------------

// Translate a logical file byte offset into a physical partition byte offset
// for the file owned by dinode buffer `di`.  `want_len` is how many bytes
// the caller would like starting at `off`; we return the contiguous length
// (may be less than requested if a block boundary is hit) and the physical
// offset via *out_phys.  Returns 0 on hole (zero-fill), >0 contiguous
// bytes mapped, <0 on error.
//
// Addresses in UFS live in fragment units.  Direct blocks are always full
// blocks (fs_frag frags) except possibly the last one, which may be a
// short tail allocated as fewer frags.  Indirect blocks contain 32-bit
// fragment numbers pointing at full fs_bsize blocks.

static int resolve_block(ufs_volume_t *vol, const uint8_t *di, uint64_t off, size_t want_len, uint64_t *out_phys,
                         size_t *out_len) {
    (void)want_len;
    uint32_t bsize = vol->bsize;
    uint32_t lbn = (uint32_t)(off / bsize); // logical block index
    uint32_t in_blk = (uint32_t)(off % bsize);

    uint32_t frag_addr = 0;

    if (lbn < UFS_NDADDR) {
        frag_addr = be32(di + DI_OFF_DB + lbn * 4);
    } else {
        uint32_t nindir = vol->nindir; // entries per indirect block
        uint32_t rel = lbn - UFS_NDADDR;
        if (rel < nindir) {
            // Single indirect: di_ib[0] -> block of uint32[nindir].
            uint32_t ib1 = be32(di + DI_OFF_IB + 0 * 4);
            if (ib1 == 0) {
                *out_phys = 0;
                *out_len = bsize - in_blk;
                return 0;
            }
            uint64_t ib_byte = (uint64_t)ib1 * vol->fsize;
            uint8_t slot[4];
            int rc = read_partition(vol, ib_byte + rel * 4, slot, 4);
            if (rc < 0)
                return rc;
            frag_addr = be32(slot);
        } else if (rel < nindir + (uint64_t)nindir * nindir) {
            // Double indirect.
            uint32_t ib2 = be32(di + DI_OFF_IB + 1 * 4);
            if (ib2 == 0) {
                *out_phys = 0;
                *out_len = bsize - in_blk;
                return 0;
            }
            uint32_t r2 = rel - nindir;
            uint32_t idx_lo = r2 % nindir;
            uint32_t idx_hi = r2 / nindir;
            uint64_t l1_byte = (uint64_t)ib2 * vol->fsize + idx_hi * 4;
            uint8_t slot[4];
            int rc = read_partition(vol, l1_byte, slot, 4);
            if (rc < 0)
                return rc;
            uint32_t ib1 = be32(slot);
            if (ib1 == 0) {
                *out_phys = 0;
                *out_len = bsize - in_blk;
                return 0;
            }
            uint64_t l2_byte = (uint64_t)ib1 * vol->fsize + idx_lo * 4;
            rc = read_partition(vol, l2_byte, slot, 4);
            if (rc < 0)
                return rc;
            frag_addr = be32(slot);
        } else {
            // Triple indirect: not supported in v1.
            *out_phys = 0;
            *out_len = bsize - in_blk;
            return 0;
        }
    }

    if (frag_addr == 0) {
        *out_phys = 0;
        *out_len = bsize - in_blk;
        return 0;
    }
    *out_phys = (uint64_t)frag_addr * vol->fsize + in_blk;
    *out_len = bsize - in_blk;
    return 1;
}

// Read `n` bytes from file with dinode `di` at logical offset `off` into
// `buf`.  Handles holes (zero-fill) and clamps against di_size.  Returns
// the number of bytes actually filled.
static int read_file_by_dinode(ufs_volume_t *vol, const uint8_t *di, uint64_t off, void *buf, size_t n, size_t *nread) {
    uint32_t size = be32(di + DI_OFF_SIZE);
    if (nread)
        *nread = 0;
    if (off >= size)
        return 0;
    uint64_t remaining = (uint64_t)size - off;
    if (n > remaining)
        n = (size_t)remaining;

    uint8_t *dst = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t phys = 0;
        size_t seg = 0;
        int rc = resolve_block(vol, di, off + done, n - done, &phys, &seg);
        if (rc < 0)
            return rc;
        size_t take = seg < (n - done) ? seg : (n - done);
        if (rc == 0) {
            memset(dst + done, 0, take);
        } else {
            rc = read_partition(vol, phys, dst + done, take);
            if (rc < 0)
                return rc;
        }
        done += take;
    }
    if (nread)
        *nread = done;
    return 0;
}

// ---- Superblock probe + open --------------------------------------------

bool ufs_probe(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size) {
    if (!img || partition_byte_size < UFS_SBOFF + 2048)
        return false;
    uint8_t buf[2048];
    if (disk_read_bytes(img, partition_byte_offset + UFS_SBOFF, buf, sizeof(buf)) < 0)
        return false;
    // Magic at offset 1372; accept either endianness.
    uint32_t be = be32(buf + SB_OFF_MAGIC);
    uint32_t le = (uint32_t)buf[SB_OFF_MAGIC] | ((uint32_t)buf[SB_OFF_MAGIC + 1] << 8) |
                  ((uint32_t)buf[SB_OFF_MAGIC + 2] << 16) | ((uint32_t)buf[SB_OFF_MAGIC + 3] << 24);
    return be == UFS_FS_MAGIC || le == UFS_FS_MAGIC;
}

ufs_volume_t *ufs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size) {
    if (!img || partition_byte_size < UFS_SBOFF + 2048)
        return NULL;
    uint8_t sb[2048];
    if (disk_read_bytes(img, partition_byte_offset + UFS_SBOFF, sb, sizeof(sb)) < 0)
        return NULL;
    if (be32(sb + SB_OFF_MAGIC) != UFS_FS_MAGIC) {
        // A/UX always writes BE, but we tolerate LE-rewritten images — not
        // yet implemented because no test fixture needs it.  Document and
        // bail out for now.
        return NULL;
    }

    ufs_volume_t *vol = calloc(1, sizeof(*vol));
    if (!vol)
        return NULL;
    vol->img = img;
    vol->partition_off = partition_byte_offset;
    vol->partition_size = partition_byte_size;

    vol->bsize = be32(sb + SB_OFF_BSIZE);
    vol->fsize = be32(sb + SB_OFF_FSIZE);
    vol->frag = be32(sb + SB_OFF_FRAG);
    vol->ncg = be32(sb + SB_OFF_NCG);
    vol->ipg = be32(sb + SB_OFF_IPG);
    vol->fpg = be32(sb + SB_OFF_FPG);
    vol->iblkno = be32(sb + SB_OFF_IBLKNO);
    vol->cgoffset = (int32_t)be32(sb + SB_OFF_CGOFFSET);
    vol->cgmask = (int32_t)be32(sb + SB_OFF_CGMASK);
    vol->nindir = be32(sb + SB_OFF_NINDIR);
    vol->inopb = be32(sb + SB_OFF_INOPB);
    vol->fsbtodb = be32(sb + SB_OFF_FSBTODB);

    // Sanity checks.  Reject obviously-corrupt values rather than reading
    // random bytes on subsequent calls.
    if (vol->bsize == 0 || vol->fsize == 0 || vol->frag == 0 || vol->bsize % 512 != 0 || vol->fsize % 512 != 0 ||
        vol->bsize / vol->fsize != vol->frag || vol->ncg == 0 || vol->ipg == 0 || vol->fpg == 0 || vol->nindir == 0 ||
        vol->nindir > 4096) {
        free(vol);
        return NULL;
    }
    return vol;
}

void ufs_close(ufs_volume_t *vol) {
    free(vol);
}

// ---- Directory walking ---------------------------------------------------

// Load the full contents of directory inode `ino` into memory.  Returns a
// malloc'd buffer of exactly di_size bytes; caller frees.  NULL on error.
static int load_directory(ufs_volume_t *vol, uint32_t ino, uint8_t **out_buf, uint64_t *out_size) {
    uint8_t di[DI_SIZE];
    int rc = load_dinode(vol, ino, di);
    if (rc < 0)
        return rc;
    uint16_t mode = be16(di + DI_OFF_MODE);
    if ((mode & UFS_IFMT) != UFS_IFDIR)
        return -ENOTDIR;
    uint32_t size = be32(di + DI_OFF_SIZE);
    if (size == 0) {
        *out_buf = NULL;
        *out_size = 0;
        return 0;
    }
    // Directories are typically one block; cap at 4 MiB to avoid runaway
    // reads on corruption.
    if (size > 4 * 1024 * 1024)
        return -EFBIG;
    uint8_t *buf = malloc(size);
    if (!buf)
        return -ENOMEM;
    size_t got = 0;
    rc = read_file_by_dinode(vol, di, 0, buf, size, &got);
    if (rc < 0 || got != size) {
        free(buf);
        return rc < 0 ? rc : -EIO;
    }
    *out_buf = buf;
    *out_size = size;
    return 0;
}

// Parse a single directory entry starting at `buf + off`.  Returns the
// reclen, or <= 0 on malformed entry.  Populates *d_ino, *d_namlen,
// *name_ptr on success.
static int parse_direct(const uint8_t *buf, uint64_t bufsize, uint64_t off, uint32_t *d_ino, uint16_t *d_namlen,
                        const char **name_ptr) {
    if (off + 8 > bufsize)
        return -1;
    uint32_t ino = be32(buf + off);
    uint16_t reclen = be16(buf + off + 4);
    uint16_t namlen = be16(buf + off + 6);
    if (reclen < 8 || reclen > bufsize - off || namlen > reclen - 8)
        return -1;
    *d_ino = ino;
    *d_namlen = namlen;
    *name_ptr = (const char *)(buf + off + 8);
    return reclen;
}

// ---- Public: lookup / readdir / read ------------------------------------

// Fill a ufs_dirent_t for the given inode number.
static int fill_dirent_for_inode(ufs_volume_t *vol, uint32_t ino, const char *name, ufs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->ino = ino;
    uint8_t di[DI_SIZE];
    int rc = load_dinode(vol, ino, di);
    if (rc < 0)
        return rc;
    uint16_t mode = be16(di + DI_OFF_MODE);
    out->mode = mode;
    uint32_t ftype = mode & UFS_IFMT;
    out->is_dir = (ftype == UFS_IFDIR);
    out->is_symlink = (ftype == UFS_IFLNK);
    if (ftype == UFS_IFREG || ftype == UFS_IFLNK)
        out->size = be32(di + DI_OFF_SIZE);
    return 0;
}

// Case-sensitive byte comparison — UFS is case-sensitive.
static bool name_equals(const char *a, size_t la, const char *b) {
    size_t lb = strlen(b);
    if (la != lb)
        return false;
    return memcmp(a, b, la) == 0;
}

// Find a child with name `want` under directory inode `dir_ino`.  Returns
// the child's inode number on hit, 0 on miss, negative on error.
static int lookup_child(ufs_volume_t *vol, uint32_t dir_ino, const char *want, uint32_t *out_child_ino) {
    uint8_t *buf = NULL;
    uint64_t size = 0;
    int rc = load_directory(vol, dir_ino, &buf, &size);
    if (rc < 0)
        return rc;
    uint64_t off = 0;
    while (off < size) {
        uint32_t d_ino = 0;
        uint16_t d_namlen = 0;
        const char *name = NULL;
        int reclen = parse_direct(buf, size, off, &d_ino, &d_namlen, &name);
        if (reclen <= 0)
            break;
        if (d_ino != 0 && name_equals(name, d_namlen, want)) {
            *out_child_ino = d_ino;
            free(buf);
            return 1;
        }
        off += (uint64_t)reclen;
    }
    free(buf);
    *out_child_ino = 0;
    return 0;
}

int ufs_lookup(ufs_volume_t *vol, const char *const *components, size_t nc, ufs_dirent_t *out) {
    if (!vol || !out)
        return -EINVAL;
    if (nc == 0) {
        return fill_dirent_for_inode(vol, UFS_ROOT_INO, "/", out);
    }
    uint32_t cur = UFS_ROOT_INO;
    for (size_t i = 0; i < nc; i++) {
        uint32_t child = 0;
        int rc = lookup_child(vol, cur, components[i], &child);
        if (rc < 0)
            return rc;
        if (rc == 0 || child == 0)
            return -ENOENT;
        if (i + 1 < nc) {
            // Descend — must be a directory.
            uint8_t di[DI_SIZE];
            rc = load_dinode(vol, child, di);
            if (rc < 0)
                return rc;
            uint16_t mode = be16(di + DI_OFF_MODE);
            if ((mode & UFS_IFMT) != UFS_IFDIR)
                return -ENOTDIR;
        }
        cur = child;
    }
    return fill_dirent_for_inode(vol, cur, components[nc - 1], out);
}

ufs_dir_iter_t *ufs_opendir_ino(ufs_volume_t *vol, uint32_t ino) {
    if (!vol)
        return NULL;
    ufs_dir_iter_t *iter = calloc(1, sizeof(*iter));
    if (!iter)
        return NULL;
    iter->vol = vol;
    iter->ino = ino;
    int rc = load_directory(vol, ino, &iter->dir_buf, &iter->dir_size);
    if (rc < 0) {
        free(iter);
        return NULL;
    }
    return iter;
}

int ufs_readdir_next(ufs_dir_iter_t *iter, ufs_dirent_t *out) {
    if (!iter || !out)
        return -EINVAL;
    while (iter->cursor < iter->dir_size) {
        uint32_t d_ino = 0;
        uint16_t d_namlen = 0;
        const char *name = NULL;
        int reclen = parse_direct(iter->dir_buf, iter->dir_size, iter->cursor, &d_ino, &d_namlen, &name);
        if (reclen <= 0) {
            iter->cursor = iter->dir_size;
            return 0;
        }
        iter->cursor += (uint64_t)reclen;
        if (d_ino == 0)
            continue;
        // Skip "." and ".." entries.
        if ((d_namlen == 1 && name[0] == '.') || (d_namlen == 2 && name[0] == '.' && name[1] == '.'))
            continue;
        char tmp[256];
        size_t copy_len = d_namlen < sizeof(tmp) - 1 ? d_namlen : sizeof(tmp) - 1;
        memcpy(tmp, name, copy_len);
        tmp[copy_len] = '\0';
        int rc = fill_dirent_for_inode(iter->vol, d_ino, tmp, out);
        if (rc < 0) {
            // Orphan entry pointing at a bad inode — skip it rather than
            // aborting the whole readdir.
            continue;
        }
        return 1;
    }
    return 0;
}

void ufs_closedir_iter(ufs_dir_iter_t *iter) {
    if (!iter)
        return;
    free(iter->dir_buf);
    free(iter);
}

int ufs_read_file(ufs_volume_t *vol, uint32_t ino, uint64_t off, void *buf, size_t n, size_t *nread) {
    if (!vol || !buf)
        return -EINVAL;
    uint8_t di[DI_SIZE];
    int rc = load_dinode(vol, ino, di);
    if (rc < 0) {
        if (nread)
            *nread = 0;
        return rc;
    }
    uint16_t mode = be16(di + DI_OFF_MODE);
    uint32_t ftype = mode & UFS_IFMT;
    if (ftype == UFS_IFDIR) {
        if (nread)
            *nread = 0;
        return -EISDIR;
    }
    // For device nodes / FIFOs / sockets di_size is 0, so read_file_by_dinode
    // immediately returns 0 bytes — which matches the semantics a recursive
    // copy wants (treat the special file as an empty file rather than an
    // error; preserving device numbers is out of scope for v1).
    (void)be16s;
    return read_file_by_dinode(vol, di, off, buf, n, nread);
}
