// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Wire-level unit tests for the AFP server (src/core/network/appletalk_server.c)
// and the three persistence modules it sits on: the CNID catalog
// (afp_catalog.c), the desktop database (afp_desktop.c) and the AppleDouble
// metadata codec (afp_meta.c), plus the shared fork/deny/lock store
// (afp_fork.c).
//
// Every case drives afp_handle_command() against a share rooted in a temp
// directory — no guest, no transport, milliseconds per case — so the
// combinatorial surface the guest-level suites cannot reach (bitmap
// permutations, deny matrices, lock overlap tables, log replay after a
// simulated crash) lives here.  Request and reply layouts are transcribed
// from docs/core/network/appletalk_server.md §2 and, for the AFP 2.1 calls,
// from Apple's AppleTalk Filing Protocol v2.1/2.2 specification.

#include "afp_catalog.h"
#include "afp_desktop.h"
#include "afp_fork.h"
#include "afp_meta.h"
#include "appletalk.h"
#include "test_assert.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void stub_set_afp_version(const char *v);
int stub_attention_count(void);

// AFP opcodes and result codes used by the tests (the server's private
// headers are not exported; these mirror appletalk_server.md §1.5/§1.6).
#define OP_BYTE_RANGE_LOCK 0x01
#define OP_CLOSE_VOL       0x02
#define OP_CLOSE_FORK      0x04
#define OP_COPY_FILE       0x05
#define OP_CREATE_DIR      0x06
#define OP_CREATE_FILE     0x07
#define OP_DELETE          0x08
#define OP_ENUMERATE       0x09
#define OP_FLUSH_FORK      0x0B
#define OP_GET_SRVR_PARMS  0x10
#define OP_GET_VOL_PARMS   0x11
#define OP_LOGIN           0x12
#define OP_MOVE_AND_RENAME 0x17
#define OP_OPEN_VOL        0x18
#define OP_OPEN_FORK       0x1A
#define OP_READ            0x1B
#define OP_RENAME          0x1C
#define OP_SET_FORK_PARMS  0x1F
#define OP_SET_VOL_PARMS   0x20
#define OP_WRITE           0x21
#define OP_GET_FD_PARMS    0x22
#define OP_SET_FD_PARMS    0x23
#define OP_GET_SRVR_MSG    0x26
#define OP_CREATE_ID       0x27
#define OP_DELETE_ID       0x28
#define OP_RESOLVE_ID      0x29
#define OP_EXCHANGE_FILES  0x2A
#define OP_CAT_SEARCH      0x2B
#define OP_OPEN_DT         0x30
#define OP_GET_ICON        0x33
#define OP_GET_ICON_INFO   0x34
#define OP_ADD_APPL        0x35
#define OP_GET_APPL        0x37
#define OP_ADD_COMMENT     0x38
#define OP_RMV_COMMENT     0x39
#define OP_GET_COMMENT     0x3A
#define OP_ADD_ICON        0xC0

#define ERR_OK              0x00000000u
#define ERR_ACCESS_DENIED   0xFFFFEC78u
#define ERR_BITMAP          0xFFFFEC74u
#define ERR_DENY_CONFLICT   0xFFFFEC72u
#define ERR_DIR_NOT_EMPTY   0xFFFFEC71u
#define ERR_EOF             0xFFFFEC6Fu
#define ERR_FILE_BUSY       0xFFFFEC6Eu
#define ERR_ITEM_NOT_FOUND  0xFFFFEC6Cu
#define ERR_LOCK            0xFFFFEC6Bu
#define ERR_NOT_SUPPORTED   0xFFFFEC60u
#define ERR_OBJECT_EXISTS   0xFFFFEC67u
#define ERR_OBJECT_NOT_FND  0xFFFFEC66u
#define ERR_PARAM           0xFFFFEC65u
#define ERR_RANGE_OVERLAP   0xFFFFEC63u
#define ERR_RANGE_NOT_LOCK  0xFFFFEC64u
#define ERR_OBJECT_TYPE     0xFFFFEC5Fu
#define ERR_OBJECT_LOCKED   0xFFFFEC58u
#define ERR_ID_NOT_FOUND    0xFFFFEC56u
#define ERR_ID_EXISTS       0xFFFFEC55u
#define ERR_CATALOG_CHANGED 0xFFFFEC53u
#define ERR_SAME_OBJECT     0xFFFFEC52u
#define ERR_BAD_ID          0xFFFFEC51u

#define CNID_ROOT 2u

#define SESSION 0x0021

// --- request/reply plumbing -------------------------------------------------

static uint8_t g_req[4096];
static int g_req_len;
static uint8_t g_reply[8192];
static int g_reply_len;

static void req_reset(void) {
    g_req_len = 0;
    memset(g_req, 0, sizeof(g_req));
}
static void put8(uint8_t v) {
    g_req[g_req_len++] = v;
}
static void put16(uint16_t v) {
    g_req[g_req_len++] = (uint8_t)(v >> 8);
    g_req[g_req_len++] = (uint8_t)v;
}
static void put32(uint32_t v) {
    put16((uint16_t)(v >> 16));
    put16((uint16_t)v);
}
static void put_pstr(const char *s) {
    size_t n = s ? strlen(s) : 0;
    put8((uint8_t)n);
    for (size_t i = 0; i < n; i++)
        put8((uint8_t)s[i]);
}
// A pathname argument is a PathType byte followed by the Pascal string.
static void put_path(const char *s) {
    put8(2); // 2 = long names
    put_pstr(s);
}
static void put_bytes(const void *p, size_t n) {
    memcpy(g_req + g_req_len, p, n);
    g_req_len += (int)n;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Dispatch the assembled request; the reply lands in g_reply/g_reply_len.
static uint32_t call(uint8_t opcode) {
    g_reply_len = 0;
    memset(g_reply, 0, sizeof(g_reply));
    return afp_handle_command(SESSION, opcode, g_req, g_req_len, g_reply, (int)sizeof(g_reply), &g_reply_len);
}

// --- share fixture ----------------------------------------------------------

static char g_root[256];
static uint16_t g_vol_id;

// Recursively remove a directory tree (the fixture's teardown).
static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            rm_rf(child);
        }
        closedir(d);
        rmdir(path);
        return;
    }
    unlink(path);
}

static void write_file(const char *rel, const char *contents) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    if (contents && *contents)
        fwrite(contents, 1, strlen(contents), f);
    fclose(f);
}

static void host_path(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_root, rel);
}

// Open a fresh share with a unique root, and log in so the 2.1 calls are
// allowed.  Every test starts from this state.
static void fixture_up(const char *tag) {
    snprintf(g_root, sizeof(g_root), "/tmp/gs-afp-test-%d-%s", (int)getpid(), tag);
    rm_rf(g_root);
    ASSERT_EQ_INT(0, mkdir(g_root, 0755));
    char err[192];
    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) >= 0);
    int slot = atalk_afp_volume_find("TestVol");
    ASSERT_TRUE(slot >= 0);
    g_vol_id = (uint16_t)atalk_afp_volume_vol_id(slot);
    stub_set_afp_version("AFPVersion 2.1");
}

static void fixture_down(void) {
    char err[192];
    atalk_afp_volume_remove("TestVol", err, sizeof(err));
    rm_rf(g_root);
}

// --- small request builders -------------------------------------------------

// Pad(1) VolumeID(2) DirectoryID(4) PathType(1) Pathname
static void req_vol_dir_path(uint16_t vol, uint32_t dir, const char *path) {
    req_reset();
    put8(0);
    put16(vol);
    put32(dir);
    put_path(path);
}

static uint32_t create_file(const char *name) {
    req_reset();
    put8(0); // soft create
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path(name);
    return call(OP_CREATE_FILE);
}

static uint32_t create_dir(const char *name, uint32_t *out_cnid) {
    req_vol_dir_path(g_vol_id, CNID_ROOT, name);
    uint32_t rc = call(OP_CREATE_DIR);
    if (rc == ERR_OK && out_cnid)
        *out_cnid = rd32(g_reply);
    return rc;
}

// FPGetFileDirParms with one file bitmap; returns the reply's parameter area
// start (6) so callers can read fields out of it.
static uint32_t get_fd_parms(const char *path, uint16_t file_bm, uint16_t dir_bm) {
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(file_bm);
    put16(dir_bm);
    put_path(path);
    return call(OP_GET_FD_PARMS);
}

// FPOpenFork; returns the result and hands back the fork refnum.
static uint32_t open_fork(const char *path, bool resource, uint16_t access, uint16_t *out_ref) {
    req_reset();
    put8(resource ? 0x80 : 0x00);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0); // no parameters wanted
    put16(access);
    put_path(path);
    uint32_t rc = call(OP_OPEN_FORK);
    if (out_ref)
        *out_ref = (g_reply_len >= 4) ? rd16(g_reply + 2) : 0;
    return rc;
}

static uint32_t close_fork(uint16_t ref) {
    req_reset();
    put8(0);
    put16(ref);
    return call(OP_CLOSE_FORK);
}

// ============================================================================
// WP-1 — honest volume parameters
// ============================================================================

