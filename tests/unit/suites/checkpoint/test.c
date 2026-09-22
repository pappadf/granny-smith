// The checkpoint stream reader, tested as an untrusted parser.
//
// 08-WORK-ORDER.md Track C, unit C0.  There was no core checkpoint suite:
// scsi_checkpoint and scc_checkpoint each cover one device's blocks, and
// nothing exercised checkpoint.c's own read path.  That absence is how F-05
// (on-disk counts validated only by GS_ASSERT) and F-06 (length checks that
// overflow on the 32-bit wasm heap) came to be written and to survive review.
//
// What a checkpoint actually is: a file the user supplies.  `checkpoint --load
// <path>`, a drag-and-drop into the browser, or the quick checkpoint the
// browser writes to OPFS every 15 seconds and reads back after a reload.  The
// reader must therefore treat every length, count and string in the stream as
// hostile, and the tests below corrupt real files by hand rather than calling
// internal helpers with crafted arguments.
//
// Structure of each test: build a VALID stream through the public writer, then
// damage exactly one field, then assert the reader refuses it.  The valid
// round-trips come first so that nothing here can pass vacuously -- a reader
// that rejected everything would fail those.
//
// Defect injection, run while writing this suite (08-WORK-ORDER.md 7 requires
// it, and it changed one test's documented claim):
//
//   guard disabled                          test that failed
//   --------------------------------------  ----------------------------------
//   build-ID comparison                     foreign_build_id_is_refused
//   v3 block size check                     corrupt_quick_payload_is_refused
//   checkpoint_read_count's cap             bounded_count_refuses_over_cap
//   checkpoint_read_string's terminator     bounded_string_terminates_...
//   v2 size-header short read               NONE -- see truncated_stream_...
//   CP_MAX_FNAME cap                        NONE -- see block_header_filen...
//   rle_decode's subtraction form           NONE -- 64-bit host, see below
//
// The last three rows are the honest part.  Each of those guards is backed by
// another that reaches the same verdict on THIS host, so disabling one alone
// leaves the suite green; the comment on each test says which guard actually
// caught it and why the one under test still matters on the 32-bit shipping
// target.  A green suite is not evidence those guards are unnecessary.

#include "build_id.h"
#include "checkpoint.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_PATH "_checkpoint_test.checkpoint"

// === Stubs for everything past checkpoint.c's module boundary ================
//
// Note what this list is evidence of: linking the checkpoint module alone drags
// in cmd_load_checkpoint and cmd_save_checkpoint from system.c, because the
// typed methods wrap the retired argc/argv handlers (F-31, Track I).  When that
// track lands, four of these stubs should disappear.

static char g_build_id[BUILD_ID_LEN + 1] = "unit-test-build-0001";

const char *get_build_id(void) {
    return g_build_id;
}

uint64_t cmd_load_checkpoint(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return 1;
}
uint64_t cmd_save_checkpoint(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return 1;
}
const char *find_valid_checkpoint_path(void) {
    return NULL;
}
bool gs_background_checkpoint(const char *label) {
    (void)label;
    return false;
}
bool gs_checkpoint_auto_get(void) {
    return false;
}
void gs_checkpoint_auto_set(bool on) {
    (void)on;
}
void gs_checkpoint_clear(void) {}

// === File helpers ===========================================================

