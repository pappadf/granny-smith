// HFS Plus catalog-walker unit tests.
//
// image_hfs.c's only dependency on the storage stack is disk_read_data()
// over an opaque image_t, so we back it with an in-memory buffer holding a
// hand-built HFS+ volume (per Apple TN1150) and drive the public API
// directly — no real image/storage layer required.  The fixture is a
// minimal but spec-faithful volume:
//
//   Volume "TestVol"  (blockSize = nodeSize = 4096)
//     Sub/          (empty folder)
//     Hello         (data fork "Hello World", 11 bytes)
//     Bullet•       (empty file; non-ASCII UTF-16 name → 3-byte UTF-8)
//
// Allocation-block layout (4096-byte blocks):
//   block 0  boot area; HFS+ Volume Header sits at offset 1024
//   block 1  catalog B-tree node 0 (header node)
//   block 2  catalog B-tree node 1 (leaf node)
//   block 3  "Hello" data fork

#include "image_hfs.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ---- In-memory image backing ----------------------------------------------

#define BLK      4096u
#define IMG_SIZE (4u * BLK)

static uint8_t g_img[IMG_SIZE];

// Stub the one storage entry point image_hfs.c uses.  disk_read_data wants a
// 512-aligned offset+length; our fixture only ever issues such reads.
size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    if (offset + size > IMG_SIZE)
        return 0; // short read
    memcpy(buf, g_img + offset, size);
    return size;
}

// ---- Big-endian writers ----------------------------------------------------

static void w8(size_t off, uint8_t v) {
    g_img[off] = v;
}
static void w16(size_t off, uint16_t v) {
    g_img[off] = (uint8_t)(v >> 8);
    g_img[off + 1] = (uint8_t)v;
}
static void w32(size_t off, uint32_t v) {
    w16(off, (uint16_t)(v >> 16));
    w16(off + 2, (uint16_t)v);
}
static void w64(size_t off, uint64_t v) {
    w32(off, (uint32_t)(v >> 32));
    w32(off + 4, (uint32_t)v);
}
// Write `n` UTF-16BE code units; returns bytes written (n*2).
static size_t wname(size_t off, const uint16_t *units, size_t n) {
    for (size_t i = 0; i < n; i++)
        w16(off + i * 2, units[i]);
    return n * 2;
}

// Write an 80-byte HFSPlusForkData (logicalSize + 1 extent).
static void wfork(size_t off, uint64_t logical, uint32_t start, uint32_t count) {
    w64(off + 0x00, logical); // logicalSize
    w32(off + 0x08, 0); // clumpSize
    w32(off + 0x0C, count); // totalBlocks
    w32(off + 0x10, start); // extents[0].startBlock
    w32(off + 0x14, count); // extents[0].blockCount
    // extents[1..7] left zero
}

// ---- Fixture builder -------------------------------------------------------