TEST(vol_parms_report_real_sizes_and_dates) {
    fixture_up("volparms");

    // FPGetVolParms: Pad(1) VolumeID(2) Bitmap(2).  Ask for attributes,
    // signature, the three dates, the ID, both size fields and the name.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put16(0x01FF);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_VOL_PARMS));

    ASSERT_EQ_INT(0x01FF, rd16(g_reply));
    const uint8_t *p = g_reply + 2;
    uint16_t attrs = rd16(p);
    p += 2;
    // Both AFP 2.1 capability bits are set because both code paths exist.
    ASSERT_TRUE(attrs & (1u << 2)); // SupportsFileIDs
    ASSERT_TRUE(attrs & (1u << 3)); // SupportsCatSearch
    ASSERT_EQ_INT(0x0002, (int)rd16(p)); // signature: fixed directory ID
    p += 2;
    uint32_t cdate = rd32(p);
    p += 4;
    uint32_t mdate = rd32(p);
    p += 4;
    uint32_t bdate = rd32(p);
    p += 4;
    ASSERT_EQ_INT((int)g_vol_id, (int)rd16(p));
    p += 2;
    uint32_t free_bytes = rd32(p);
    p += 4;
    uint32_t total_bytes = rd32(p);
    p += 4;

    // Dates are AFP timestamps taken from the share directory, not raw Unix
    // seconds and not time(NULL): anything after 1 Jan 2000 is > 96 years of
    // seconds from the 1904 epoch.
    ASSERT_TRUE(cdate > 3029529600u);
    ASSERT_TRUE(mdate > 3029529600u);
    ASSERT_EQ_INT((int)0x80000000u, (int)bdate); // never backed up

    // Sizes come from the host filesystem and are clamped to the 2 GB ceiling
    // a 32-bit classic client can hold — never the old fixed 12/16 MB.
    ASSERT_TRUE(total_bytes > 16u * 1024u * 1024u);
    ASSERT_TRUE(total_bytes <= 0x7FFFFC00u);
    ASSERT_TRUE(free_bytes <= total_bytes);

    // The volume name is packed after the fixed fields, addressed by offset.
    uint16_t name_off = rd16(g_reply + 2 + 26);
    const uint8_t *nm = g_reply + 2 + name_off;
    ASSERT_EQ_INT(7, (int)nm[0]);
    ASSERT_EQ_INT(0, memcmp(nm + 1, "TestVol", 7));

    fixture_down();
}

TEST(set_vol_parms_persists_the_backup_date) {
    fixture_up("setvol");

    // FPSetVolParms: Pad(1) VolumeID(2) Bitmap(2) BackupDate(4).
    req_reset();
    put8(0);
    put16(g_vol_id);
    put16(0x0010);
    put32(0xC0000000u);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_VOL_PARMS));

    req_reset();
    put8(0);
    put16(g_vol_id);
    put16(0x0010);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_VOL_PARMS));
    ASSERT_EQ_INT((int)0xC0000000u, (int)rd32(g_reply + 2));

    // Anything other than the backup date is a bitmap error, not a silent
    // no-op the way the old stub behaved.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put16(0x0004);
    put32(0);
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_SET_VOL_PARMS));

    fixture_down();
}

// ============================================================================
// WP-2 — full FPSet*Parms semantics
// ============================================================================

// The historical bug: Finder Info was located by a hand-rolled switch that
// only knew bits 0..4, so any higher bit preceding it mis-offset the field.
// Drive the whole permutation of preceding bits.
TEST(set_parms_finds_finder_info_behind_every_preceding_bit) {
    fixture_up("setparms");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));

    // Every settable bit that can precede Finder Info: Attributes (0),
    // Creation (2), Modification (3) and Backup (4) dates.  Bit 1 (Parent
    // Directory ID) is read-only and is covered by the bitmap-error test.
    static const uint16_t preceding[] = {0x0000, 0x0001, 0x0004, 0x0008, 0x0010, 0x0005, 0x0011, 0x001C, 0x001D};
    for (size_t i = 0; i < sizeof(preceding) / sizeof(preceding[0]); i++) {
        uint16_t bitmap = (uint16_t)(preceding[i] | 0x0020); // + Finder Info
        uint8_t finder[32];
        for (int b = 0; b < 32; b++)
            finder[b] = (uint8_t)(0x10 + i * 8 + b);

        req_reset();
        put8(0);
        put16(g_vol_id);
        put32(CNID_ROOT);
        put16(bitmap);
        put_path("Doc");
        if (bitmap & 0x0001)
            put16(0); // Attributes
        if (bitmap & 0x0004)
            put32(0xB0000000u); // Creation date
        if (bitmap & 0x0008)
            put32(0xB1000000u); // Modification date
        if (bitmap & 0x0010)
            put32(0xB2000000u); // Backup date
        put_bytes(finder, sizeof(finder));
        ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));

        // Read it straight back through the Finder Info bit alone.
        ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Doc", 0x0020, 0));
        ASSERT_EQ_INT(0, memcmp(g_reply + 6, finder, 32));
    }
    fixture_down();
}

TEST(set_parms_round_trips_dates_and_write_through_mtime) {
    fixture_up("dates");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));

    // Bits 2/3/4 = creation / modification / backup date.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x001C);
    put_path("Doc");
    put32(0xB0000000u);
    put32(0xB1000000u);
    put32(0xB2000000u);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));

    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Doc", 0x001C, 0));
    ASSERT_EQ_INT((int)0xB0000000u, (int)rd32(g_reply + 6));
    ASSERT_EQ_INT((int)0xB1000000u, (int)rd32(g_reply + 10));
    ASSERT_EQ_INT((int)0xB2000000u, (int)rd32(g_reply + 14));

    // The modification date is written through to the host so the two agree.
    char path[512];
    host_path("Doc", path, sizeof(path));
    struct stat st;
    ASSERT_EQ_INT(0, stat(path, &st));
    ASSERT_EQ_INT((int)afp_meta_time_to_unix(0xB1000000u), (int)st.st_mtime);

    fixture_down();
}

TEST(locked_attribute_blocks_delete_and_rename) {
    fixture_up("locked");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));

    // Attributes bit 0, with the Set/Clear bit (15) set: raise DeleteInhibit
    // and RenameInhibit.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0001);
    put_path("Doc");
    put16(0x8000u | (1u << 7) | (1u << 8));
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));

    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Doc", 0x0001, 0));
    uint16_t attrs = rd16(g_reply + 6);
    ASSERT_TRUE(attrs & (1u << 7));
    ASSERT_TRUE(attrs & (1u << 8));

    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_OBJECT_LOCKED, (int)call(OP_DELETE));

    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    put_path("Other");
    ASSERT_EQ_INT((int)ERR_OBJECT_LOCKED, (int)call(OP_RENAME));

    // Clearing the same bits (Set/Clear = 0) unlocks the file again.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0001);
    put_path("Doc");
    put16((1u << 7) | (1u << 8));
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_DELETE));

    fixture_down();
}

TEST(write_inhibit_blocks_opening_a_fork_for_writing) {
    fixture_up("winhibit");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0001);
    put_path("Doc");
    put16(0x8000u | (1u << 5)); // WriteInhibit
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));

    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_ACCESS_DENIED, (int)open_fork("Doc", false, 0x0003, &ref));
    // Reading is still allowed.
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, 0x0001, &ref));
    close_fork(ref);
    fixture_down();
}

TEST(set_parms_rejects_read_only_bits) {
    fixture_up("setparmsbm");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0100); // File Number — not settable
    put_path("Doc");
    put32(0);
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_SET_FD_PARMS));
    fixture_down();
}

// ============================================================================
// WP-3 — persistent CNID catalog
// ============================================================================

// FileNumber for a path, straight out of the catalog (bit 8).
static uint32_t file_number(const char *path) {
    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms(path, 0x0100, 0x0100));
    return rd32(g_reply + 6);
}

TEST(cnid_survives_rename_move_and_is_never_reused) {
    fixture_up("cnid");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));
    uint32_t id = file_number("Doc");
    ASSERT_TRUE(id >= 17u); // HFS reserves everything below 16

    // Rename keeps the ID.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    put_path("Renamed");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RENAME));
    ASSERT_EQ_INT((int)id, (int)file_number("Renamed"));

    // So does a move into a new directory.
    uint32_t dir_id = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)create_dir("Folder", &dir_id));
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT); // source directory
    put32(dir_id); // destination directory
    put_path("Renamed");
    put_path(""); // destination path is the directory itself
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_MOVE_AND_RENAME));
    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Folder/Renamed", 0x0100, 0x0100));
    ASSERT_EQ_INT((int)id, (int)rd32(g_reply + 6));

    // A deleted CNID is never handed out again.
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Folder/Renamed");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_DELETE));
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Fresh"));
    ASSERT_TRUE(file_number("Fresh") != id);

    fixture_down();
}

TEST(cnids_persist_across_a_share_remove_and_readd) {
    fixture_up("cnidpersist");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Alpha"));
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Beta"));
    uint32_t alpha = file_number("Alpha");
    uint32_t beta = file_number("Beta");
    ASSERT_TRUE(alpha != beta);

    // Removing and re-adding the share drops every scrap of in-memory server
    // state — the restart proxy available inside one process.
    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("TestVol", err, sizeof(err)));
    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) >= 0);
    g_vol_id = (uint16_t)atalk_afp_volume_vol_id(atalk_afp_volume_find("TestVol"));

    ASSERT_EQ_INT((int)alpha, (int)file_number("Alpha"));
    ASSERT_EQ_INT((int)beta, (int)file_number("Beta"));
    fixture_down();
}

