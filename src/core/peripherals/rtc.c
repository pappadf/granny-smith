// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rtc.c
// Real-Time Clock (RTC) and Parameter RAM emulation for Mac Plus.

#include "rtc.h"

#include "log.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_USE_CATEGORY_NAME("rtc");

// Forward declaration — class descriptor is defined at the bottom of the
// file but rtc_init / rtc_delete reference it.
extern const class_desc_t rtc_class;

// === Private Types ===
// RTC state structure (opaque to callers)
struct rtc {
    /* Plain data fields first so we can read/write a single contiguous block
       for checkpointing using offsetof to stop before pointer fields. */
    uint32_t seconds;
    int tx_bits;
    int rx_bits;
    bool clock;
    uint8_t command;
    uint16_t shift;
    unsigned char read_only;
    unsigned char pram[256];

    /* Pointers and non-POD members last */
    via_t *via;
    struct scheduler *scheduler;
    struct object *object; // object-tree node; lifetime tied to this rtc
    struct object *pram_object; // rtc.pram child node (peek/poke/dump/...)
};

extern const class_desc_t rtc_pram_class;

// diff between mac (1904) and unix (1970) epochs
// precalculated using any online epoch converter
#define MAC_TO_UNIX_EPOCH 2082844800

#define IS_READ(cmd)     (cmd & 0x80)
#define IS_EXTENDED(cmd) ((cmd >> 3 & 0x0F) == 7)

#define CMD_SECONDS_REG_0 0x01
#define CMD_SECONDS_REG_1 0x05
#define CMD_SECONDS_REG_2 0x09
#define CMD_SECONDS_REG_3 0x0D
#define CMD_TEST          0x31
#define CMD_WRITE_PROTECT 0x35

void rtc_input(rtc_t *rtc, bool disable, bool clock, bool pram);

static uint8_t read_cmd(rtc_t *rtc, uint8_t cmd) {
    // high bits set equals read operation
    assert(cmd >> 7);

    switch (cmd & 0x7F) {

    case CMD_SECONDS_REG_0:
    case CMD_SECONDS_REG_0 + 16:
        return rtc->seconds & 0xFF;

    case CMD_SECONDS_REG_1:
    case CMD_SECONDS_REG_1 + 16:
        return rtc->seconds >> 8 & 0xFF;

    case CMD_SECONDS_REG_2:
    case CMD_SECONDS_REG_2 + 16:
        return rtc->seconds >> 16 & 0xFF;

    case CMD_SECONDS_REG_3:
    case CMD_SECONDS_REG_3 + 16:
        return rtc->seconds >> 24 & 0xFF;

    case CMD_TEST:
        // Test register is write-only; reading it is unexpected but should return 0
        LOG(2, "Unexpected read from write-only TEST register");
        return 0x00;

    case CMD_WRITE_PROTECT:
        // Write-protect register is write-only
        LOG(2, "Unexpected read from write-only WRITE_PROTECT register");
        return 0x00;

    default:
        // Pattern for addresses 0x10-0x13: z010aa01 where aa are address bits
        // Fixed bits: 6=0, 5=1, 4=0. Bit 3 is an address bit, not checked.
        // Mask: 0x70 (0111 0000) checks only bits 6-5-4
        if ((cmd & 0x70) == 0x20)
            return rtc->pram[0x10 + ((cmd >> 2) & 0x03)];
        // Pattern for addresses 0x00-0x0F: z1aaaa01 where aaaa are address bits
        // Fixed bit: 6=1. Bits 5-2 are address bits, not checked.
        // Mask: 0x40 (0100 0000) checks only bit 6
        else if ((cmd & 0x40) == 0x40)
            return rtc->pram[(cmd >> 2) & 0x0F];
    }

    LOG(1, "Unknown read command: 0x%02X", cmd);
    return 0;
}