// Build the volume into g_img.  `sig` selects "H+" (0x482B) or "HX" (0x4858).
static void build_volume(uint16_t sig) {
    memset(g_img, 0, sizeof(g_img));

    // --- Volume Header at offset 1024 ---
    size_t vh = 1024;
    w16(vh + 0x00, sig); // signature
    w16(vh + 0x02, (sig == 0x4858) ? 5 : 4); // version
    w32(vh + 0x28, BLK); // blockSize
    w32(vh + 0x2C, 4); // totalBlocks
    // catalogFile fork: 2 nodes at blocks 1..2
    wfork(vh + 0x110, 2u * BLK, 1, 2);
    // extentsFile fork: empty (logicalSize 0) — wfork not needed (zeroed)

    // --- Catalog node 0 (header node) at block 1 ---
    size_t n0 = 1u * BLK;
    w8(n0 + 8, 1); // kind = header
    w16(n0 + 10, 3); // numRecords
    // header record at +14: only nodeSize(@+18) and firstLeafNode(@+10) read.
    size_t hdr = n0 + 14;
    w16(hdr + 0, 1); // treeDepth
    w32(hdr + 2, 1); // rootNode
    w32(hdr + 6, 5); // leafRecords
    w32(hdr + 10, 1); // firstLeafNode = node 1
    w32(hdr + 14, 1); // lastLeafNode
    w16(hdr + 18, BLK); // nodeSize

    // --- Catalog node 1 (leaf node) at block 2 ---
    size_t n1 = 2u * BLK;
    w32(n1 + 0, 0); // fLink = 0 (last leaf)
    w8(n1 + 8, 0xFF); // kind = leaf (-1)
    w8(n1 + 9, 1); // height
    w16(n1 + 10, 5); // numRecords

    const uint16_t volname[] = {'T', 'e', 's', 't', 'V', 'o', 'l'}; // 7 units
    const uint16_t nm_sub[] = {'S', 'u', 'b'};
    const uint16_t nm_hello[] = {'H', 'e', 'l', 'l', 'o'};
    const uint16_t nm_bullet[] = {'B', 'u', 'l', 'l', 'e', 't', 0x2022}; // "Bullet•"

    size_t p = n1 + 14;
    size_t off[6];

    // R0: root folder thread (key parentID=2, empty name) → volume name.
    off[0] = p - n1;
    w16(p, 6); // keyLength
    w32(p + 2, 2); // parentID = root folder CNID
    w16(p + 6, 0); // nameLength = 0
    {
        size_t d = p + 8; // record data
        w16(d + 0, 3); // recordType = folderThread
        w16(d + 2, 0); // reserved
        w32(d + 4, 1); // parentID = root parent
        w16(d + 8, 7); // nodeName.length
        wname(d + 10, volname, 7);
        p = d + 24;
    }

    // R1: root folder record (key parentID=1, name="TestVol", folderID=2).
    off[1] = p - n1;
    w16(p, 20); // keyLength = 4 + 2 + 14
    w32(p + 2, 1); // parentID = root parent
    w16(p + 6, 7); // nameLength
    wname(p + 8, volname, 7);
    {
        size_t d = p + 2 + 20;
        w16(d + 0x00, 1); // recordType = folder
        w32(d + 0x04, 3); // valence = 3 (Sub, Hello, Bullet)
        w32(d + 0x08, 2); // folderID = root CNID
        p = d + 88;
    }

    // R2: folder "Sub" (parent=2, folderID=16, valence=0).
    off[2] = p - n1;
    w16(p, 12); // keyLength = 4 + 2 + 6
    w32(p + 2, 2); // parentID = root
    w16(p + 6, 3); // nameLength
    wname(p + 8, nm_sub, 3);
    {
        size_t d = p + 2 + 12;
        w16(d + 0x00, 1); // folder
        w32(d + 0x04, 0); // valence
        w32(d + 0x08, 16); // folderID
        p = d + 88;
    }

    // R3: file "Hello" (parent=2, fileID=17, data fork "Hello World").
    off[3] = p - n1;
    w16(p, 16); // keyLength = 4 + 2 + 10
    w32(p + 2, 2); // parentID
    w16(p + 6, 5); // nameLength
    wname(p + 8, nm_hello, 5);
    {
        size_t d = p + 2 + 16;
        w16(d + 0x00, 2); // recordType = file
        w32(d + 0x08, 17); // fileID
        wfork(d + 0x58, 11, 3, 1); // dataFork: 11 bytes at block 3
        // resourceFork (@0xA8) left zero
        p = d + 248;
    }

    // R4: file "Bullet•" (parent=2, fileID=18, empty) — non-ASCII name.
    off[4] = p - n1;
    w16(p, 20); // keyLength = 4 + 2 + 14
    w32(p + 2, 2); // parentID
    w16(p + 6, 7); // nameLength
    wname(p + 8, nm_bullet, 7);
    {
        size_t d = p + 2 + 20;
        w16(d + 0x00, 2); // file
        w32(d + 0x08, 18); // fileID
        p = d + 248;
    }
    off[5] = p - n1; // free-space offset

    // Record offset table at the end of node 1 (reversed).
    for (int i = 0; i < 6; i++)
        w16(n1 + BLK - (size_t)(i + 1) * 2, (uint16_t)off[i]);

    // --- "Hello" data fork at block 3 ---
    memcpy(g_img + 3u * BLK, "Hello World", 11);
}

// ---- Tests ----------------------------------------------------------------

// A non-NULL dummy image handle; disk_read_data ignores it.
static image_t *const DUMMY = (image_t *)1;

TEST(test_open_and_volume_name) {
    build_volume(0x482B); // "H+"
    hfs_volume_t *vol = hfs_open(DUMMY, 0, IMG_SIZE);
    ASSERT_TRUE(vol != NULL);
    ASSERT_EQ_INT(0, strcmp(hfs_volume_name(vol), "TestVol"));
    hfs_close(vol);
}