TEST(catalog_survives_a_torn_tail_record) {
    fixture_up("torn");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Alpha"));
    uint32_t alpha = file_number("Alpha");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Beta"));

    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("TestVol", err, sizeof(err)));

    // Simulate a crash mid-append: lop three bytes off the log's last record.
    char log[512];
    snprintf(log, sizeof(log), "%s/%s/catalog.gsc", g_root, AFP_CONTROL_DIR);
    struct stat st;
    ASSERT_EQ_INT(0, stat(log, &st));
    ASSERT_EQ_INT(0, truncate(log, st.st_size - 3));

    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) >= 0);
    g_vol_id = (uint16_t)atalk_afp_volume_vol_id(atalk_afp_volume_find("TestVol"));

    // Everything before the torn record replayed; the truncated one did not,
    // so Beta is re-adopted with a fresh ID rather than the log being lost.
    ASSERT_EQ_INT((int)alpha, (int)file_number("Alpha"));
    ASSERT_TRUE(file_number("Beta") != 0);
    fixture_down();
}

TEST(out_of_band_files_are_adopted_on_first_touch) {
    fixture_up("adopt");
    // A file that appeared behind the server's back (the shell's `cp`, the
    // host, a page reload) gets a catalog entry the moment AFP touches it.
    write_file("Outside", "hello");
    int slot = atalk_afp_volume_find("TestVol");
    ASSERT_EQ_INT(1, (int)atalk_afp_volume_cnid_count(slot)); // just the root
    uint32_t id = file_number("Outside");
    ASSERT_TRUE(id >= 17u);
    ASSERT_EQ_INT(2, (int)atalk_afp_volume_cnid_count(slot));
    // A second touch reuses the entry rather than minting a new ID.
    ASSERT_EQ_INT((int)id, (int)file_number("Outside"));
    ASSERT_EQ_INT(2, (int)atalk_afp_volume_cnid_count(slot));
    fixture_down();
}

TEST(catalog_module_reconstructs_paths_and_sweeps_stale_entries) {
    fixture_up("catmodule");
    afp_catalog_t *cat = afp_catalog_open(g_root);
    ASSERT_TRUE(cat != NULL);

    const afp_cat_entry_t *dir = afp_catalog_add(cat, AFP_CNID_ROOT, "Folder", true);
    ASSERT_TRUE(dir != NULL);
    uint32_t dir_id = dir->cnid;
    const afp_cat_entry_t *file = afp_catalog_add(cat, dir_id, "Leaf", false);
    ASSERT_TRUE(file != NULL);
    uint32_t leaf_id = file->cnid;

    char path[AFP_CAT_MAX_PATH];
    ASSERT_TRUE(afp_catalog_path(cat, leaf_id, path, sizeof(path)));
    ASSERT_EQ_INT(0, strcmp(path, "Folder/Leaf"));

    // Renaming the parent re-derives every descendant path, IDs untouched.
    ASSERT_TRUE(afp_catalog_rename(cat, dir_id, "Moved"));
    ASSERT_TRUE(afp_catalog_path(cat, leaf_id, path, sizeof(path)));
    ASSERT_EQ_INT(0, strcmp(path, "Moved/Leaf"));

    // Deleting the directory tombstones the subtree.
    ASSERT_TRUE(afp_catalog_remove(cat, dir_id));
    ASSERT_TRUE(afp_catalog_find(cat, leaf_id) == NULL);

    // A sweep drops entries whose host path is gone and bumps the generation.
    const afp_cat_entry_t *ghost = afp_catalog_add(cat, AFP_CNID_ROOT, "Ghost", false);
    ASSERT_TRUE(ghost != NULL);
    uint32_t gen_before = afp_catalog_generation(cat);
    ASSERT_EQ_INT(1, (int)afp_catalog_sweep(cat));
    ASSERT_TRUE(afp_catalog_generation(cat) > gen_before);

    afp_catalog_close(cat);
    fixture_down();
}

// ============================================================================
// WP-4 — FPEnumerate: snapshot paging, no silent cap
// ============================================================================

// One FPEnumerate page; returns the result and the entry count served.
static uint32_t enumerate(uint16_t start, uint16_t req_count, uint16_t max_reply, uint16_t *out_actual) {
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0140); // file bitmap: long name + file number
    put16(0x0140); // dir bitmap: long name + directory ID
    put16(req_count);
    put16(start);
    put16(max_reply);
    put_path("");
    uint32_t rc = call(OP_ENUMERATE);
    if (out_actual)
        *out_actual = (g_reply_len >= 6) ? rd16(g_reply + 4) : 0;
    return rc;
}

TEST(enumerate_lists_every_entry_of_a_large_directory) {
    fixture_up("enumbig");
    // 600 files: past the old fixed AFP_ENUM_MAX_ENTRIES cap of 512, which
    // silently truncated the listing.
    for (int i = 0; i < 600; i++) {
        char name[32];
        snprintf(name, sizeof(name), "F%03d", i);
        write_file(name, "x");
    }
    uint16_t seen = 0;
    uint16_t start = 1;
    for (int page = 0; page < 100 && seen < 600; page++) {
        uint16_t actual = 0;
        uint32_t rc = enumerate(start, 50, 4096, &actual);
        if (rc != ERR_OK)
            break;
        ASSERT_TRUE(actual > 0);
        seen = (uint16_t)(seen + actual);
        start = (uint16_t)(start + actual);
    }
    ASSERT_EQ_INT(600, (int)seen);
    fixture_down();
}

TEST(enumerate_pages_are_served_from_a_snapshot) {
    fixture_up("enumsnap");
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "F%02d", i);
        write_file(name, "x");
    }
    // First page takes the snapshot.
    uint16_t actual = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)enumerate(1, 5, 4096, &actual));
    ASSERT_EQ_INT(5, (int)actual);

    // A file appearing out of band between pages must not shift the indices
    // of the walk already in progress.
    write_file("AAA-inserted", "x");

    uint16_t total = 5;
    uint16_t start = 6;
    for (int page = 0; page < 20 && total < 20; page++) {
        uint16_t got = 0;
        if (enumerate(start, 5, 4096, &got) != ERR_OK)
            break;
        total = (uint16_t)(total + got);
        start = (uint16_t)(start + got);
    }
    ASSERT_EQ_INT(20, (int)total); // exactly the 20 captured, no duplicate, no skip

    // Restarting the walk picks up the new file.
    uint16_t restart = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)enumerate(1, 100, 8000, &restart));
    ASSERT_EQ_INT(21, (int)restart);
    fixture_down();
}

TEST(enumerate_hides_sidecars_and_the_control_directory) {
    fixture_up("enumhide");
    write_file("Doc", "data");
    write_file("._Doc", "sidecar");
    uint16_t actual = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)enumerate(1, 100, 4096, &actual));
    ASSERT_EQ_INT(1, (int)actual); // .gs-afp and ._Doc are both invisible
    fixture_down();
}

TEST(enumerate_rejects_an_empty_bitmap) {
    fixture_up("enumbm");
    write_file("Doc", "x");
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0);
    put16(0);
    put16(10);
    put16(1);
    put16(4096);
    put_path("");
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_ENUMERATE));
    fixture_down();
}

// ============================================================================
// WP-5 — the AFP 2.1 command set
// ============================================================================

TEST(file_id_lifecycle) {
    fixture_up("fileids");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));

    // FPCreateID: Pad(1) VolumeID(2) DirectoryID(4) PathType(1) Pathname.
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_CREATE_ID));
    uint32_t id = rd32(g_reply);
    ASSERT_EQ_INT((int)file_number("Doc"), (int)id); // the ID is the FileNumber

    // A second FPCreateID reports afpIDExists and still returns the ID.
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_ID_EXISTS, (int)call(OP_CREATE_ID));
    ASSERT_EQ_INT((int)id, (int)rd32(g_reply));

    // FPResolveID: Pad(1) VolumeID(2) FileID(4) Bitmap(2) — ask for the name.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(id);
    put16(0x0040); // long name
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RESOLVE_ID));
    uint16_t name_off = rd16(g_reply + 2);
    const uint8_t *nm = g_reply + 2 + name_off;
    ASSERT_EQ_INT(3, (int)nm[0]);
    ASSERT_EQ_INT(0, memcmp(nm + 1, "Doc", 3));

    // Renaming the file does not disturb the ID — that is the whole point.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RENAME));
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(id);
    put16(0x0040);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RESOLVE_ID));
    nm = g_reply + 2 + rd16(g_reply + 2);
    ASSERT_EQ_INT(4, (int)nm[0]);
    ASSERT_EQ_INT(0, memcmp(nm + 1, "Doc2", 4));

    // FPDeleteID invalidates the thread; resolving then fails.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(id);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_DELETE_ID));
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(id);
    put16(0x0040);
    ASSERT_EQ_INT((int)ERR_BAD_ID, (int)call(OP_RESOLVE_ID));
    // And a second delete reports afpIDNotFound.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(id);
    ASSERT_EQ_INT((int)ERR_ID_NOT_FOUND, (int)call(OP_DELETE_ID));

    fixture_down();
}

TEST(create_id_refuses_directories_and_missing_files) {
    fixture_up("fileiderr");
    uint32_t dir_id = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)create_dir("Folder", &dir_id));
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Folder");
    ASSERT_EQ_INT((int)ERR_OBJECT_TYPE, (int)call(OP_CREATE_ID));
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Nope");
    ASSERT_EQ_INT((int)ERR_OBJECT_NOT_FND, (int)call(OP_CREATE_ID));
    fixture_down();
}

