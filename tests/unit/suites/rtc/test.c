// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// RTC / PRAM unit test (code review 2026-09-03, 06-io-controllers unit A1).
// Links the real rtc.c against recording stubs and pins three things the
// module had no test for at all.
//
// 1. THE WRITE-PROTECT LAW APPLIES TO BOTH ADDRESSING WINDOWS.
//
//    The chip has one 256-byte array and one protect latch, reached two ways.
//    Macintosh Hardware Overview rev.2 (local/gs-docs/library/books/
//    apple-mac-hardware-overview-rev2-1991/single-file.md:1759):
//
//      "A 256-byte battery-backed-up RAM on the RTC holds system configuration
//       information and control panel settings.  It is organized as 8 sectors
//       of 32 bytes each, accessed by the 'extended address' mode.  Twenty
//       bytes of RAM can also be directly accessed in order to provide
//       compatibility with an older version of the RTC (Apple part number
//       343-0040), which had only 20 bytes of storage."
//
//    Before A1 the model enforced protection on the legacy window and ignored
//    it on the extended one — so the SAME physical byte was protected or not
//    depending on which command form reached it.  That self-inconsistency is
//    what this pins; no document we hold states the real 343-0042's behaviour
//    directly (Guide 2e p.3492 explicitly declines to document the command
//    set, and the Overview names the spec without reproducing it), so the
//    argument is "one chip, one array, one latch", not a datasheet quote.
//
// 2. THE EXTENDED-ADDRESS DECODE.  3 high bits from cmd1 + 5 low bits from
//    cmd2 = the Overview's "8 sectors of 32 bytes each".
//
// 3. THE PER-VARIANT LEGACY GROUP MAPPING, which is the only thing rtc.h's
//    `extended` flag still selects.
//
// The chip is driven the way a machine drives it: rtc_input() bit-bangs
// serial data on rising clock edges with CE (disable) low, exactly as the
// VIA port-B glue does.

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The RTC drives VIA1 for its one-second interrupt (CA2) and the transceiver
// handshake.  This suite constructs the chip with no VIA (rtc_set_via is never
// called), so these are never reached with a live object — they exist only to
// satisfy the link.  Keeping them here rather than linking the real via.c
// keeps the suite's surface to the one module under test.
void via_input(struct via *via, int port, int pin, bool value);
void via_input_c(struct via *via, int port, int c, bool value);

void via_input(struct via *via, int port, int pin, bool value) {
    (void)via;
    (void)port;
    (void)pin;
    (void)value;
}

void via_input_c(struct via *via, int port, int c, bool value) {
    (void)via;
    (void)port;
    (void)c;
    (void)value;
}

// Likewise the scheduler: rtc_init arms a one-second tick and rtc_delete
// forgets its source.  This suite drives the serial interface directly and
// never advances time, so the events are inert.  Stubbed here rather than via
// support/stub_system.c, which pulls in the display/framebuffer harness this
// suite has no use for.
event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict scheduler, event_callback_t callback, void *source,
                                    uint64_t data, uint64_t cycles, uint64_t ns, bool periodic) {
    (void)periodic;
    (void)scheduler;
    (void)callback;
    (void)source;
    (void)data;
    (void)cycles;
    (void)ns;
    return NULL;
}

void scheduler_new_event_type(struct scheduler *restrict scheduler, const char *source_name, void *source,
                              const char *event_name, event_callback_t callback) {
    (void)scheduler;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)callback;
}

void scheduler_forget_source(struct scheduler *restrict scheduler, void *source) {
    (void)scheduler;
    (void)source;
}

static int failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// === Serial driver =========================================================
//
// One RTC bit: data is latched on the rising clock edge while CE is asserted.
// rtc_input(rtc, disable, clock, data) — `disable` is CE-not, so false means
// "chip selected".
static void send_bit(rtc_t *rtc, int bit) {
    rtc_input(rtc, false, false, bit != 0); // clock low, data set up
    rtc_input(rtc, false, true, bit != 0); // rising edge latches
}

static void send_byte(rtc_t *rtc, uint8_t v) {
    for (int i = 7; i >= 0; i--)
        send_bit(rtc, (v >> i) & 1);
}

// A transaction begins with the chip deselected so the bit counter reloads,
// then eight bits of command (and, for a write, eight bits of data).
static void begin(rtc_t *rtc) {
    rtc_input(rtc, true, false, false);
    rtc_input(rtc, true, true, false); // rising edge while disabled = reload
    rtc_input(rtc, true, false, false);
}