TEST(test_readdir_root) {
    build_volume(0x482B);
    hfs_volume_t *vol = hfs_open(DUMMY, 0, IMG_SIZE);
    ASSERT_TRUE(vol != NULL);

    hfs_dir_iter_t *it = hfs_opendir_cnid(vol, HFS_ROOT_CNID);
    ASSERT_TRUE(it != NULL);
    int saw_sub = 0, saw_hello = 0, saw_bullet = 0, count = 0;
    hfs_dirent_t de;
    int r;
    while ((r = hfs_readdir_next(it, &de)) > 0) {
        count++;
        if (strcmp(de.name, "Sub") == 0) {
            saw_sub = 1;
            ASSERT_TRUE(de.is_dir);
        } else if (strcmp(de.name, "Hello") == 0) {
            saw_hello = 1;
            ASSERT_TRUE(!de.is_dir);
            ASSERT_EQ_INT(11, (int)de.data_fork.logical_size);
        } else if (strcmp(de.name, "Bullet\xe2\x80\xa2") == 0) {
            saw_bullet = 1; // UTF-16 U+2022 decoded to UTF-8 E2 80 A2
            ASSERT_TRUE(!de.is_dir);
        }
    }
    ASSERT_EQ_INT(0, r); // clean EOF
    ASSERT_EQ_INT(3, count);
    ASSERT_TRUE(saw_sub && saw_hello && saw_bullet);
    hfs_closedir_iter(it);
    hfs_close(vol);
}

TEST(test_lookup_and_read_fork) {
    build_volume(0x482B);
    hfs_volume_t *vol = hfs_open(DUMMY, 0, IMG_SIZE);
    ASSERT_TRUE(vol != NULL);

    const char *path[] = {"Hello"};
    hfs_dirent_t de;
    ASSERT_EQ_INT(0, hfs_lookup(vol, path, 1, &de));
    ASSERT_TRUE(!de.is_dir);
    ASSERT_EQ_INT(17, (int)de.cnid);
    ASSERT_EQ_INT(11, (int)de.data_fork.logical_size);

    char buf[32];
    size_t got = 0;
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ_INT(0, hfs_read_fork(vol, &de.data_fork, 0, buf, 11, &got));
    ASSERT_EQ_INT(11, (int)got);
    ASSERT_EQ_INT(0, memcmp(buf, "Hello World", 11));

    // A reading past EOF yields zero bytes, not an error.
    got = 99;
    ASSERT_EQ_INT(0, hfs_read_fork(vol, &de.data_fork, 11, buf, 11, &got));
    ASSERT_EQ_INT(0, (int)got);

    hfs_close(vol);
}

TEST(test_lookup_nested_and_missing) {
    build_volume(0x482B);
    hfs_volume_t *vol = hfs_open(DUMMY, 0, IMG_SIZE);
    ASSERT_TRUE(vol != NULL);

    // "Sub" resolves as an (empty) directory.
    const char *sub[] = {"Sub"};
    hfs_dirent_t de;
    ASSERT_EQ_INT(0, hfs_lookup(vol, sub, 1, &de));
    ASSERT_TRUE(de.is_dir);

    // Descending through a file is ENOTDIR; an absent name is ENOENT.
    const char *under_file[] = {"Hello", "x"};
    ASSERT_TRUE(hfs_lookup(vol, under_file, 2, &de) < 0);
    const char *missing[] = {"Nope"};
    ASSERT_TRUE(hfs_lookup(vol, missing, 1, &de) < 0);

    hfs_close(vol);
}

TEST(test_hfsx_signature_accepted) {
    build_volume(0x4858); // "HX"
    hfs_volume_t *vol = hfs_open(DUMMY, 0, IMG_SIZE);
    ASSERT_TRUE(vol != NULL);
    ASSERT_EQ_INT(0, strcmp(hfs_volume_name(vol), "TestVol"));
    hfs_close(vol);
}

TEST(test_bad_signature_rejected) {
    build_volume(0x482B);
    g_img[1024] = 'X'; // corrupt the signature word
    g_img[1025] = 'Y';
    ASSERT_TRUE(hfs_open(DUMMY, 0, IMG_SIZE) == NULL);
}

int main(void) {
    RUN(test_open_and_volume_name);
    RUN(test_readdir_root);
    RUN(test_lookup_and_read_fork);
    RUN(test_lookup_nested_and_missing);
    RUN(test_hfsx_signature_accepted);
    RUN(test_bad_signature_rejected);
    fprintf(stderr, "All hfsplus tests passed.\n");
    return 0;
}