static void write_cmd(rtc_t *rtc, uint8_t cmd, uint8_t pram) {
    // High bit clear indicates write operation
    assert(cmd >> 7 == 0);

    // Write-protect command itself is always allowed
    if (cmd != CMD_WRITE_PROTECT) {
        if (rtc->read_only) {
            // Silently ignore write attempts when protected (real hardware behavior)
            LOG(2, "Write attempt while write-protected: cmd=0x%02X data=0x%02X", cmd, pram);
            return;
        }
    }

    switch (cmd) {

    case CMD_SECONDS_REG_0:
    case CMD_SECONDS_REG_0 + 16:
        rtc->seconds = rtc->seconds & 0xFFFFFF00 | pram;
        return;

    case CMD_SECONDS_REG_1:
    case CMD_SECONDS_REG_1 + 16:
        rtc->seconds = rtc->seconds & 0xFFFF00FF | (uint32_t)pram << 8;
        return;

    case CMD_SECONDS_REG_2:
    case CMD_SECONDS_REG_2 + 16:
        rtc->seconds = rtc->seconds & 0xFF00FFFF | (uint32_t)pram << 16;
        return;

    case CMD_SECONDS_REG_3:
    case CMD_SECONDS_REG_3 + 16:
        rtc->seconds = rtc->seconds & 0x00FFFFFF | (uint32_t)pram << 24;
        return;

    case CMD_TEST:
        // Test register is write-only and used to clear test mode during initialization
        LOG(3, "Write to TEST register: value=0x%02X", pram);
        // In real hardware, this clears test mode. In emulation, no action needed.
        return;

    case CMD_WRITE_PROTECT:
        // The data byte determines protection: bit 7 = 1 means protected
        // 0x35 0x55 disables protection (bit 7 = 0)
        // 0x35 0xD5 enables protection (bit 7 = 1)
        rtc->read_only = (pram >> 7) & 1;
        LOG(3, "Write protection %s", rtc->read_only ? "enabled" : "disabled");
        return;

    default:
        // Pattern for addresses 0x10-0x13: z010aa01 where aa are address bits
        // Fixed bits: 6=0, 5=1, 4=0. Bit 3 is an address bit, not checked.
        // Mask: 0x70 (0111 0000) checks only bits 6-5-4
        if ((cmd & 0x70) == 0x20) {
            uint8_t wa = 0x10 + ((cmd >> 2) & 0x03);
            rtc->pram[wa] = pram;
        }
        // Pattern for addresses 0x00-0x0F: z1aaaa01 where aaaa are address bits
        // Fixed bit: 6=1. Bits 5-2 are address bits, not checked.
        // Mask: 0x40 (0100 0000) checks only bit 6
        else if ((cmd & 0x40) == 0x40) {
            uint8_t wa = (cmd >> 2) & 0x0F;
            rtc->pram[wa] = pram;
        } else
            LOG(1, "Unknown write command: 0x%02X data=0x%02X", cmd, pram);
    }

    return;
}

// Extract address from extended command bytes
// Extended commands use bits from both cmd1 and cmd2 to form the full address
static uint8_t read_ext(rtc_t *rtc, uint8_t cmd1, uint8_t cmd2) {
    // Extract address bits: bits [2:0] of cmd1 become addr[7:5], bits [6:2] of cmd2 become addr[4:0]
    uint8_t addr_high = (cmd1 & 0x07) << 5; // bits 7:5 from cmd1
    uint8_t addr_low = (cmd2 >> 2) & 0x1F; // bits 4:0 from cmd2
    uint8_t address = addr_high | addr_low;

    LOG(3, "Extended read: cmd1=0x%02X cmd2=0x%02X addr=0x%02X", cmd1, cmd2, address);
    LOG(1, "Extended PRAM read: addr=0x%02X value=0x%02X", address, rtc->pram[address]);
    return rtc->pram[address];
}

static void write_ext(rtc_t *rtc, uint8_t cmd1, uint8_t cmd2, uint8_t value) {
    // Extract address bits: bits [2:0] of cmd1 become addr[7:5], bits [6:2] of cmd2 become addr[4:0]
    uint8_t addr_high = (cmd1 & 0x07) << 5; // bits 7:5 from cmd1
    uint8_t addr_low = (cmd2 >> 2) & 0x1F; // bits 4:0 from cmd2
    uint8_t address = addr_high | addr_low;

    LOG(3, "Extended write: cmd1=0x%02X cmd2=0x%02X addr=0x%02X value=0x%02X", cmd1, cmd2, address, value);
    rtc->pram[address] = value;
}