static void cp_unlink(void) {
    remove(CP_PATH);
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

// Truncate the file to `bytes`, simulating a checkpoint cut short by a crash,
// a full disk, or a partial download.
static void truncate_to(const char *path, long bytes) {
    long n = file_size(path);
    if (n < 0 || bytes >= n)
        return;
    uint8_t *buf = (uint8_t *)malloc((size_t)bytes);
    FILE *f = fopen(path, "rb");
    size_t got = fread(buf, 1, (size_t)bytes, f);
    fclose(f);
    f = fopen(path, "wb");
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
}

// Overwrite `len` bytes at `off`.  The tests use this to poison exactly one
// length field, leaving the rest of a valid stream intact.
static void poke(const char *path, long off, const void *data, size_t len) {
    FILE *f = fopen(path, "r+b");
    if (!f)
        return;
    fseek(f, off, SEEK_SET);
    fwrite(data, 1, len, f);
    fclose(f);
}

// Write a small, valid checkpoint of the given kind: three blocks whose
// contents are distinctive enough to detect cross-loading.
static const uint32_t BLOCK_A = 0xA1A2A3A4u;
static const uint64_t BLOCK_B = 0xB1B2B3B4B5B6B7B8ull;
static const uint16_t BLOCK_C = 0xC1C2u;

static void write_valid(checkpoint_kind_t kind) {
    cp_unlink();
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, kind, "plus", 4096);
    ASSERT_TRUE(cp != NULL);
    uint32_t a = BLOCK_A;
    uint64_t b = BLOCK_B;
    uint16_t c = BLOCK_C;
    system_write_checkpoint_data(cp, &a, sizeof(a));
    system_write_checkpoint_data(cp, &b, sizeof(b));
    system_write_checkpoint_data(cp, &c, sizeof(c));
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    checkpoint_close(cp);
}

// === Round trips ============================================================
//
// These exist so the rejection tests below cannot pass vacuously.

TEST(consolidated_round_trip) {
    write_valid(CHECKPOINT_KIND_CONSOLIDATED);
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t a = 0;
    uint64_t b = 0;
    uint16_t c = 0;
    system_read_checkpoint_data(cp, &a, sizeof(a));
    system_read_checkpoint_data(cp, &b, sizeof(b));
    system_read_checkpoint_data(cp, &c, sizeof(c));
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    ASSERT_TRUE(a == BLOCK_A);
    ASSERT_TRUE(b == BLOCK_B);
    ASSERT_TRUE(c == BLOCK_C);
    ASSERT_EQ_INT((int)checkpoint_get_kind(cp), (int)CHECKPOINT_KIND_CONSOLIDATED);
    ASSERT_TRUE(strcmp(checkpoint_get_model_id(cp), "plus") == 0);
    ASSERT_EQ_INT((int)checkpoint_get_ram_size_kb(cp), 4096);
    checkpoint_close(cp);
    cp_unlink();
}

TEST(quick_round_trip) {
    write_valid(CHECKPOINT_KIND_QUICK);
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t a = 0;
    uint64_t b = 0;
    uint16_t c = 0;
    system_read_checkpoint_data(cp, &a, sizeof(a));
    system_read_checkpoint_data(cp, &b, sizeof(b));
    system_read_checkpoint_data(cp, &c, sizeof(c));
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    ASSERT_TRUE(a == BLOCK_A);
    ASSERT_TRUE(b == BLOCK_B);
    ASSERT_TRUE(c == BLOCK_C);
    ASSERT_EQ_INT((int)checkpoint_get_kind(cp), (int)CHECKPOINT_KIND_QUICK);
    checkpoint_close(cp);
    cp_unlink();
}

// === The build-ID gate ======================================================
//
// This is the only compatibility check the format has -- there is no version
// integer anywhere (F-21).  It is worth pinning precisely because so much rests
// on it, and because the work order's C4 adds a version field beside it.

TEST(foreign_build_id_is_refused) {
    write_valid(CHECKPOINT_KIND_CONSOLIDATED);
    // Repoint the stub at a different build, exactly as loading someone else's
    // checkpoint would.
    memcpy(g_build_id, "unit-test-build-0002", BUILD_ID_LEN);
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp == NULL);
    memcpy(g_build_id, "unit-test-build-0001", BUILD_ID_LEN);
    // ...and the same file loads again once the build matches, so the test is
    // pinning the ID comparison rather than some unrelated rejection.
    cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    checkpoint_close(cp);
    cp_unlink();
}

// === Truncation =============================================================