// AFP_21_22 Figure 1-17: exchanging Blue and Red swaps the filename, parent
// directory ID, file ID and creation date — the bytes stay where the fork
// reference expects them.
TEST(exchange_files_moves_contents_and_keeps_ids_with_names) {
    fixture_up("exchange");
    write_file("Blue", "blue-contents");
    write_file("Red", "red-contents");
    uint32_t blue_id = file_number("Blue");
    uint32_t red_id = file_number("Red");

    // Give each file a distinct creation date so the swap is observable.
    for (int i = 0; i < 2; i++) {
        req_reset();
        put8(0);
        put16(g_vol_id);
        put32(CNID_ROOT);
        put16(0x0004);
        put_path(i == 0 ? "Blue" : "Red");
        put32(i == 0 ? 0xB0000000u : 0xB5000000u);
        ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FD_PARMS));
    }

    // FPExchangeFiles: Pad(1) VolumeID(2) SrcDirID(4) DestDirID(4)
    // SrcPathType(1) SrcPath DestPathType(1) DestPath.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put32(CNID_ROOT);
    put_path("Blue");
    put_path("Red");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_EXCHANGE_FILES));

    // The contents changed places...
    char path[512];
    char buf[64];
    host_path("Blue", path, sizeof(path));
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT_EQ_INT(0, strcmp(buf, "red-contents"));

    // ...while the file IDs and creation dates stayed with the names.
    ASSERT_EQ_INT((int)blue_id, (int)file_number("Blue"));
    ASSERT_EQ_INT((int)red_id, (int)file_number("Red"));
    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Blue", 0x0004, 0));
    ASSERT_EQ_INT((int)0xB0000000u, (int)rd32(g_reply + 6));
    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Red", 0x0004, 0));
    ASSERT_EQ_INT((int)0xB5000000u, (int)rd32(g_reply + 6));

    // Exchanging a file with itself is afpSameObjectErr.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put32(CNID_ROOT);
    put_path("Blue");
    put_path("Blue");
    ASSERT_EQ_INT((int)ERR_SAME_OBJECT, (int)call(OP_EXCHANGE_FILES));

    fixture_down();
}

TEST(exchange_files_keeps_an_open_fork_pointed_at_its_bytes) {
    fixture_up("exchangefork");
    write_file("Temp", "NEW");
    write_file("Doc", "OLD");

    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Temp", false, 0x0001, &ref));

    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put32(CNID_ROOT);
    put_path("Temp");
    put_path("Doc");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_EXCHANGE_FILES));

    // "Byte-range locks and deny modes still apply to the same file reference
    // number and data" — so the refnum still reads the bytes it opened, which
    // now live under the other name.
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(3);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));
    ASSERT_EQ_INT(3, g_reply_len);
    ASSERT_EQ_INT(0, memcmp(g_reply, "NEW", 3));
    close_fork(ref);
    fixture_down();
}

// FPCatSearch: Pad(1) VolumeID(2) ReqMatches(4) Reserved(4) CatPosition(16)
// FileRsltBitmap(2) DirRsltBitmap(2) RequestBitmap(4) Spec1 Spec2.
// Each specification is Size(1) + filler(1) + parameters in bitmap order.
static void put_catsearch_header(uint32_t req_matches, const uint8_t catpos[16], uint16_t file_bm, uint16_t dir_bm,
                                 uint32_t request_bm) {
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(req_matches);
    put32(0);
    if (catpos)
        put_bytes(catpos, 16);
    else
        for (int i = 0; i < 16; i++)
            put8(0);
    put16(file_bm);
    put16(dir_bm);
    put32(request_bm);
}

TEST(cat_search_matches_on_name_and_resumes_from_cat_position) {
    fixture_up("catsearch");
    write_file("Report", "x");
    write_file("Report2", "x");
    write_file("Other", "x");
    // Adopt everything so the catalog has entries to walk.
    (void)file_number("Report");
    (void)file_number("Report2");
    (void)file_number("Other");

    // Search files by full long name (request bitmap bit 6, no partial flag).
    put_catsearch_header(10, NULL, 0x0040, 0x0000, 0x00000040u);
    // Specification1: size, filler, then the name-offset field, then the
    // Pascal string it points at.  The offset is measured from the first
    // parameter byte.
    int spec1 = g_req_len;
    put8(0); // size, patched below
    put8(0); // filler
    put16(2); // name offset: just past this 2-byte field
    put_pstr("Report");
    g_req[spec1] = (uint8_t)(g_req_len - spec1);
    // Specification2 carries a nil name field.
    int spec2 = g_req_len;
    put8(0);
    put8(0);
    put16(0);
    g_req[spec2] = (uint8_t)(g_req_len - spec2);

    uint32_t rc = call(OP_CAT_SEARCH);
    ASSERT_TRUE(rc == ERR_OK || rc == ERR_EOF); // afpEofError = walked the tree
    ASSERT_EQ_INT(1, (int)rd32(g_reply + 20)); // exactly "Report", not "Report2"

    // A stale CatPosition (wrong generation) is rejected so the client
    // restarts rather than silently resuming into a changed catalog.
    uint8_t catpos[16];
    memcpy(catpos, g_reply, 16);
    catpos[7] ^= 0xFF; // corrupt the recorded generation
    put_catsearch_header(10, catpos, 0x0040, 0x0000, 0x00000040u);
    spec1 = g_req_len;
    put8(0);
    put8(0);
    put16(2);
    put_pstr("Report");
    g_req[spec1] = (uint8_t)(g_req_len - spec1);
    spec2 = g_req_len;
    put8(0);
    put8(0);
    put16(0);
    g_req[spec2] = (uint8_t)(g_req_len - spec2);
    ASSERT_EQ_INT((int)ERR_CATALOG_CHANGED, (int)call(OP_CAT_SEARCH));

    fixture_down();
}

TEST(cat_search_partial_name_matches_several) {
    fixture_up("catsearchpart");
    write_file("Report", "x");
    write_file("Report2", "x");
    write_file("Other", "x");
    (void)file_number("Report");
    (void)file_number("Report2");
    (void)file_number("Other");

    // Bit 31 of RequestBitmap selects partial-name matching.
    put_catsearch_header(10, NULL, 0x0040, 0x0000, 0x80000040u);
    int spec1 = g_req_len;
    put8(0);
    put8(0);
    put16(2);
    put_pstr("Repo");
    g_req[spec1] = (uint8_t)(g_req_len - spec1);
    int spec2 = g_req_len;
    put8(0);
    put8(0);
    put16(0);
    g_req[spec2] = (uint8_t)(g_req_len - spec2);

    uint32_t rc = call(OP_CAT_SEARCH);
    ASSERT_TRUE(rc == ERR_OK || rc == ERR_EOF);
    ASSERT_EQ_INT(2, (int)rd32(g_reply + 20));
    fixture_down();
}

TEST(cat_search_rejects_criteria_it_cannot_serve) {
    fixture_up("catsearchbm");
    // Bit 11 (Group ID) is not a searchable field here.
    put_catsearch_header(10, NULL, 0x0040, 0x0000, 0x00000800u);
    put8(2);
    put8(0);
    put8(2);
    put8(0);
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_CAT_SEARCH));
    fixture_down();
}

TEST(server_message_round_trips_and_raises_an_attention) {
    fixture_up("srvrmsg");
    int before = stub_attention_count();
    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_set_message("Back at 5pm", err, sizeof(err)));
    ASSERT_TRUE(stub_attention_count() > before); // "server message available"

    // FPGetSrvrMsg: Pad(1) MsgType(2) MsgBitmap(2).
    req_reset();
    put8(0);
    put16(1); // 1 = server message
    put16(1); // bitmap: the message string
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_SRVR_MSG));
    ASSERT_EQ_INT(1, (int)rd16(g_reply));
    ASSERT_EQ_INT(1, (int)rd16(g_reply + 2));
    ASSERT_EQ_INT(11, (int)g_reply[4]);
    ASSERT_EQ_INT(0, memcmp(g_reply + 5, "Back at 5pm", 11));

    // Unrecognised bitmap bits are an error, not silently ignored.
    req_reset();
    put8(0);
    put16(1);
    put16(0x0002);
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_GET_SRVR_MSG));

    ASSERT_EQ_INT(0, atalk_afp_set_message("", err, sizeof(err)));
    fixture_down();
}

TEST(afp_21_commands_are_refused_on_a_20_session) {
    fixture_up("gate21");
    stub_set_afp_version("AFPVersion 2.0");
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_NOT_SUPPORTED, (int)call(OP_CREATE_ID));
    req_reset();
    put8(0);
    put16(1);
    put16(1);
    ASSERT_EQ_INT((int)ERR_NOT_SUPPORTED, (int)call(OP_GET_SRVR_MSG));
    stub_set_afp_version("AFPVersion 2.1");
    fixture_down();
}

TEST(login_negotiates_a_known_version_and_uam) {
    fixture_up("login");
    req_reset();
    put_pstr("AFPVersion 2.1");
    put_pstr("No User Authent");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_LOGIN));

    req_reset();
    put_pstr("AFPVersion 3.1");
    put_pstr("No User Authent");
    ASSERT_EQ_INT((int)0xFFFFEC75u, (int)call(OP_LOGIN)); // BadVersNum

    req_reset();
    put_pstr("AFPVersion 2.1");
    put_pstr("Cleartxt Passwrd");
    ASSERT_EQ_INT((int)0xFFFFEC76u, (int)call(OP_LOGIN)); // BadUAM
    stub_set_afp_version("AFPVersion 2.1");
    fixture_down();
}