void rtc_input(rtc_t *restrict rtc, bool disable, bool clock, bool data) {
    // Only process clock signal changes
    if (clock == rtc->clock)
        return;

    rtc->clock = clock;

    // Only act on rising (positive) edges of the clock
    // Data is latched on the rising edge per [1]: set data, raise clock, lower clock
    if (!clock)
        return;

    if (disable) {
        rtc->command = 0;
        rtc->tx_bits = 0;
        rtc->rx_bits = 8;
        rtc->shift = 0; // Clear shift register for complete state reset
        return;
    }

    // Allow valid state transitions where both may be 0 momentarily, or at least one is active
    assert((rtc->rx_bits >= 0 && rtc->tx_bits >= 0) && (rtc->rx_bits > 0 || rtc->tx_bits > 0));

    if (rtc->rx_bits) {

        rtc->shift = rtc->shift << 1 | data;

        if (!--rtc->rx_bits) {

            if (!rtc->command) {

                // if it's a non-exteded read command - simply return the data
                // Non-extended read command: return data immediately
                if (IS_READ(rtc->shift) && !IS_EXTENDED(rtc->shift)) {
                    rtc->shift = read_cmd(rtc, (uint8_t)rtc->shift);
                    rtc->tx_bits = 8;
                } else { // In all other cases, we need to wait for more input
                    rtc->command = (uint8_t)rtc->shift;
                    if (IS_EXTENDED(rtc->command) && !IS_READ(rtc->command))
                        // Extended write: need cmd2 (8 bits) + data (8 bits) = 16 bits total
                        rtc->rx_bits = 16;
                    else
                        // Extended read or normal write: need one more byte (8 bits)
                        rtc->rx_bits = 8;
                }
            } else if (IS_EXTENDED(rtc->command)) {

                if (IS_READ(rtc->command)) {
                    // Extended read: look up PRAM value and switch to transmit mode
                    rtc->shift = read_ext(rtc, rtc->command, (uint8_t)rtc->shift);
                    rtc->tx_bits = 8;
                } else {
                    // Extended write: we have cmd2 in lower 8 bits, data in upper 8 bits
                    write_ext(rtc, rtc->command, rtc->shift >> 8, rtc->shift & 0xFF);
                    rtc->rx_bits = 8;
                }

                rtc->command = 0;
            } else { // normal (non extended) write command

                assert(!IS_READ(rtc->command));
                write_cmd(rtc, rtc->command, (uint8_t)rtc->shift);
                rtc->command = 0;
                rtc->rx_bits = 8;
            }
        }
    } else {

        via_input(rtc->via, 1, 0, rtc->shift >> 7 & 1);

        if (!--rtc->tx_bits) {
            rtc->command = 0;
            rtc->rx_bits = 8;
        } else
            rtc->shift <<= 1;
    }

    assert((rtc->rx_bits && !rtc->tx_bits) || (!rtc->rx_bits && rtc->tx_bits));
}

static void one_second_interrupt(void *source, uint64_t data) {
    rtc_t *rtc = (rtc_t *)source;

    rtc->seconds++;

    LOG(1, "one_second_interrupt: seconds=%u", rtc->seconds);

    via_input_c(rtc->via, 0, 1, 0);
    via_input_c(rtc->via, 0, 1, 1);

    scheduler_new_cpu_event(rtc->scheduler, &one_second_interrupt, rtc, 0, 0, 1000000000ULL);
}

static uint32_t wall_clock_seconds(void) {
    assert(sizeof(time_t) >= 4);

    return (uint32_t)(time(NULL) + MAC_TO_UNIX_EPOCH);
}

void rtc_set_via(rtc_t *restrict rtc, via_t *via) {
    rtc->via = via;
}

void rtc_set_seconds(rtc_t *restrict rtc, uint32_t mac_seconds) {
    if (!rtc)
        return;
    rtc->seconds = mac_seconds;
    LOG(1, "rtc_set_seconds: seconds=%u", rtc->seconds);
}