TEST(truncated_stream_sets_error_not_garbage) {
    write_valid(CHECKPOINT_KIND_CONSOLIDATED);
    long full = file_size(CP_PATH);
    ASSERT_TRUE(full > 0);
    // Cut the last block in half.  The header and the first blocks survive, so
    // the reader gets far enough to matter.
    truncate_to(CP_PATH, full - 6);

    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t a = 0;
    uint64_t b = 0;
    uint16_t c = 0xFFFF;
    system_read_checkpoint_data(cp, &a, sizeof(a));
    system_read_checkpoint_data(cp, &b, sizeof(b));
    system_read_checkpoint_data(cp, &c, sizeof(c));
    // The contract that matters: a short file is an ERROR, not a silent
    // half-restore.  F-26 was three subsystems restoring nothing and reporting
    // success; this is the same failure mode one level down.
    //
    // Scope of this test, measured rather than assumed.  The v2 read path has
    // FOUR independent guards, any one of which catches a truncated stream:
    // the size-header short read, the stored_size/size mismatch, the
    // compression-flag short read, and the data short read.  Disabling any one
    // of them -- or the first two together -- leaves this test passing.  So it
    // pins the CONTRACT, not a particular guard, and it is deliberately not
    // claimed as the injection test for any single line.  That is the right
    // shape here: which guard fires is an implementation detail C1-C3 are
    // about to move, and a test coupled to one of them would have to be
    // rewritten by the work it is meant to protect.
    ASSERT_EQ_INT(checkpoint_has_error(cp), 1);
    checkpoint_close(cp);
    cp_unlink();
}

// === Block-order divergence =================================================
//
// The stream is positional: no per-block tag, no version (F-21).  Ordering
// integrity rests entirely on two lists in two functions staying in sync, and
// F-20 is the proof that they do not -- the IIfx saved ASC/ADB/floppy and
// restored ASC/floppy/ADB, which broke checkpoint.load on that machine
// outright.  In the consolidated format a size mismatch is at least caught.
// This test pins that much, and C4's per-block identity is what will let the
// SAME-sized case be caught too.

TEST(block_size_divergence_is_caught_in_v2) {
    write_valid(CHECKPOINT_KIND_CONSOLIDATED);
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    // Read the blocks back in the WRONG order -- a restore path that disagrees
    // with its save path, which is F-20 reduced to three fields.
    uint64_t b = 0;
    system_read_checkpoint_data(cp, &b, sizeof(b)); // stream holds a 4-byte block
    ASSERT_EQ_INT(checkpoint_has_error(cp), 1);
    checkpoint_close(cp);
    cp_unlink();
}

// === F-06: the length arithmetic that only overflows on wasm32 ==============
//
// rle_decode guards its copies with `op + count > out_size` and
// `ip + count > in_size`, where count is a uint32_t read straight from the
// file.  On this 64-bit test host size_t is 64 bits, the sum cannot wrap, and
// the guard is correct -- so a test that feeds the real decoder a huge count
// passes on BOTH the old and the fixed code, and proves nothing.
//
// The shipping target is emcc/WebAssembly, where size_t is 32 bits and the sum
// DOES wrap.  This test therefore reproduces the wasm32 arithmetic explicitly,
// in uint32_t, and asserts the two forms disagree.  It is a test of the
// arithmetic rather than of the function: the honest claim is "the guard as
// written admits a count it must reject on a 32-bit target", not "the decoder
// was observed overflowing here".
//
// The real-decoder coverage is the test below it -- and note that reverting
// rle_decode to the summing form does NOT fail anything here, for exactly the
// reason above.  That is a limit of testing a 32-bit defect on a 64-bit host,
// not evidence the change is unnecessary.

static bool guard_as_written_32(uint32_t op, uint32_t count, uint32_t out_size) {
    return op + count > out_size; // rejects when true
}

static bool guard_fixed_32(uint32_t op, uint32_t count, uint32_t out_size) {
    return count > out_size - op; // rejects when true
}

TEST(rle_length_guard_wraps_on_32bit_targets) {
    const uint32_t out_size = 64;
    const uint32_t op = 16;
    // A count chosen so that op + count wraps past 2^32 and lands inside the
    // buffer: 16 + 0xFFFFFFF8 == 8 (mod 2^32), which is <= 64.
    const uint32_t count = 0xFFFFFFF8u;

    ASSERT_EQ_INT((int)(op + count), 8); // the wrap, stated outright
    ASSERT_EQ_INT(guard_as_written_32(op, count, out_size), 0); // ADMITS it
    ASSERT_EQ_INT(guard_fixed_32(op, count, out_size), 1); // rejects it

    // And the fixed form still accepts everything legitimate, so C2 cannot
    // "fix" this by rejecting all input.
    ASSERT_EQ_INT(guard_fixed_32(16, 48, 64), 0); // exactly fills
    ASSERT_EQ_INT(guard_fixed_32(16, 49, 64), 1); // one past
    ASSERT_EQ_INT(guard_fixed_32(0, 64, 64), 0);
    ASSERT_EQ_INT(guard_fixed_32(64, 1, 64), 1); // no room left
}