// ============================================================================
// WP-6 / WP-7 — deny modes, byte-range locks, shared fork backing
// ============================================================================

#define ACC_READ   0x0001
#define ACC_WRITE  0x0002
#define DENY_READ  0x0010
#define DENY_WRITE 0x0020

// Inside AppleTalk ch. 13 "Synchronization rules": a second open fails when
// the requested access intersects the cumulative deny mode, or the requested
// deny intersects the cumulative access mode.  Walk the whole 4x4 matrix of
// (access, deny) pairs against a first open, and check each expectation
// against that rule rather than against a hand-written table.
TEST(open_fork_deny_matrix) {
    fixture_up("denymatrix");
    write_file("Doc", "contents");

    static const uint16_t modes[] = {
        ACC_READ,
        ACC_WRITE,
        ACC_READ | ACC_WRITE,
        ACC_READ | DENY_READ,
        ACC_READ | DENY_WRITE,
        ACC_WRITE | DENY_WRITE,
        ACC_READ | ACC_WRITE | DENY_READ | DENY_WRITE,
        DENY_READ | DENY_WRITE,
    };
    const int n = (int)(sizeof(modes) / sizeof(modes[0]));

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            uint16_t first = modes[i];
            uint16_t second = modes[j];
            uint16_t ref1 = 0, ref2 = 0;
            ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, first, &ref1));

            uint16_t cam = (uint16_t)(first & (ACC_READ | ACC_WRITE));
            uint16_t cdm = (uint16_t)(first & (DENY_READ | DENY_WRITE));
            uint16_t denied_access =
                (uint16_t)(((cdm & DENY_READ) ? ACC_READ : 0) | ((cdm & DENY_WRITE) ? ACC_WRITE : 0));
            uint16_t want_deny_as_access =
                (uint16_t)(((second & DENY_READ) ? ACC_READ : 0) | ((second & DENY_WRITE) ? ACC_WRITE : 0));
            bool expect_conflict =
                ((second & (ACC_READ | ACC_WRITE)) & denied_access) != 0 || (want_deny_as_access & cam) != 0;

            uint32_t rc = open_fork("Doc", false, second, &ref2);
            if (expect_conflict) {
                ASSERT_EQ_INT((int)ERR_DENY_CONFLICT, (int)rc);
            } else {
                ASSERT_EQ_INT((int)ERR_OK, (int)rc);
                close_fork(ref2);
            }
            close_fork(ref1);
        }
    }
    fixture_down();
}

TEST(deny_conflict_still_returns_the_file_parameters) {
    fixture_up("denyparms");
    write_file("Doc", "contents");
    uint16_t ref1 = 0, ref2 = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_WRITE | DENY_WRITE, &ref1));

    // Ask for the data-fork length alongside the conflicting open.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(0x0200); // data fork length
    put16(ACC_WRITE);
    put_path("Doc");
    ASSERT_EQ_INT((int)ERR_DENY_CONFLICT, (int)call(OP_OPEN_FORK));
    ASSERT_EQ_INT(0, (int)rd16(g_reply + 2)); // OForkRefNum is 0 on conflict
    ASSERT_EQ_INT(8, (int)rd32(g_reply + 4)); // ...but the parameters are there
    (void)ref2;
    close_fork(ref1);
    fixture_down();
}

TEST(byte_range_locks_report_overlap_and_release_on_close) {
    fixture_up("locks");
    write_file("Doc", "0123456789abcdef");
    uint16_t a = 0, b = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &a));
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &b));

    // FPByteRangeLock: Flag(1) OForkRefNum(2) Offset(4) Length(4).
    req_reset();
    put8(0x00); // lock, start-relative
    put16(a);
    put32(0);
    put32(8);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_BYTE_RANGE_LOCK));
    ASSERT_EQ_INT(0, (int)rd32(g_reply)); // reply carries the range start

    // The same fork overlapping its own lock is afpRangeOverlap...
    req_reset();
    put8(0x00);
    put16(a);
    put32(4);
    put32(8);
    ASSERT_EQ_INT((int)ERR_RANGE_OVERLAP, (int)call(OP_BYTE_RANGE_LOCK));

    // ...a different fork's is afpLockErr.
    req_reset();
    put8(0x00);
    put16(b);
    put32(4);
    put32(8);
    ASSERT_EQ_INT((int)ERR_LOCK, (int)call(OP_BYTE_RANGE_LOCK));

    // A read covered by a foreign lock is refused.
    req_reset();
    put8(0);
    put16(b);
    put32(0);
    put32(4);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_LOCK, (int)call(OP_READ));

    // Beyond the locked range it succeeds.
    req_reset();
    put8(0);
    put16(b);
    put32(8);
    put32(4);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));

    // Unlocking a range that was never locked is afpRangeNotLocked.
    req_reset();
    put8(0x01); // unlock
    put16(b);
    put32(100);
    put32(4);
    ASSERT_EQ_INT((int)ERR_RANGE_NOT_LOCK, (int)call(OP_BYTE_RANGE_LOCK));

    // Closing the holder releases its locks.
    close_fork(a);
    req_reset();
    put8(0x00);
    put16(b);
    put32(0);
    put32(8);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_BYTE_RANGE_LOCK));
    close_fork(b);
    fixture_down();
}

TEST(two_opens_of_one_fork_see_each_other_writes) {
    fixture_up("dualopen");
    write_file("Doc", "AAAAAAAA");
    uint16_t a = 0, b = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &a));
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &b));

    // FPWrite: Flag(1) OForkRefNum(2) Offset(4) ReqCount(4) + payload.
    req_reset();
    put8(0);
    put16(a);
    put32(0);
    put32(4);
    put_bytes("ZZZZ", 4);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));
    ASSERT_EQ_INT(4, (int)rd32(g_reply)); // LastWritten

    req_reset();
    put8(0);
    put16(b);
    put32(0);
    put32(4);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));
    ASSERT_EQ_INT(0, memcmp(g_reply, "ZZZZ", 4)); // no stale private copy

    close_fork(a);
    close_fork(b);
    fixture_down();
}

TEST(resource_fork_round_trips_through_the_sidecar) {
    fixture_up("rsrc");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("App"));

    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("App", true, ACC_READ | ACC_WRITE, &ref));
    static uint8_t payload[8192];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 31 + 7);
    // Write it in 512-byte chunks, the way a client streams a fork.
    for (size_t off = 0; off < sizeof(payload); off += 512) {
        req_reset();
        put8(0);
        put16(ref);
        put32((uint32_t)off);
        put32(512);
        put_bytes(payload + off, 512);
        ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));
    }
    close_fork(ref); // persists into "._App"

    // The reported resource-fork length comes from the sidecar's entry table.
    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("App", 0x0400, 0));
    ASSERT_EQ_INT((int)sizeof(payload), (int)rd32(g_reply + 6));

    // Re-open and compare byte for byte.
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("App", true, ACC_READ, &ref));
    for (size_t off = 0; off < sizeof(payload); off += 512) {
        req_reset();
        put8(0);
        put16(ref);
        put32((uint32_t)off);
        put32(512);
        put8(0);
        put8(0);
        ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));
        ASSERT_EQ_INT(512, g_reply_len);
        ASSERT_EQ_INT(0, memcmp(g_reply, payload + off, 512));
    }
    close_fork(ref);

    // The data fork is still a clean stream — an empty file, untouched.
    char path[512];
    struct stat st;
    host_path("App", path, sizeof(path));
    ASSERT_EQ_INT(0, stat(path, &st));
    ASSERT_EQ_INT(0, (int)st.st_size);
    fixture_down();
}

TEST(set_fork_parms_truncates_and_flush_persists) {
    fixture_up("trunc");
    write_file("Doc", "0123456789");
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &ref));

    // FPSetForkParms: Pad(1) OForkRefNum(2) Bitmap(2) Length(4).
    req_reset();
    put8(0);
    put16(ref);
    put16(0x0200); // data fork length
    put32(4);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_SET_FORK_PARMS));

    req_reset();
    put8(0);
    put16(ref);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_FLUSH_FORK));
    close_fork(ref);

    char path[512];
    struct stat st;
    host_path("Doc", path, sizeof(path));
    ASSERT_EQ_INT(0, stat(path, &st));
    ASSERT_EQ_INT(4, (int)st.st_size);

    // An undefined bitmap bit is an error.
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &ref));
    req_reset();
    put8(0);
    put16(ref);
    put16(0x0001);
    put32(0);
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)call(OP_SET_FORK_PARMS));
    close_fork(ref);
    fixture_down();
}