// Legacy one-byte write command: [cmd][data].
static void legacy_write(rtc_t *rtc, uint8_t cmd, uint8_t data) {
    begin(rtc);
    send_byte(rtc, cmd);
    send_byte(rtc, data);
}

// Extended two-byte write: [cmd1][cmd2][data], cmd1 having the $38 selector.
static void ext_write(rtc_t *rtc, uint8_t addr, uint8_t data) {
    uint8_t cmd1 = (uint8_t)(0x38 | ((addr >> 5) & 0x07));
    uint8_t cmd2 = (uint8_t)((addr & 0x1F) << 2);
    begin(rtc);
    send_byte(rtc, cmd1);
    send_byte(rtc, cmd2);
    send_byte(rtc, data);
}

static void set_protect(rtc_t *rtc, bool on) {
    // $35 is the write-protect register; bit 7 of the data byte is the latch.
    // $35 $55 clears it (writes allowed), $35 $D5 sets it (protected).
    legacy_write(rtc, 0x35, on ? 0xD5 : 0x55);
}

// === Tests =================================================================

// The headline: the same physical byte, reached both ways, obeys the same law.
static void test_protect_covers_both_windows(void) {
    rtc_t *rtc = rtc_init(NULL, NULL, true);
    CHECK(rtc != NULL, "rtc_init returned NULL");
    if (!rtc)
        return;

    // Unprotected: both windows write. $1E is in the legacy group-B range AND
    // reachable by extended address, which is exactly why it is the probe.
    set_protect(rtc, false);
    ext_write(rtc, 0x1E, 0xAA);
    CHECK(rtc_pram_read(rtc, 0x1E) == 0xAA, "extended write while unprotected did not land (got $%02X)",
          rtc_pram_read(rtc, 0x1E));

    // Protected: the extended window must refuse.  This is the regression the
    // unit exists for — it used to write straight through.
    set_protect(rtc, true);
    ext_write(rtc, 0x1E, 0x55);
    CHECK(rtc_pram_read(rtc, 0x1E) == 0xAA, "EXTENDED write was not refused while write-protected (got $%02X)",
          rtc_pram_read(rtc, 0x1E));

    // And the legacy window still refuses, as it always did — so the two
    // windows now agree rather than contradicting each other.
    legacy_write(rtc, 0x41, 0x77); // group B, physical $10 on the extended chip
    CHECK(rtc_pram_read(rtc, 0x10) != 0x77, "legacy write was not refused while write-protected");

    // Releasing protection re-enables both.
    set_protect(rtc, false);
    ext_write(rtc, 0x1E, 0x55);
    CHECK(rtc_pram_read(rtc, 0x1E) == 0x55, "extended write after unprotect did not land (got $%02X)",
          rtc_pram_read(rtc, 0x1E));

    rtc_delete(rtc);
}

// 8 sectors x 32 bytes: walk one address in each sector and confirm it lands
// where the decode says, with no aliasing between sectors.
static void test_extended_address_decode(void) {
    rtc_t *rtc = rtc_init(NULL, NULL, true);
    if (!rtc)
        return;
    set_protect(rtc, false);

    for (int sector = 0; sector < 8; sector++) {
        uint8_t addr = (uint8_t)((sector << 5) | 0x11); // byte 17 of each sector
        ext_write(rtc, addr, (uint8_t)(0xC0 | sector));
    }
    for (int sector = 0; sector < 8; sector++) {
        uint8_t addr = (uint8_t)((sector << 5) | 0x11);
        CHECK(rtc_pram_read(rtc, addr) == (uint8_t)(0xC0 | sector), "sector %d: addr $%02X read back $%02X", sector,
              addr, rtc_pram_read(rtc, addr));
    }

    rtc_delete(rtc);
}