// A corrupt compressed payload must be refused by the real decoder rather than
// decoded into whatever it happens to reach.  This runs the actual quick-format
// read path, so unlike the arithmetic test above it exercises rle_decode itself.
TEST(corrupt_quick_payload_is_refused) {
    write_valid(CHECKPOINT_KIND_QUICK);
    long sz = file_size(CP_PATH);
    ASSERT_TRUE(sz > 0);
    // Scribble over the tail of the compressed payload.  Whatever the reader
    // makes of it, it must not report success.
    uint8_t junk[16];
    memset(junk, 0xFF, sizeof(junk));
    poke(CP_PATH, sz - (long)sizeof(junk), junk, sizeof(junk));

    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    if (cp) {
        uint32_t a = 0;
        uint64_t b = 0;
        uint16_t c = 0;
        system_read_checkpoint_data(cp, &a, sizeof(a));
        system_read_checkpoint_data(cp, &b, sizeof(b));
        system_read_checkpoint_data(cp, &c, sizeof(c));
        bool err = checkpoint_has_error(cp);
        bool values_intact = (a == BLOCK_A && b == BLOCK_B && c == BLOCK_C);
        // Either the stream was refused, or the damage fell in slack that does
        // not change the decoded bytes.  What must NOT happen is a clean read
        // of wrong values.
        ASSERT_TRUE(err || values_intact);
        checkpoint_close(cp);
    }
    cp_unlink();
}

// === Bounded reads (F-22, F-23, F-24 via the C3 helpers) ====================
//
// checkpoint_read_count() and checkpoint_read_string() are the shape that
// makes the next variable-length field correct by construction, so they are
// tested directly rather than only through their callers -- the callers
// (checkpoint_images.c's restore loop and its two path strings) need a config
// and an image stack that this suite deliberately does not build.

TEST(bounded_count_accepts_within_cap) {
    cp_unlink();
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, CHECKPOINT_KIND_CONSOLIDATED, "plus", 4096);
    ASSERT_TRUE(cp != NULL);
    uint32_t n = 7;
    system_write_checkpoint_data(cp, &n, sizeof(n));
    checkpoint_close(cp);

    cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t got = 0;
    ASSERT_EQ_INT(checkpoint_read_count(cp, &got, 10, "images"), 1);
    ASSERT_EQ_INT((int)got, 7);
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    checkpoint_close(cp);
    cp_unlink();
}

TEST(bounded_count_refuses_over_cap) {
    cp_unlink();
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, CHECKPOINT_KIND_CONSOLIDATED, "plus", 4096);
    ASSERT_TRUE(cp != NULL);
    uint32_t n = 0xFFFFFFFFu; // the value F-24 turned into a four-billion-iteration loop
    system_write_checkpoint_data(cp, &n, sizeof(n));
    checkpoint_close(cp);

    cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t got = 12345;
    ASSERT_EQ_INT(checkpoint_read_count(cp, &got, 10, "images"), 0);
    ASSERT_EQ_INT((int)got, 0); // cleared, so a caller that ignores the return still loops zero times
    ASSERT_EQ_INT(checkpoint_has_error(cp), 1);
    checkpoint_close(cp);
    cp_unlink();
}

// The writer includes its own NUL in the length (image.c writes strlen+1), and
// the old reader trusted that: malloc(len), read len, then hand the buffer to
// access(), image_open_with_geometry() and printf("%s").  A file that omits
// the terminator read off the end of the allocation.  This writes a string
// with NO terminator and requires the reader to supply one.
TEST(bounded_string_terminates_what_the_writer_did_not) {
    cp_unlink();
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, CHECKPOINT_KIND_CONSOLIDATED, "plus", 4096);
    ASSERT_TRUE(cp != NULL);
    uint32_t len = 5;
    system_write_checkpoint_data(cp, &len, sizeof(len));
    system_write_checkpoint_data(cp, "abcde", 5); // deliberately no '\0'
    checkpoint_close(cp);

    cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    char *str = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image path");
    ASSERT_TRUE(str != NULL);
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    ASSERT_EQ_INT((int)strlen(str), 5); // terminated by the reader, not the file
    ASSERT_EQ_INT(memcmp(str, "abcde", 5), 0);
    free(str);
    checkpoint_close(cp);
    cp_unlink();
}