// FPWrite's reply is LastWritten precisely so a write may be partial: the
// transport can split a large write across ASP requests and the client
// resumes from where the server got to. Only a request carrying no payload at
// all is an error.
TEST(write_may_be_partial_and_reports_where_it_stopped) {
    fixture_up("partialwrite");
    write_file("Doc", "");
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &ref));

    // ReqCount says 10, only 4 bytes arrived: write those and say so.
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(10);
    put_bytes("ABCD", 4);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));
    ASSERT_EQ_INT(4, (int)rd32(g_reply));
    ASSERT_EQ_INT(4, (int)afp_fork_length(afp_fork_find(ref)));

    // The client resumes from LastWritten.
    req_reset();
    put8(0);
    put16(ref);
    put32(4);
    put32(6);
    put_bytes("EFGHIJ", 6);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));
    ASSERT_EQ_INT(10, (int)rd32(g_reply));

    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(10);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));
    ASSERT_EQ_INT(0, memcmp(g_reply, "ABCDEFGHIJ", 10));

    // A non-zero ReqCount with an empty payload is a truncated request.
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(4);
    ASSERT_EQ_INT((int)ERR_PARAM, (int)call(OP_WRITE));

    // A fork opened read-only refuses writes outright.
    uint16_t ro = 0;
    close_fork(ref);
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ, &ro));
    req_reset();
    put8(0);
    put16(ro);
    put32(0);
    put32(2);
    put_bytes("XY", 2);
    ASSERT_EQ_INT((int)ERR_ACCESS_DENIED, (int)call(OP_WRITE));
    close_fork(ro);
    fixture_down();
}

TEST(read_past_end_of_fork_is_eof) {
    fixture_up("eof");
    write_file("Doc", "1234");
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ, &ref));
    req_reset();
    put8(0);
    put16(ref);
    put32(4);
    put32(10);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_EOF, (int)call(OP_READ));
    // A short read that hits the end also reports EOF, with the bytes.
    req_reset();
    put8(0);
    put16(ref);
    put32(2);
    put32(10);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_EOF, (int)call(OP_READ));
    ASSERT_EQ_INT(2, g_reply_len);
    close_fork(ref);
    fixture_down();
}

// ============================================================================
// WP-9 — persistent desktop database
// ============================================================================

// FPOpenDT: Pad(1) VolumeID(2) -> DTRefNum(2).
static uint16_t open_dt(void) {
    req_reset();
    put8(0);
    put16(g_vol_id);
    uint32_t rc = call(OP_OPEN_DT);
    ASSERT_EQ_INT((int)ERR_OK, (int)rc);
    return rd16(g_reply);
}

TEST(icons_survive_a_share_reopen) {
    fixture_up("icons");
    uint16_t dt = open_dt();

    uint8_t bitmap[256];
    for (size_t i = 0; i < sizeof(bitmap); i++)
        bitmap[i] = (uint8_t)(i ^ 0x5A);

    // FPAddIcon: Pad(1) DTRefNum(2) Creator(4) FileType(4) IconType(1)
    // Pad(1) IconTag(4) BitmapSize(2) + bitmap.
    req_reset();
    put8(0);
    put16(dt);
    put32(0x41505054u); // 'APPT'
    put32(0x4150504Cu); // 'APPL'
    put8(1); // kLargeIcon
    put8(0);
    put32(0xDEADBEEFu);
    put16((uint16_t)sizeof(bitmap));
    put_bytes(bitmap, sizeof(bitmap));
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_ADD_ICON));

    // Drop and rebuild the whole server-side view of the share.
    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("TestVol", err, sizeof(err)));
    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) >= 0);
    g_vol_id = (uint16_t)atalk_afp_volume_vol_id(atalk_afp_volume_find("TestVol"));
    dt = open_dt();

    // FPGetIcon: Pad(1) DTRefNum(2) Creator(4) FileType(4) IconType(1) Length(2).
    req_reset();
    put8(0);
    put16(dt);
    put32(0x41505054u);
    put32(0x4150504Cu);
    put8(1);
    put16((uint16_t)sizeof(bitmap));
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_ICON));
    ASSERT_EQ_INT((int)sizeof(bitmap), g_reply_len);
    ASSERT_EQ_INT(0, memcmp(g_reply, bitmap, sizeof(bitmap)));

    // FPGetIconInfo: Pad(1) DTRefNum(2) Creator(4) IconIndex(2).
    req_reset();
    put8(0);
    put16(dt);
    put32(0x41505054u);
    put16(1);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_ICON_INFO));
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)rd32(g_reply));
    ASSERT_EQ_INT((int)0x4150504Cu, (int)rd32(g_reply + 4));
    ASSERT_EQ_INT(1, (int)g_reply[8]);
    ASSERT_EQ_INT((int)sizeof(bitmap), (int)rd16(g_reply + 10));

    // A miss is afpItemNotFound, per the DTDBMgr.a contract — never fnfErr.
    req_reset();
    put8(0);
    put16(dt);
    put32(0x4E4F4E45u);
    put32(0x4150504Cu);
    put8(1);
    put16(256);
    ASSERT_EQ_INT((int)ERR_ITEM_NOT_FOUND, (int)call(OP_GET_ICON));
    req_reset();
    put8(0);
    put16(dt);
    put32(0x4E4F4E45u);
    put16(1);
    ASSERT_EQ_INT((int)ERR_ITEM_NOT_FOUND, (int)call(OP_GET_ICON_INFO));

    fixture_down();
}

TEST(appl_mapping_is_cnid_keyed_and_survives_a_rename) {
    fixture_up("appl");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("TeachText"));
    uint16_t dt = open_dt();

    // FPAddAPPL: Pad(1) DTRefNum(2) DirectoryID(4) Creator(4) ApplTag(4)
    // PathType(1) Pathname.
    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put32(0x74747874u); // 'ttxt'
    put32(0x00000042u);
    put_path("TeachText");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_ADD_APPL));

    // FPGetAPPL: Pad(1) DTRefNum(2) Creator(4) Index(2) Bitmap(2).
    req_reset();
    put8(0);
    put16(dt);
    put32(0x74747874u);
    put16(1);
    put16(0x0040); // long name
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_APPL));
    ASSERT_EQ_INT((int)0x00000042u, (int)rd32(g_reply + 2));

    // Renaming the application through AFP must not orphan the mapping — the
    // old path-keyed store lost it here.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("TeachText");
    put_path("SimpleText");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RENAME));

    req_reset();
    put8(0);
    put16(dt);
    put32(0x74747874u);
    put16(1);
    put16(0x0040);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_APPL));
    const uint8_t *nm = g_reply + 6 + rd16(g_reply + 6);
    ASSERT_EQ_INT(10, (int)nm[0]);
    ASSERT_EQ_INT(0, memcmp(nm + 1, "SimpleText", 10));

    // An unknown creator is afpItemNotFound.
    req_reset();
    put8(0);
    put16(dt);
    put32(0x4E4F4E45u);
    put16(1);
    put16(0);
    ASSERT_EQ_INT((int)ERR_ITEM_NOT_FOUND, (int)call(OP_GET_APPL));

    fixture_down();
}

TEST(comments_live_in_the_sidecar_and_follow_the_file) {
    fixture_up("comments");
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));
    uint16_t dt = open_dt();

    // FPAddComment: Pad(1) DTRefNum(2) DirectoryID(4) PathType(1) Pathname
    // + Comment (Pascal string).
    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put_path("Doc");
    put_pstr("Written on the server");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_ADD_COMMENT));

    // A rename carries the comment along, because it lives in "._Doc".
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RENAME));

    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_COMMENT));
    ASSERT_EQ_INT(21, (int)g_reply[0]);
    ASSERT_EQ_INT(0, memcmp(g_reply + 1, "Written on the server", 21));

    // And it survives the server forgetting everything it held in memory.
    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("TestVol", err, sizeof(err)));
    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) >= 0);
    g_vol_id = (uint16_t)atalk_afp_volume_vol_id(atalk_afp_volume_find("TestVol"));
    dt = open_dt();
    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_COMMENT));
    ASSERT_EQ_INT(21, (int)g_reply[0]);

    // Removing it, then reading, is afpItemNotFound.
    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_RMV_COMMENT));
    req_reset();
    put8(0);
    put16(dt);
    put32(CNID_ROOT);
    put_path("Doc2");
    ASSERT_EQ_INT((int)ERR_ITEM_NOT_FOUND, (int)call(OP_GET_COMMENT));

    fixture_down();
}

// ============================================================================
// WP-10 — error fidelity
// ============================================================================

TEST(golden_error_codes_per_command) {
    fixture_up("errors");

    // FPCreateFile on an existing file: soft create is afpObjectExists.
    ASSERT_EQ_INT((int)ERR_OK, (int)create_file("Doc"));
    ASSERT_EQ_INT((int)ERR_OBJECT_EXISTS, (int)create_file("Doc"));

    // Hard create (flag bit 7) overwrites instead.
    req_reset();
    put8(0x80);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_CREATE_FILE));

    // ...unless a fork is open on it, which is afpFileBusy.
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ, &ref));
    req_reset();
    put8(0x80);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    ASSERT_EQ_INT((int)ERR_FILE_BUSY, (int)call(OP_CREATE_FILE));
    // FPDelete on an open file is afpFileBusy too.
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Doc");
    ASSERT_EQ_INT((int)ERR_FILE_BUSY, (int)call(OP_DELETE));
    close_fork(ref);

    // FPCreateDir on an existing name.
    uint32_t dir_id = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)create_dir("Folder", &dir_id));
    ASSERT_EQ_INT((int)ERR_OBJECT_EXISTS, (int)create_dir("Folder", NULL));

    // FPDelete on a non-empty directory.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(dir_id);
    put_path("Inner");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_CREATE_FILE));
    req_vol_dir_path(g_vol_id, CNID_ROOT, "Folder");
    ASSERT_EQ_INT((int)ERR_DIR_NOT_EMPTY, (int)call(OP_DELETE));

    // FPGetFileDirParms on a missing object, and with an empty bitmap.
    ASSERT_EQ_INT((int)ERR_OBJECT_NOT_FND, (int)get_fd_parms("Nope", 0x0100, 0x0100));
    ASSERT_EQ_INT((int)ERR_BITMAP, (int)get_fd_parms("Doc", 0, 0));

    // An unknown volume identifier is afpParamErr.
    req_reset();
    put8(0);
    put16(0xBEEF);
    put32(CNID_ROOT);
    put16(0x0100);
    put16(0x0100);
    put_path("Doc");
    ASSERT_EQ_INT((int)ERR_PARAM, (int)call(OP_GET_FD_PARMS));

    // An unknown opcode is afpCallNotSupported.
    req_reset();
    put8(0);
    ASSERT_EQ_INT((int)ERR_NOT_SUPPORTED, (int)call(0x7F));

    // A client path may not name the server's control directory: the
    // pathname is rejected outright, which the spec calls afpParmErr
    // ("pathname is null or bad"), not merely "not found".
    ASSERT_EQ_INT((int)ERR_PARAM, (int)get_fd_parms(".gs-afp", 0x0100, 0x0100));
    ASSERT_EQ_INT((int)ERR_PARAM, (int)get_fd_parms(".gs-afp/catalog.gsc", 0x0100, 0x0100));

    // FPRename onto an existing name is afpObjectExists.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Doc");
    put_path("Folder");
    ASSERT_EQ_INT((int)ERR_OBJECT_EXISTS, (int)call(OP_RENAME));

    fixture_down();
}