// The legacy group mapping is the one behaviour `extended` still selects:
//   group A (z010aa01, 4 bytes)  -> $08..$0B extended / $10..$13 pre-Plus
//   group B (z1aaaa01, 16 bytes) -> $10..$1F extended / $00..$0F pre-Plus
// The extended layout leaves $0C..$0F free for the XPRAM 'NuMc' signature,
// which is the whole point of the split.
static void test_legacy_group_mapping(void) {
    rtc_t *ext = rtc_init(NULL, NULL, true);
    if (!ext)
        return;
    set_protect(ext, false);
    legacy_write(ext, 0x21, 0x11); // group A, index 0
    legacy_write(ext, 0x41, 0x22); // group B, index 0
    CHECK(rtc_pram_read(ext, 0x08) == 0x11, "extended chip: group A index 0 should be physical $08 (got $%02X at $08)",
          rtc_pram_read(ext, 0x08));
    CHECK(rtc_pram_read(ext, 0x10) == 0x22, "extended chip: group B index 0 should be physical $10 (got $%02X at $10)",
          rtc_pram_read(ext, 0x10));
    // $0C..$0F must be untouched by the legacy window — that is what keeps
    // the 'NuMc' signature from colliding with SysParam bytes.
    for (uint8_t a = 0x0C; a <= 0x0F; a++)
        CHECK(rtc_pram_read(ext, a) == 0x00, "extended chip: legacy write touched $%02X (the 'NuMc' slot)", a);
    rtc_delete(ext);

    rtc_t *legacy = rtc_init(NULL, NULL, false);
    if (!legacy)
        return;
    set_protect(legacy, false);
    legacy_write(legacy, 0x21, 0x33); // group A, index 0
    legacy_write(legacy, 0x41, 0x44); // group B, index 0
    CHECK(rtc_pram_read(legacy, 0x10) == 0x33, "20-byte chip: group A index 0 should be physical $10 (got $%02X)",
          rtc_pram_read(legacy, 0x10));
    CHECK(rtc_pram_read(legacy, 0x00) == 0x44, "20-byte chip: group B index 0 should be physical $00 (got $%02X)",
          rtc_pram_read(legacy, 0x00));
    rtc_delete(legacy);
}

// A fresh chip is waiting for a command byte (unit C1 / N-03).
//
// rtc_init() memset the whole struct, so rx_bits and tx_bits were both zero,
// and the ONLY path that reloads rx_bits from idle is rtc_input's `disable`
// branch — a rising clock edge seen while CE is deasserted.  Every other
// helper here calls begin() first, which is why the rest of the suite never
// noticed.  A guest that asserts CE and starts clocking without that prelude
// hit the bit-count invariant at rtc.c's first assert; with assertions
// compiled out (the GS_FAST / NDEBUG wasm profile) it fell into the transmit
// branch instead, decremented tx_bits to -1 and shifted garbage onto the VIA
// data line forever, since the `!--tx_bits` reload can never be reached from
// a negative count.
//
// So this test drives a legacy write with NO begin(), straight off rtc_init.
// It runs first: reintroducing the defect aborts here, at rtc.c's invariant,
// before the rest of the suite has run.
static void test_fresh_chip_accepts_a_command(void) {
    rtc_t *rtc = rtc_init(NULL, NULL, true);
    CHECK(rtc != NULL, "rtc_init returned NULL");
    if (!rtc)
        return;

    // No begin() anywhere in this function — CE stays asserted throughout.
    send_byte(rtc, 0x35); // write-protect register
    send_byte(rtc, 0x55); // protection off
    send_byte(rtc, 0x21); // legacy group A, index 0
    send_byte(rtc, 0x77);

    CHECK(rtc_pram_read(rtc, 0x08) == 0x77,
          "a fresh chip dropped the first command clocked at it without a CE-deasserted prelude (got $%02X at $08)",
          rtc_pram_read(rtc, 0x08));

    rtc_delete(rtc);
}

// rtc_pram_write() is the shell/host path.  It honours the protect bit as a
// deliberate policy choice (a test that protects PRAM should see writes
// refused), which is separate from the chip law above.
static void test_host_path_honours_protect(void) {
    rtc_t *rtc = rtc_init(NULL, NULL, true);
    if (!rtc)
        return;

    set_protect(rtc, false);
    CHECK(rtc_pram_write(rtc, 0x40, 0x5A), "rtc_pram_write should succeed while unprotected");
    CHECK(rtc_pram_read(rtc, 0x40) == 0x5A, "rtc_pram_write did not land");

    set_protect(rtc, true);
    CHECK(!rtc_pram_write(rtc, 0x40, 0xA5), "rtc_pram_write should report failure while protected");
    CHECK(rtc_pram_read(rtc, 0x40) == 0x5A, "rtc_pram_write wrote through write-protection");

    rtc_delete(rtc);
}

int main(void) {
    test_fresh_chip_accepts_a_command();
    test_protect_covers_both_windows();
    test_extended_address_decode();
    test_legacy_group_mapping();
    test_host_path_honours_protect();

    if (failures) {
        printf("\n%d RTC test(s) failed\n", failures);
        return 1;
    }
    printf("All RTC tests passed\n");
    return 0;
}