// === Validity tokens — see docs/pram.md §2..§3 ==============================
// `_InitUtil` checks two independent tokens at cold boot.  If either is
// missing it re-initialises the relevant region from a hardcoded default
// table, which clobbers any seeded slot-PRAM bytes the test wanted to
// preserve.  `rtc.pram.validate()` writes both tokens so seeded state
// survives `_InitUtil`.
#define RTC_PRAM_VALIDITY_LOW_OFFSET   0x00 // low-PRAM validator byte
#define RTC_PRAM_VALIDITY_LOW_VALUE    0xA8
#define RTC_PRAM_VALIDITY_XPRAM_OFFSET 0x0C // 'NuMc' marker, 4 bytes BE
#define RTC_PRAM_VALIDITY_XPRAM_BYTE_0 0x4E // 'N'
#define RTC_PRAM_VALIDITY_XPRAM_BYTE_1 0x75 // 'u'
#define RTC_PRAM_VALIDITY_XPRAM_BYTE_2 0x4D // 'M'
#define RTC_PRAM_VALIDITY_XPRAM_BYTE_3 0x63 // 'c'

// === M7b — object-model views ===============================================

uint32_t rtc_get_seconds(const rtc_t *rtc) {
    return rtc ? rtc->seconds : 0;
}
bool rtc_get_read_only(const rtc_t *rtc) {
    return rtc ? (rtc->read_only != 0) : false;
}
uint8_t rtc_pram_read(const rtc_t *rtc, uint8_t addr) {
    return rtc ? rtc->pram[addr] : 0;
}
bool rtc_pram_write(rtc_t *rtc, uint8_t addr, uint8_t value) {
    if (!rtc)
        return false;
    if (rtc->read_only)
        return false; // honor the write-protect bit, same as the chip path
    rtc->pram[addr] = value;
    return true;
}

rtc_t *rtc_init(struct scheduler *restrict scheduler, checkpoint_t *checkpoint) {
    rtc_t *rtc = (rtc_t *)malloc(sizeof(rtc_t));
    if (rtc == NULL)
        return NULL;

    memset(rtc, 0, sizeof(rtc_t));

    rtc->scheduler = scheduler;

    rtc->seconds = wall_clock_seconds();

    LOG(1, "rtc_init: seconds=%u", rtc->seconds);

    // Register event type for checkpointing
    scheduler_new_event_type(scheduler, "rtc", rtc, "one_second", &one_second_interrupt);

    // Load from checkpoint if provided. Read a single contiguous block containing
    // the plain-data members of rtc_t (everything before the first pointer).
    if (checkpoint) {
        size_t data_size = offsetof(rtc_t, via);
        system_read_checkpoint_data(checkpoint, rtc, data_size);
        // Do NOT schedule the default one-second event here; it will be restored
        // from the scheduler's checkpointed event queue in scheduler_start().
        LOG(1, "rtc_init: restored from checkpoint");
    } else {
        // Fresh boot: schedule periodic one-second interrupt
        scheduler_new_cpu_event(rtc->scheduler, &one_second_interrupt, rtc, 0, 0, 1000000000ULL);
        LOG(1, "rtc_init: scheduled one-second interrupt");
    }

    // Object-tree binding — instance_data is the rtc itself, so getters
    // can recover it without consulting cfg.  rtc.pram is its own child
    // node so per-byte / range / whole-buffer access lives at one
    // dotted prefix instead of being smeared across `pram`/`pram_read`/
    // `pram_write` on the rtc root.
    rtc->object = object_new(&rtc_class, rtc, "rtc");
    if (rtc->object) {
        object_attach(object_root(), rtc->object);
        rtc->pram_object = object_new(&rtc_pram_class, rtc, "pram");
        if (rtc->pram_object)
            object_attach(rtc->object, rtc->pram_object);
    }

    return rtc;
}