TEST(bounded_string_refuses_over_cap) {
    cp_unlink();
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, CHECKPOINT_KIND_CONSOLIDATED, "plus", 4096);
    ASSERT_TRUE(cp != NULL);
    uint32_t len = 0xFFFFFFFFu; // F-22/F-23: a 4 GB malloc on the 32-bit wasm heap
    system_write_checkpoint_data(cp, &len, sizeof(len));
    checkpoint_close(cp);

    cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    char *str = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image path");
    ASSERT_TRUE(str == NULL);
    ASSERT_EQ_INT(checkpoint_has_error(cp), 1);
    checkpoint_close(cp);
    cp_unlink();
}

// F-22 in its own right: the length is in the v2 BLOCK HEADER, below the
// system_read_checkpoint_data layer the helpers above sit on, so it has its
// own bounded reader and needs its own test.  The block header stores the
// save site's __FILE__, which is this file -- so the stream contains the
// literal path, and the four bytes before it are the length to poison.
TEST(block_header_filename_length_is_bounded) {
    write_valid(CHECKPOINT_KIND_CONSOLIDATED);

    long sz = file_size(CP_PATH);
    ASSERT_TRUE(sz > 0);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    FILE *f = fopen(CP_PATH, "rb");
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    // Locate this file's name inside the first block header.  It must be the
    // WHOLE __FILE__ string, not a suffix of it: the layout is
    // [uint64 size][uint32 fname_len][fname bytes][int32 line][uint8 flag],
    // so the length sits four bytes before the START of the path.  Matching a
    // suffix put `at - 4` inside the path text, and the first version of this
    // test poisoned four characters of the filename instead of its length --
    // which the reader tolerated, because the name is diagnostic only.
    const char *needle = __FILE__;
    size_t nlen = strlen(needle);
    long at = -1;
    for (size_t i = 0; i + nlen <= got; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            at = (long)i;
            break;
        }
    }
    free(buf);
    ASSERT_TRUE(at >= 4); // the length sits immediately before the name

    uint32_t huge = 0xFFFFFFFFu;
    poke(CP_PATH, at - 4, &huge, sizeof(huge));

    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    uint32_t a = 0;
    system_read_checkpoint_data(cp, &a, sizeof(a));
    ASSERT_EQ_INT(checkpoint_has_error(cp), 1);
    checkpoint_close(cp);
    cp_unlink();

    // Scope, measured: removing the CP_MAX_FNAME cap leaves this test passing,
    // because a four-billion-byte malloc fails on this host and the
    // out-of-memory branch flags the checkpoint instead.  So this pins the
    // contract -- an absurd length is refused -- and not the cap specifically.
    //
    // The cap is still the guard that matters, and the reason it cannot be
    // isolated here is the same reason F-06 cannot be: word size.  On the
    // 32-bit wasm heap a 4 GB request is the entire address space, and the
    // point of the cap is that the allocation is never ATTEMPTED.  A 64-bit
    // host with 100+ GB of address space reaches the same verdict by a route
    // the shipping target does not have.
}

int main(void) {
    cp_unlink();
    RUN(consolidated_round_trip);
    RUN(quick_round_trip);
    RUN(foreign_build_id_is_refused);
    RUN(truncated_stream_sets_error_not_garbage);
    RUN(block_size_divergence_is_caught_in_v2);
    RUN(rle_length_guard_wraps_on_32bit_targets);
    RUN(corrupt_quick_payload_is_refused);
    RUN(bounded_count_accepts_within_cap);
    RUN(bounded_count_refuses_over_cap);
    RUN(bounded_string_terminates_what_the_writer_did_not);
    RUN(bounded_string_refuses_over_cap);
    RUN(block_header_filename_length_is_bounded);
    cp_unlink();
    return 0;
}