TEST(copy_file_copies_both_forks_and_refuses_an_existing_destination) {
    fixture_up("copy");
    write_file("Source", "data-fork");
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Source", true, ACC_READ | ACC_WRITE, &ref));
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(9);
    put_bytes("rsrc-fork", 9);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));
    close_fork(ref);

    // FPCopyFile: Pad(1) SrcVolID(2) SrcDirID(4) DstVolID(2) DstDirID(4)
    // SrcPathType(1) SrcPath DstPathType(1) DstPath NewType(1) NewName.
    // The two volume IDs are deliberately not adjacent — reading them as if
    // they were is what made the guest's Duplicate fail with afpDirNotFound.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Source");
    put_path("");
    put_path("Copy");
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_COPY_FILE));

    ASSERT_EQ_INT((int)ERR_OK, (int)get_fd_parms("Copy", 0x0600, 0)); // data + rsrc lengths
    ASSERT_EQ_INT(9, (int)rd32(g_reply + 6));
    ASSERT_EQ_INT(9, (int)rd32(g_reply + 10));

    // A second copy onto the same destination is afpObjectExists.
    req_reset();
    put8(0);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put16(g_vol_id);
    put32(CNID_ROOT);
    put_path("Source");
    put_path("");
    put_path("Copy");
    ASSERT_EQ_INT((int)ERR_OBJECT_EXISTS, (int)call(OP_COPY_FILE));

    fixture_down();
}

TEST(get_srvr_parms_lists_every_volume) {
    fixture_up("srvrparms");
    char err[192];
    char second[300];
    snprintf(second, sizeof(second), "%s-two", g_root);
    rm_rf(second);
    ASSERT_EQ_INT(0, mkdir(second, 0755));
    ASSERT_TRUE(atalk_afp_volume_add("Second", second, err, sizeof(err)) >= 0);

    req_reset();
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_GET_SRVR_PARMS));
    // ServerTime(4) NumVols(1) then {VolFlags(1), Pascal name} per volume.
    ASSERT_TRUE(rd32(g_reply) > 3029529600u); // an AFP timestamp, not Unix seconds
    ASSERT_EQ_INT(2, (int)g_reply[4]);
    ASSERT_EQ_INT(0, (int)g_reply[5]); // no volume password, no config info
    ASSERT_EQ_INT(7, (int)g_reply[6]);
    ASSERT_EQ_INT(0, memcmp(g_reply + 7, "TestVol", 7));

    atalk_afp_volume_remove("Second", err, sizeof(err));
    rm_rf(second);
    fixture_down();
}

// ============================================================================
// Object-model surface (proposal-appletalk-afp-object-model.md §2)
// ============================================================================

TEST(volume_add_reports_the_real_reason_it_failed) {
    fixture_up("errmsg");
    char err[192];
    ASSERT_TRUE(atalk_afp_volume_add("TestVol", g_root, err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "already exists") != NULL);

    ASSERT_TRUE(atalk_afp_volume_add("Other", "/definitely/not/here", err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "does not exist") != NULL);

    ASSERT_TRUE(atalk_afp_volume_add("aaaaaaaaaabbbbbbbbbbccccccccccddddd", g_root, err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "max 32 chars") != NULL);

    char file[512];
    host_path("PlainFile", file, sizeof(file));
    write_file("PlainFile", "x");
    ASSERT_TRUE(atalk_afp_volume_add("NotADir", file, err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "not a directory") != NULL);
    fixture_down();
}

TEST(stats_track_commands_bytes_and_error_codes) {
    fixture_up("stats");
    const atalk_afp_stats_t *st = atalk_afp_get_stats();
    uint64_t base_commands = st->commands_served;
    uint64_t base_errors = st->errors;

    write_file("Doc", "0123456789");
    uint16_t ref = 0;
    ASSERT_EQ_INT((int)ERR_OK, (int)open_fork("Doc", false, ACC_READ | ACC_WRITE, &ref));
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(10);
    put8(0);
    put8(0);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_READ));
    req_reset();
    put8(0);
    put16(ref);
    put32(0);
    put32(4);
    put_bytes("ABCD", 4);
    ASSERT_EQ_INT((int)ERR_OK, (int)call(OP_WRITE));

    st = atalk_afp_get_stats();
    ASSERT_TRUE(st->commands_served >= base_commands + 3);
    ASSERT_TRUE(st->bytes_read >= 10);
    ASSERT_TRUE(st->bytes_written >= 4);
    ASSERT_EQ_INT(1, (int)st->open_forks);

    // An error is counted both in the total and per code.
    uint64_t before = atalk_afp_error_count(-5018); // afpObjectNotFound
    ASSERT_EQ_INT((int)ERR_OBJECT_NOT_FND, (int)get_fd_parms("Missing", 0x0100, 0x0100));
    st = atalk_afp_get_stats();
    ASSERT_TRUE(st->errors > base_errors);
    ASSERT_EQ_INT((int)(before + 1), (int)atalk_afp_error_count(-5018));

    close_fork(ref);
    fixture_down();
}

TEST(disabling_the_server_refuses_commands) {
    fixture_up("disabled");
    char err[192];
    ASSERT_EQ_INT(0, atalk_afp_set_enabled(false, err, sizeof(err)));
    ASSERT_EQ_INT(0, (int)atalk_afp_get_enabled());
    // afpServerGoingDown, so a client stops rather than retrying blindly.
    ASSERT_EQ_INT((int)0xFFFFEC5Du, (int)get_fd_parms("Doc", 0x0100, 0x0100));
    ASSERT_EQ_INT(0, atalk_afp_set_enabled(true, err, sizeof(err)));
    ASSERT_EQ_INT(1, (int)atalk_afp_get_enabled());
    fixture_down();
}

TEST(server_name_and_versions_are_readable_and_settable) {
    fixture_up("identity");
    char err[192];
    ASSERT_EQ_INT(0, strcmp(atalk_afp_get_name(), "Shared Folders"));
    ASSERT_EQ_INT(0, atalk_afp_set_name("Attic Mac", err, sizeof(err)));
    ASSERT_EQ_INT(0, strcmp(atalk_afp_get_name(), "Attic Mac"));
    ASSERT_TRUE(atalk_afp_set_name("aaaaaaaaaabbbbbbbbbbccccccccccddddd", err, sizeof(err)) < 0);
    ASSERT_EQ_INT(0, atalk_afp_set_name("Shared Folders", err, sizeof(err)));

    int count = 0;
    const char *const *versions = atalk_afp_versions(&count);
    ASSERT_EQ_INT(2, count);
    ASSERT_EQ_INT(0, strcmp(versions[0], "AFPVersion 2.0"));
    ASSERT_EQ_INT(0, strcmp(versions[1], "AFPVersion 2.1"));
    fixture_down();
}

// ============================================================================
// afp_meta — the AppleDouble metadata codec
// ============================================================================