void rtc_delete(rtc_t *rtc) {
    if (!rtc)
        return;
    LOG(1, "rtc_delete: freeing rtc seconds=%u", rtc->seconds);
    if (rtc->pram_object) {
        object_detach(rtc->pram_object);
        object_delete(rtc->pram_object);
        rtc->pram_object = NULL;
    }
    if (rtc->object) {
        object_detach(rtc->object);
        object_delete(rtc->object);
        rtc->object = NULL;
    }
    free(rtc);
}

void rtc_checkpoint(rtc_t *restrict rtc, checkpoint_t *checkpoint) {
    if (!rtc || !checkpoint)
        return;
    // Write the contiguous plain-data portion of rtc_t in a single operation.
    size_t data_size = offsetof(rtc_t, via);
    system_write_checkpoint_data(checkpoint, rtc, data_size);
}

// === Object-model class descriptor =========================================
//
// `rtc.time` is the writable head (Mac-epoch seconds, 1904); it
// replaces `set-time` as the canonical entry point — the legacy
// command remains and `rtc.time = N` is the new equivalent.  PRAM is
// exposed under `rtc.pram` as a child object with peek / poke / dump
// / snapshot / restore / validate methods, mirroring the
// memory.peek / memory.poke shape so anyone who knows that interface
// knows this one.  `poke` takes a V_BYTES second argument (any width):
// per-byte writes use the integer-with-width literal (`0xa8:1`),
// 4-byte tokens like 'NuMc' use `0x4e754d63:4`, and the whole 8-byte
// slot-9 sPRAMRec fits in `0x002780b6b6000000:8`.  All writes honour
// the write-protect bit; bypassing it from the shell would let test
// scripts mask kernel bugs.
//
// instance_data is the rtc_t* itself — the object's lifetime is tied
// to rtc_init / rtc_delete so it is never NULL while the object exists.

static rtc_t *rtc_from(struct object *self) {
    return (rtc_t *)object_data(self);
}

static value_t rtc_attr_time_get(struct object *self, const member_t *m) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    return val_uint(4, rtc ? rtc_get_seconds(rtc) : 0);
}

static value_t rtc_attr_time_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    if (!rtc) {
        value_free(&in);
        return val_err("rtc not available");
    }
    uint32_t mac_seconds = 0;
    bool resolved = false;
    if (in.kind == V_STRING && in.s) {
        // Accept either a decimal unix-epoch string or an ISO-8601
        // "YYYY-MM-DDTHH:MM:SS" timestamp. Either way, the result is
        // unix seconds, then we shift to the Mac 1904 epoch.
        char *endp = NULL;
        long long parsed = strtoll(in.s, &endp, 10);
        if (endp && endp != in.s && *endp == '\0') {
            if (parsed < 0) {
                value_free(&in);
                return val_err("rtc.time: epoch must be non-negative");
            }
            mac_seconds = (uint32_t)((uint64_t)parsed + 2082844800u /* MAC_TO_UNIX_EPOCH */);
            resolved = true;
        } else {
            struct tm tm = {0};
            if (strptime(in.s, "%Y-%m-%dT%H:%M:%S", &tm)) {
                time_t t = timegm(&tm);
                if (t != (time_t)-1) {
                    mac_seconds = (uint32_t)((int64_t)t + 2082844800);
                    resolved = true;
                }
            }
        }
        if (!resolved) {
            value_free(&in);
            return val_err("rtc.time: expected unix-epoch integer or YYYY-MM-DDTHH:MM:SS");
        }
    } else {
        bool ok = true;
        uint64_t s = val_as_u64(&in, &ok);
        if (!ok) {
            value_free(&in);
            return val_err("rtc.time: value is not numeric");
        }
        // Numeric input is treated as Mac-epoch seconds (matches the
        // getter's V_UINT result). Use the string form for unix epochs
        // or ISO timestamps.
        mac_seconds = (uint32_t)s;
    }
    value_free(&in);
    rtc_set_seconds(rtc, mac_seconds);
    return val_none();
}

static value_t rtc_attr_read_only(struct object *self, const member_t *m) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    return val_bool(rtc ? rtc_get_read_only(rtc) : false);
}

static const member_t rtc_members[] = {
    {.kind = M_ATTR,
     .name = "time",
     .doc = "Mac-epoch seconds (1904-based); writable",
     .flags = 0,
     // .type = V_NONE: setter intentionally accepts either V_UINT (Mac
     // seconds) or V_STRING (unix epoch / ISO-8601). Skip framework
     // kind validation; the body discriminates.
     .attr = {.type = V_NONE, .get = rtc_attr_time_get, .set = rtc_attr_time_set}},
    {.kind = M_ATTR,
     .name = "read_only",
     .doc = "Write-protect bit",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = rtc_attr_read_only, .set = NULL}            },
};

const class_desc_t rtc_class = {
    .name = "rtc",
    .members = rtc_members,
    .n_members = sizeof(rtc_members) / sizeof(rtc_members[0]),
};

// === rtc.pram child class ====================================================
//
// Modelled on memory.peek / memory.poke: per-cell read/write through
// `peek`/`poke`, range read through `dump`, whole-buffer transfers
// through `snapshot`/`restore`, and a domain-specific `validate` for
// the boot-ROM cold-init tokens.  `poke` takes a V_BYTES second arg
// (any length, big-endian) so the integer-literal width suffix from
// parse.c (`0xa8:1`, `0x4e754d63:4`, `0x002780b6b6000000:8`) can stand
// in for both per-byte writes and bulk seeds without polymorphism on
// the value's kind.
//
// instance_data is the same rtc_t* the parent uses, set in rtc_init
// when the child is attached.

static value_t rtc_pram_method_peek(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    uint64_t addr = argv[0].u;
    if (addr > 0xFF)
        return val_err("rtc.pram.peek: addr must be 0..255");
    value_t v = val_uint(1, rtc_pram_read(rtc, (uint8_t)addr));
    v.flags |= VAL_HEX;
    return v;
}

static value_t rtc_pram_method_poke(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    uint64_t addr = argv[0].u;
    const value_t *bytes = &argv[1];
    if (bytes->kind != V_BYTES || !bytes->bytes.p)
        return val_err("rtc.pram.poke: bytes argument must be V_BYTES (use the :N width suffix, e.g. 0xa8:1)");
    size_t n = bytes->bytes.n;
    if (n == 0)
        return val_err("rtc.pram.poke: bytes argument is empty");
    if (addr > 0xFF || addr + n > 0x100)
        return val_err("rtc.pram.poke: write of %zu bytes at 0x%02llX would overflow PRAM (256 bytes)", n,
                       (unsigned long long)addr);
    for (size_t i = 0; i < n; i++) {
        if (!rtc_pram_write(rtc, (uint8_t)(addr + i), bytes->bytes.p[i]))
            return val_err("rtc.pram.poke: PRAM is write-protected");
    }
    return val_none();
}

static value_t rtc_pram_method_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    uint64_t addr = argv[0].u;
    uint64_t n = argv[1].u;
    if (addr > 0xFF || n == 0 || addr + n > 0x100)
        return val_err("rtc.pram.dump: read of %llu bytes at 0x%02llX would overflow PRAM (256 bytes)",
                       (unsigned long long)n, (unsigned long long)addr);
    uint8_t buf[256];
    for (size_t i = 0; i < (size_t)n; i++)
        buf[i] = rtc_pram_read(rtc, (uint8_t)(addr + i));
    return val_bytes(buf, (size_t)n);
}

static value_t rtc_pram_method_snapshot(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    uint8_t buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = rtc_pram_read(rtc, (uint8_t)i);
    return val_bytes(buf, sizeof(buf));
}

// Whole-PRAM restore — used by integration tests that want to seed PRAM
// with a previously-snapshotted state (typically dumped via `snapshot`
// after a cold boot, then replayed across `machine.boot` cycles to
// preserve the boot ROM's validity tokens and slot-PRAM init data —
// individually replaying 256 `poke` calls is slow).  Honours the
// write-protect bit the same way per-byte writes do.
static value_t rtc_pram_method_restore(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    const value_t *bytes = &argv[0];
    if (bytes->kind != V_BYTES || bytes->bytes.n != 256 || !bytes->bytes.p)
        return val_err("rtc.pram.restore: expected V_BYTES of length 256 (got len=%zu)",
                       bytes->kind == V_BYTES ? bytes->bytes.n : 0);
    for (int i = 0; i < 256; i++) {
        if (!rtc_pram_write(rtc, (uint8_t)i, bytes->bytes.p[i]))
            return val_err("rtc.pram.restore: PRAM is write-protected");
    }
    return val_none();
}