TEST(meta_round_trips_every_entry_and_drops_an_empty_sidecar) {
    fixture_up("meta");
    char path[512];
    host_path("Doc", path, sizeof(path));
    write_file("Doc", "data");

    afp_meta_t m;
    memset(&m, 0, sizeof(m));
    m.has_dates = true;
    m.create_date = 0xB0000000u;
    m.modify_date = 0xB1000000u;
    m.backup_date = AFP_DATE_NEVER;
    m.access_date = 0xB3000000u;
    m.has_finder = true;
    for (int i = 0; i < AFP_META_FINDER_SIZE; i++)
        m.finder[i] = (uint8_t)(0xA0 + i);
    m.has_attrs = true;
    m.attrs = AFP_ATTR_INVISIBLE | AFP_ATTR_DELETEINHIBIT;
    m.has_comment = true;
    snprintf(m.comment, sizeof(m.comment), "hello");
    m.comment_len = 5;

    uint8_t rsrc[64];
    for (size_t i = 0; i < sizeof(rsrc); i++)
        rsrc[i] = (uint8_t)i;
    ASSERT_EQ_INT(0, afp_meta_store(path, &m, rsrc, sizeof(rsrc)));

    afp_meta_t back;
    ASSERT_TRUE(afp_meta_load(path, &back));
    ASSERT_TRUE(back.has_dates);
    ASSERT_EQ_INT((int)0xB0000000u, (int)back.create_date);
    ASSERT_EQ_INT((int)0xB1000000u, (int)back.modify_date);
    ASSERT_EQ_INT((int)AFP_DATE_NEVER, (int)back.backup_date);
    ASSERT_TRUE(back.has_finder);
    ASSERT_EQ_INT(0, memcmp(back.finder, m.finder, AFP_META_FINDER_SIZE));
    ASSERT_TRUE(back.has_attrs);
    ASSERT_EQ_INT(m.attrs, back.attrs);
    ASSERT_TRUE(back.has_comment);
    ASSERT_EQ_INT(5, (int)back.comment_len);
    ASSERT_EQ_INT(0, strcmp(back.comment, "hello"));
    ASSERT_EQ_INT((int)sizeof(rsrc), (int)afp_meta_rsrc_len(path));

    // Storing nothing removes the sidecar so a plain file stays a clean stream.
    afp_meta_t empty;
    memset(&empty, 0, sizeof(empty));
    ASSERT_EQ_INT(0, afp_meta_store(path, &empty, NULL, 0));
    char sc[512];
    ASSERT_TRUE(afp_meta_sidecar_path(path, sc, sizeof(sc)));
    ASSERT_TRUE(access(sc, F_OK) != 0);
    fixture_down();
}

TEST(meta_streams_a_fork_without_holding_it_in_memory) {
    fixture_up("metastream");
    char path[512];
    host_path("Big", path, sizeof(path));
    write_file("Big", "");

    // 4 MB — well past the old 32 MB whole-fork malloc's intent, and large
    // enough that a buffered implementation would be obvious.
    const size_t len = 4u * 1024u * 1024u;
    FILE *src = tmpfile();
    ASSERT_TRUE(src != NULL);
    uint8_t chunk[4096];
    for (size_t i = 0; i < sizeof(chunk); i++)
        chunk[i] = (uint8_t)(i * 13);
    for (size_t off = 0; off < len; off += sizeof(chunk))
        ASSERT_EQ_INT((int)sizeof(chunk), (int)fwrite(chunk, 1, sizeof(chunk), src));
    rewind(src);

    afp_meta_t m;
    memset(&m, 0, sizeof(m));
    ASSERT_EQ_INT(0, afp_meta_store_stream(path, &m, src, len));
    fclose(src);
    ASSERT_EQ_INT((int)len, (int)afp_meta_rsrc_len(path));

    FILE *dst = tmpfile();
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ_INT((int)len, (int)afp_meta_copy_rsrc(path, dst));
    rewind(dst);
    uint8_t got[4096];
    ASSERT_EQ_INT((int)sizeof(got), (int)fread(got, 1, sizeof(got), dst));
    ASSERT_EQ_INT(0, memcmp(got, chunk, sizeof(chunk)));
    fclose(dst);
    fixture_down();
}

TEST(meta_hides_the_names_that_back_server_state) {
    ASSERT_TRUE(afp_meta_is_hidden("._Doc"));
    ASSERT_TRUE(afp_meta_is_hidden("Doc.rsrc"));
    ASSERT_TRUE(afp_meta_is_hidden(".gs-afp"));
    ASSERT_TRUE(!afp_meta_is_hidden("Doc"));
    ASSERT_TRUE(!afp_meta_is_hidden(".hidden"));
}

TEST(meta_time_conversions_round_trip) {
    // The AFP epoch is 1904; a 2026 date is ~3.85e9 seconds from it.
    uint32_t afp = afp_meta_time_from_unix(1776000000);
    ASSERT_EQ_INT(1776000000, (int)afp_meta_time_to_unix(afp));
    // The "never" sentinel maps to 0 so it can be handed to utimes().
    ASSERT_EQ_INT(0, (int)afp_meta_time_to_unix(AFP_DATE_NEVER));
}

// ============================================================================
// afp_desktop — the stores in isolation
// ============================================================================

TEST(desktop_store_reopens_and_prunes) {
    fixture_up("dtmodule");
    afp_desktop_t *dt = afp_desktop_open(g_root);
    ASSERT_TRUE(dt != NULL);
    uint8_t bits[32];
    memset(bits, 0xC3, sizeof(bits));
    ASSERT_EQ_INT(0, afp_desktop_put_icon(dt, 1, 2, 3, 0x99, bits, sizeof(bits)));
    ASSERT_EQ_INT(0, afp_desktop_put_appl(dt, 1, 100, 7));
    ASSERT_EQ_INT(0, afp_desktop_put_appl(dt, 1, 200, 8));
    afp_desktop_close(dt);

    dt = afp_desktop_open(g_root);
    ASSERT_TRUE(dt != NULL);
    const afp_icon_t *icon = afp_desktop_get_icon(dt, 1, 2, 3);
    ASSERT_TRUE(icon != NULL);
    ASSERT_EQ_INT((int)sizeof(bits), (int)icon->size);
    ASSERT_EQ_INT(0, memcmp(icon->bitmap, bits, sizeof(bits)));
    ASSERT_TRUE(afp_desktop_get_icon(dt, 9, 9, 9) == NULL);
    ASSERT_TRUE(afp_desktop_appl_at(dt, 1, 1) != NULL);
    ASSERT_TRUE(afp_desktop_appl_at(dt, 1, 2) != NULL);
    ASSERT_TRUE(afp_desktop_appl_at(dt, 1, 3) == NULL);

    // An oversize bitmap is rejected rather than silently truncated.
    static uint8_t huge[AFP_ICON_MAX_BYTES + 1];
    ASSERT_TRUE(afp_desktop_put_icon(dt, 1, 2, 4, 0, huge, sizeof(huge)) != 0);

    ASSERT_EQ_INT(1, afp_desktop_remove_appl(dt, 1, 100));
    ASSERT_EQ_INT(0, afp_desktop_remove_appl(dt, 1, 100));
    afp_desktop_close(dt);
    fixture_down();
}

// ============================================================================

int main(void) {
    RUN(vol_parms_report_real_sizes_and_dates);
    RUN(set_vol_parms_persists_the_backup_date);

    RUN(set_parms_finds_finder_info_behind_every_preceding_bit);
    RUN(set_parms_round_trips_dates_and_write_through_mtime);
    RUN(locked_attribute_blocks_delete_and_rename);
    RUN(write_inhibit_blocks_opening_a_fork_for_writing);
    RUN(set_parms_rejects_read_only_bits);

    RUN(cnid_survives_rename_move_and_is_never_reused);
    RUN(cnids_persist_across_a_share_remove_and_readd);
    RUN(catalog_survives_a_torn_tail_record);
    RUN(out_of_band_files_are_adopted_on_first_touch);
    RUN(catalog_module_reconstructs_paths_and_sweeps_stale_entries);

    RUN(enumerate_lists_every_entry_of_a_large_directory);
    RUN(enumerate_pages_are_served_from_a_snapshot);
    RUN(enumerate_hides_sidecars_and_the_control_directory);
    RUN(enumerate_rejects_an_empty_bitmap);

    RUN(file_id_lifecycle);
    RUN(create_id_refuses_directories_and_missing_files);
    RUN(exchange_files_moves_contents_and_keeps_ids_with_names);
    RUN(exchange_files_keeps_an_open_fork_pointed_at_its_bytes);
    RUN(cat_search_matches_on_name_and_resumes_from_cat_position);
    RUN(cat_search_partial_name_matches_several);
    RUN(cat_search_rejects_criteria_it_cannot_serve);
    RUN(server_message_round_trips_and_raises_an_attention);
    RUN(afp_21_commands_are_refused_on_a_20_session);
    RUN(login_negotiates_a_known_version_and_uam);

    RUN(open_fork_deny_matrix);
    RUN(deny_conflict_still_returns_the_file_parameters);
    RUN(byte_range_locks_report_overlap_and_release_on_close);
    RUN(two_opens_of_one_fork_see_each_other_writes);
    RUN(resource_fork_round_trips_through_the_sidecar);
    RUN(set_fork_parms_truncates_and_flush_persists);
    RUN(write_may_be_partial_and_reports_where_it_stopped);
    RUN(read_past_end_of_fork_is_eof);

    RUN(icons_survive_a_share_reopen);
    RUN(appl_mapping_is_cnid_keyed_and_survives_a_rename);
    RUN(comments_live_in_the_sidecar_and_follow_the_file);

    RUN(golden_error_codes_per_command);
    RUN(copy_file_copies_both_forks_and_refuses_an_existing_destination);
    RUN(get_srvr_parms_lists_every_volume);

    RUN(volume_add_reports_the_real_reason_it_failed);
    RUN(stats_track_commands_bytes_and_error_codes);
    RUN(disabling_the_server_refuses_commands);
    RUN(server_name_and_versions_are_readable_and_settable);

    RUN(meta_round_trips_every_entry_and_drops_an_empty_sidecar);
    RUN(meta_streams_a_fork_without_holding_it_in_memory);
    RUN(meta_hides_the_names_that_back_server_state);
    RUN(meta_time_conversions_round_trip);
    RUN(desktop_store_reopens_and_prunes);

    fprintf(stderr, "All AFP tests passed\n");
    return 0;
}