// Stamp both validity tokens that the boot ROM's `_InitUtil` checks
// at cold-boot (see docs/pram.md §3).  Without these, `_InitUtil` will
// rewrite low-PRAM and/or zero out the XPRAM region — destroying any
// seeded slot-PRAM bytes the test wanted to preserve.
static value_t rtc_pram_method_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    if (!rtc_pram_write(rtc, RTC_PRAM_VALIDITY_LOW_OFFSET, RTC_PRAM_VALIDITY_LOW_VALUE) ||
        !rtc_pram_write(rtc, RTC_PRAM_VALIDITY_XPRAM_OFFSET + 0, RTC_PRAM_VALIDITY_XPRAM_BYTE_0) ||
        !rtc_pram_write(rtc, RTC_PRAM_VALIDITY_XPRAM_OFFSET + 1, RTC_PRAM_VALIDITY_XPRAM_BYTE_1) ||
        !rtc_pram_write(rtc, RTC_PRAM_VALIDITY_XPRAM_OFFSET + 2, RTC_PRAM_VALIDITY_XPRAM_BYTE_2) ||
        !rtc_pram_write(rtc, RTC_PRAM_VALIDITY_XPRAM_OFFSET + 3, RTC_PRAM_VALIDITY_XPRAM_BYTE_3))
        return val_err("rtc.pram.validate: PRAM is write-protected");
    return val_none();
}

static const arg_decl_t rtc_pram_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "PRAM offset (0..255)"},
};
static const arg_decl_t rtc_pram_poke_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "PRAM offset (0..255)"},
    {.name = "bytes", .kind = V_BYTES, .doc = "1..N bytes to write (use the :N integer-width suffix)"},
};
static const arg_decl_t rtc_pram_dump_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "PRAM offset (0..255)"},
    {.name = "n", .kind = V_UINT, .doc = "byte count"},
};
static const arg_decl_t rtc_pram_restore_args[] = {
    {.name = "bytes", .kind = V_BYTES, .doc = "256-byte buffer (typically from rtc.pram.snapshot)"},
};

static const member_t rtc_pram_members[] = {
    {.kind = M_METHOD,
     .name = "peek",
     .doc = "Read one PRAM byte",
     .method = {.args = rtc_pram_peek_args, .nargs = 1, .result = V_UINT, .fn = rtc_pram_method_peek}      },
    {.kind = M_METHOD,
     .name = "poke",
     .doc = "Write 1..N PRAM bytes (honours write-protect)",
     .method = {.args = rtc_pram_poke_args, .nargs = 2, .result = V_NONE, .fn = rtc_pram_method_poke}      },
    {.kind = M_METHOD,
     .name = "dump",
     .doc = "Read N PRAM bytes starting at addr",
     .method = {.args = rtc_pram_dump_args, .nargs = 2, .result = V_BYTES, .fn = rtc_pram_method_dump}     },
    {.kind = M_METHOD,
     .name = "snapshot",
     .doc = "Read all 256 PRAM bytes",
     .method = {.args = NULL, .nargs = 0, .result = V_BYTES, .fn = rtc_pram_method_snapshot}               },
    {.kind = M_METHOD,
     .name = "restore",
     .doc = "Write all 256 PRAM bytes from a snapshot",
     .method = {.args = rtc_pram_restore_args, .nargs = 1, .result = V_NONE, .fn = rtc_pram_method_restore}},
    {.kind = M_METHOD,
     .name = "validate",
     .doc = "Stamp the boot-ROM validity tokens ($00=$A8, $0C-$0F='NuMc')",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = rtc_pram_method_validate}                },
};

const class_desc_t rtc_pram_class = {
    .name = "pram",
    .members = rtc_pram_members,
    .n_members = sizeof(rtc_pram_members) / sizeof(rtc_pram_members[0]),
};
