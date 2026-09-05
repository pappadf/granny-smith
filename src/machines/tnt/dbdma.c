// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dbdma.c
// The DBDMA engine (see dbdma.h for the source-of-truth discussion).
//
// Programming model implemented here, in one paragraph: a channel is
// started by writing the physical address of a 16-byte-aligned command
// list to commandPtrLo and setting RUN through the control register's
// mask/value convention; the engine then fetches 16-byte little-endian
// descriptors from guest memory and executes them — data moves between
// descriptor-addressed memory and the attached device port, quad
// stores/loads touch memory directly, NOP carries the branch/interrupt
// modifiers, STOP parks the channel on the STOP descriptor itself (so a
// driver can overwrite it and WAKE the channel — the audio-ring idiom).
// Status changes are synchronous with the control write: the canonical
// stop (`clear RUN|FLUSH, poll ACTIVE|FLUSH`) and reset (`clear
// everything, poll RUN`) loops terminate on the guest's next read.
//
// Descriptor field layout (little-endian; Linux `struct dbdma_cmd`):
//   +$0  reqCount (15:0), w (17:16), b (19:18), i (21:20), key (26:24),
//        cmd (31:28)
//   +$4  physical data address
//   +$8  cmdDep — branch target (data commands, NOP) or quad data
//   +$C  resCount (15:0), xferStatus (31:16)
// Commands: 0/1 OUTPUT_MORE/LAST, 2/3 INPUT_MORE/LAST, 4 STORE_QUAD,
// 5 LOAD_QUAD, 6 NOP, 7 STOP.  The i/b/w two-bit modifiers are
// NEVER/IFSET/IFCLR/ALWAYS (0/1/2/3) against the channel's condition-
// select registers: cond = ((devstat & sel.mask) == (sel.value &
// sel.mask)) with sel.mask in bits 23:16 and sel.value in bits 7:0.
// Whether shipping Grand Central implements the three select registers
// is unattested (no driver in the corpus programs them); this model
// keeps them live — power-on zero makes every condition read true, which
// is what INTR_ALWAYS/BR_ALWAYS-only drivers observe either way.
//
// Drivers commit a descriptor by writing its operation word LAST
// (Apple's MakeCCDescriptor rule); the engine honors that by never
// caching descriptors — every (re)activation refetches from cmdptr, and
// only the in-flight byte cursor survives a stall.

#include "dbdma.h"
#include "common.h"

#include "log.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("dbdma");

// Command nibble values (descriptor word 0, bits 31:28).
#define CMD_OUTPUT_MORE 0
#define CMD_OUTPUT_LAST 1
#define CMD_INPUT_MORE  2
#define CMD_INPUT_LAST  3
#define CMD_STORE_QUAD  4
#define CMD_LOAD_QUAD   5
#define CMD_NOP         6
#define CMD_STOP        7

// i/b/w modifier values (NEVER/IFSET/IFCLR/ALWAYS).
#define COND_NEVER  0
#define COND_IFSET  1
#define COND_IFCLR  2
#define COND_ALWAYS 3

// Host-settable status bits (mask writes may CLEAR any bit, but can only
// SET these — ACTIVE/DEAD/BT are engine-owned outputs).
#define HOST_SET_BITS (TNT_DBDMA_RUN | TNT_DBDMA_PAUSE | TNT_DBDMA_FLUSH | TNT_DBDMA_WAKE | TNT_DBDMA_DEVSTAT)

// Commands executed per activation before the runaway guard trips (a
// descriptor ring with no data command and no WAIT would otherwise spin
// the host forever; no sane driver builds one).
#define RUN_BUDGET 4096

// Bytes moved per device-port call (bounds stack use; a data command
// loops until done or the port stalls).
#define PORT_CHUNK 512

// Per-channel software-visible state (plain data; checkpointed whole).
typedef struct dbdma_chan {
    uint32_t status; // RUN..BT + host-latched s-bits (live device s-bits OR in on read)
    uint32_t cmdptr; // physical address of the next (or in-flight) descriptor
    uint32_t intr_sel; // interrupt condition select (mask 23:16, value 7:0)
    uint32_t br_sel; // branch condition select
    uint32_t wait_sel; // wait condition select
    uint32_t cursor; // bytes already moved by the in-flight data command
} dbdma_chan_t;

struct tnt_dbdma {
    dbdma_chan_t chan[TNT_DBDMA_CHANNELS];
    tnt_dbdma_port_t port[TNT_DBDMA_CHANNELS]; // device ports (out==in==NULL when absent)
    tnt_dbdma_mem_read_fn mem_read;
    tnt_dbdma_mem_write_fn mem_write;
    void *mem_ctx;
    tnt_dbdma_irq_fn irq;
    void *irq_ctx;
};

// ============================================================
// Lifecycle
// ============================================================

tnt_dbdma_t *tnt_dbdma_init(checkpoint_t *cp) {
    tnt_dbdma_t *d = calloc(1, sizeof(*d));
    if (!d) {
        LOG(0, "Error: out of memory allocating the DBDMA engine");
        return NULL;
    }
    if (cp)
        for (int n = 0; n < TNT_DBDMA_CHANNELS; n++)
            system_read_checkpoint_data(cp, &d->chan[n], sizeof(d->chan[n]));
    return d;
}

void tnt_dbdma_delete(tnt_dbdma_t *d) {
    free(d);
}

void tnt_dbdma_checkpoint(tnt_dbdma_t *d, checkpoint_t *cp) {
    for (int n = 0; n < TNT_DBDMA_CHANNELS; n++)
        system_write_checkpoint_data(cp, &d->chan[n], sizeof(d->chan[n]));
}

void tnt_dbdma_reset(tnt_dbdma_t *d) {
    // Power-on: every channel idle, all registers zero (ports and hooks
    // are wiring, not state — they survive).
    memset(d->chan, 0, sizeof(d->chan));
}

void tnt_dbdma_set_memory_hooks(tnt_dbdma_t *d, tnt_dbdma_mem_read_fn rd, tnt_dbdma_mem_write_fn wr, void *ctx) {
    d->mem_read = rd;
    d->mem_write = wr;
    d->mem_ctx = ctx;
}

void tnt_dbdma_set_irq_hook(tnt_dbdma_t *d, tnt_dbdma_irq_fn fn, void *ctx) {
    d->irq = fn;
    d->irq_ctx = ctx;
}

void tnt_dbdma_set_port(tnt_dbdma_t *d, int chan, const tnt_dbdma_port_t *port) {
    assert(chan >= 0 && chan < TNT_DBDMA_CHANNELS);
    if (port)
        d->port[chan] = *port;
    else
        memset(&d->port[chan], 0, sizeof(d->port[chan]));
}

// ============================================================
// Descriptor + status helpers
// ============================================================

// Compose a little-endian 32-bit field from raw guest bytes.
// Read the descriptor at `addr` into its four little-endian words.
static void desc_fetch(tnt_dbdma_t *d, uint32_t addr, uint32_t w[4]) {
    uint8_t raw[16];
    d->mem_read(d->mem_ctx, addr, raw, 16);
    for (int i = 0; i < 4; i++)
        w[i] = RD_LE32(raw + 4 * i);
}

// Write one little-endian 32-bit descriptor field back to guest memory.
static void desc_store32(tnt_dbdma_t *d, uint32_t addr, uint32_t value) {
    uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    d->mem_write(d->mem_ctx, addr, raw, 4);
}

// The channel's live device-status byte: host-latched s-bits OR the
// device's live view.
static uint8_t devstat(tnt_dbdma_t *d, int n) {
    uint8_t s = (uint8_t)(d->chan[n].status & TNT_DBDMA_DEVSTAT);
    if (d->port[n].s_bits)
        s |= d->port[n].s_bits(d->port[n].ctx);
    return s;
}

// Evaluate one i/b/w two-bit modifier against a condition-select
// register (see the header comment for the mask/value convention).
static bool cond_eval(uint32_t modifier, uint32_t sel, uint8_t stat) {
    uint8_t mask = (uint8_t)(sel >> 16);
    uint8_t value = (uint8_t)sel;
    bool cond = (stat & mask) == (value & mask);
    switch (modifier) {
    case COND_IFSET:
        return cond;
    case COND_IFCLR:
        return !cond;
    case COND_ALWAYS:
        return true;
    default:
        return false; // COND_NEVER
    }
}

// The 16-bit status image written into xferStatus: hardware bits plus
// the live device byte (drivers test ACTIVE and BT here).
static uint16_t status16(tnt_dbdma_t *d, int n) {
    return (uint16_t)((d->chan[n].status & ~(uint32_t)TNT_DBDMA_DEVSTAT) | devstat(d, n));
}

// Write the in-flight descriptor's result field (xferStatus + resCount).
// ALWAYS called before any interrupt for the same command — drivers read
// residuals from here (the resCount-before-interrupt gotcha).
static void result_writeback(tnt_dbdma_t *d, int n, uint32_t desc_addr, uint32_t req) {
    uint32_t res = req - d->chan[n].cursor;
    desc_store32(d, desc_addr + 12, ((uint32_t)status16(d, n) << 16) | (res & 0xFFFFu));
}

// A channel executes commands only in this state.
static bool runnable(const dbdma_chan_t *c) {
    uint32_t need = TNT_DBDMA_RUN | TNT_DBDMA_ACTIVE;
    uint32_t veto = TNT_DBDMA_PAUSE | TNT_DBDMA_DEAD;
    return (c->status & (need | veto)) == need;
}

// ============================================================
// Program execution
// ============================================================

// Run channel n until it stops, stalls on the device or a WAIT, pauses,
// or exhausts the runaway budget.  Synchronous by design: control-write
// status transitions and completion interrupts all happen before the
// guest's next instruction.
static void run_channel(tnt_dbdma_t *d, int n) {
    dbdma_chan_t *c = &d->chan[n];
    if (!d->mem_read || !d->mem_write) {
        LOG(1, "ch%d activated with no memory hooks", n);
        return;
    }
    int budget = RUN_BUDGET;
    while (runnable(c)) {
        if (budget-- == 0) {
            LOG(1, "ch%d runaway program (no data command or WAIT in %d commands) — parking until next kick", n,
                RUN_BUDGET);
            return;
        }
        uint32_t w[4];
        desc_fetch(d, c->cmdptr, w);
        uint32_t cmd = (w[0] >> 28) & 0xFu;
        uint32_t i_mod = (w[0] >> 20) & 3u;
        uint32_t b_mod = (w[0] >> 18) & 3u;
        uint32_t w_mod = (w[0] >> 16) & 3u;
        uint32_t req = w[0] & 0xFFFFu;
        uint8_t stat = devstat(d, n);

        // WAIT is a modifier, evaluated before the command runs; a
        // waiting channel stays ACTIVE and re-evaluates on the next kick
        // (a device s-bit change) or control write.
        if (cond_eval(w_mod, c->wait_sel, stat)) {
            LOG(3, "ch%d waiting at $%08X (w=%u devstat=$%02X)", n, c->cmdptr, w_mod, stat);
            return;
        }

        bool advance = true; // STOP parks on its own descriptor
        bool taken = false; // branch outcome -> BT + next cmdptr
        bool wrote_result = false; // data commands write result before any irq

        switch (cmd) {
        case CMD_OUTPUT_MORE:
        case CMD_OUTPUT_LAST:
        case CMD_INPUT_MORE:
        case CMD_INPUT_LAST: {
            bool out = cmd <= CMD_OUTPUT_LAST;
            const tnt_dbdma_port_t *p = &d->port[n];
            if (!(out ? p->out != NULL : p->in != NULL)) {
                // No device behind this channel yet: stall honestly (the
                // program resumes when a later phase attaches the port).
                LOG(1, "ch%d %s $%04X bytes with no device port — stalling", n, out ? "OUTPUT" : "INPUT", req);
                return;
            }
            while (c->cursor < req) {
                uint8_t buf[PORT_CHUNK];
                int want = (int)(req - c->cursor);
                if (want > PORT_CHUNK)
                    want = PORT_CHUNK;
                int moved;
                if (out) {
                    d->mem_read(d->mem_ctx, w[1] + c->cursor, buf, (uint32_t)want);
                    moved = p->out(p->ctx, buf, want);
                } else {
                    moved = p->in(p->ctx, buf, want);
                    if (moved > 0)
                        d->mem_write(d->mem_ctx, w[1] + c->cursor, buf, (uint32_t)moved);
                }
                if (moved < 0)
                    moved = 0;
                c->cursor += (uint32_t)moved;
                if (moved < want) {
                    // Device stalled mid-command: cursor survives, the
                    // descriptor is refetched on the device's kick.
                    LOG(3, "ch%d stalled at %u/%u bytes", n, c->cursor, req);
                    return;
                }
            }
            // Command complete: branch decision, then result write-back,
            // then (below) the interrupt.
            taken = cond_eval(b_mod, c->br_sel, devstat(d, n));
            if (taken)
                c->status |= TNT_DBDMA_BT;
            else
                c->status &= ~(uint32_t)TNT_DBDMA_BT;
            result_writeback(d, n, c->cmdptr, req);
            wrote_result = true;
            break;
        }
        case CMD_STORE_QUAD:
        case CMD_LOAD_QUAD: {
            // Quadlet access to guest space; reqCount selects 1/2/4 bytes
            // (low bytes of cmdDep), per the published architecture.
            uint32_t len = (req == 1 || req == 2) ? req : 4;
            if (req != len)
                LOG(2, "ch%d quad with reqCount $%04X treated as 4 bytes", n, req);
            if (cmd == CMD_STORE_QUAD) {
                uint8_t raw[4] = {(uint8_t)w[2], (uint8_t)(w[2] >> 8), (uint8_t)(w[2] >> 16), (uint8_t)(w[2] >> 24)};
                d->mem_write(d->mem_ctx, w[1], raw, len);
            } else {
                uint8_t raw[4] = {0, 0, 0, 0};
                d->mem_read(d->mem_ctx, w[1], raw, len);
                desc_store32(d, c->cmdptr + 8, RD_LE32(raw)); // into cmdDep
            }
            // cmdDep carries the quad, so these commands have no branch
            // target; a set b-modifier is a driver bug — log, don't jump.
            if (cond_eval(b_mod, c->br_sel, devstat(d, n)))
                LOG(2, "ch%d branch modifier on a quad command ignored", n);
            break;
        }
        case CMD_NOP:
            // NOP carries the modifiers: branch (target in cmdDep) and
            // interrupt.  BR_ALWAYS+NOP is the ring-buffer jump idiom.
            taken = cond_eval(b_mod, c->br_sel, stat);
            if (taken)
                c->status |= TNT_DBDMA_BT;
            else
                c->status &= ~(uint32_t)TNT_DBDMA_BT;
            break;
        case CMD_STOP:
            // Park ON the STOP descriptor (cmdptr does not advance): the
            // driver overwrites it with a live command and sets WAKE to
            // continue — the audio-ring idiom the commit rule exists for.
            c->status &= ~(uint32_t)TNT_DBDMA_ACTIVE;
            c->cursor = 0;
            advance = false;
            LOG(3, "ch%d STOP at $%08X", n, c->cmdptr);
            break;
        default:
            // Reserved encodings 8-15: no attested semantics; a real
            // program never contains them, so treat as a dead program.
            LOG(1, "ch%d reserved command %u at $%08X — channel dead", n, cmd, c->cmdptr);
            c->status |= TNT_DBDMA_DEAD;
            c->status &= ~(uint32_t)TNT_DBDMA_ACTIVE;
            advance = false;
            break;
        }

        uint32_t desc_addr = c->cmdptr;
        if (advance) {
            c->cmdptr = taken ? w[2] : c->cmdptr + 16;
            c->cursor = 0;
        }
        // Interrupt AFTER write-back (and after cmdptr advance, so a
        // handler reading commandPtrLo sees the post-command position).
        if (cond_eval(i_mod, c->intr_sel, devstat(d, n))) {
            if (!wrote_result && cmd != CMD_STOP) {
                // NOP/quad interrupts still stamp the result field so a
                // completion handler finds a fresh status there (cursor
                // is 0 here, so resCount reads 0).
                result_writeback(d, n, desc_addr, 0);
            }
            LOG(3, "ch%d interrupt at $%08X", n, desc_addr);
            if (d->irq)
                d->irq(d->irq_ctx, n);
        }
    }
}

// ============================================================
// Register file
// ============================================================

// Flush/stop rundown: a partial data command's residual becomes guest-
// visible (MESH/53C94 short transfers read resCount after stopping the
// channel).  Needs a refetch for reqCount — descriptors are never cached.
static void partial_writeback(tnt_dbdma_t *d, int n) {
    dbdma_chan_t *c = &d->chan[n];
    if (c->cursor == 0 || !d->mem_read || !d->mem_write)
        return;
    uint32_t w[4];
    desc_fetch(d, c->cmdptr, w);
    result_writeback(d, n, c->cmdptr, w[0] & 0xFFFFu);
}

uint32_t tnt_dbdma_reg_read(tnt_dbdma_t *d, int chan, uint32_t offset) {
    assert(chan >= 0 && chan < TNT_DBDMA_CHANNELS);
    dbdma_chan_t *c = &d->chan[chan];
    // The register-level trace (level 4): every read, with the status it
    // answers from — the instrument for a driver that polls something the
    // model never changes.
    LOG(4, "ch%d rd +$%02X (status $%04X cmdptr $%08X)", chan, offset & 0xFCu, status16(d, chan), c->cmdptr);
    switch (offset & 0xFCu) {
    case TNT_DBDMA_REG_CONTROL:
        return 0; // write-only in effect
    case TNT_DBDMA_REG_STATUS:
        return (uint32_t)status16(d, chan);
    case TNT_DBDMA_REG_CMDPTRLO:
        return c->cmdptr; // advances as the program runs
    case TNT_DBDMA_REG_INTRSEL:
        return c->intr_sel;
    case TNT_DBDMA_REG_BRSEL:
        return c->br_sel;
    case TNT_DBDMA_REG_WAITSEL:
        return c->wait_sel;
    default:
        // commandPtrHi and the rest of the optional set: implemented as
        // read-zero on Grand Central.
        LOG(3, "ch%d read of unimplemented reg +$%02X", chan, offset);
        return 0;
    }
}

void tnt_dbdma_reg_write(tnt_dbdma_t *d, int chan, uint32_t offset, uint32_t value) {
    assert(chan >= 0 && chan < TNT_DBDMA_CHANNELS);
    dbdma_chan_t *c = &d->chan[chan];
    LOG(4, "ch%d wr +$%02X = $%08X (status $%04X)", chan, offset & 0xFCu, value, status16(d, chan));
    switch (offset & 0xFCu) {
    case TNT_DBDMA_REG_CONTROL: {
        // Mask/value convention: only bits set in the upper half change,
        // taking the value of the corresponding lower-half bit.  Any bit
        // may be CLEARED this way (the canonical reset clears ACTIVE and
        // DEAD too); only host-owned bits may be SET.
        uint32_t mask = value >> 16;
        uint32_t set = mask & value & HOST_SET_BITS;
        uint32_t clr = mask & ~value;
        uint32_t was = c->status;
        c->status = (c->status & ~clr) | set;
        LOG(3, "ch%d control $%08X: status $%04X -> $%04X", chan, value, was, c->status);

        // RUN transitions own ACTIVE: setting RUN arms the program at
        // cmdptr; clearing RUN halts it (residual made visible first) —
        // both synchronously, so the canonical poll loops terminate.
        if ((c->status & TNT_DBDMA_RUN) && !(was & TNT_DBDMA_RUN)) {
            c->status &= ~(uint32_t)TNT_DBDMA_DEAD;
            c->status |= TNT_DBDMA_ACTIVE;
            c->cursor = 0;
        } else if (!(c->status & TNT_DBDMA_RUN) && (was & TNT_DBDMA_RUN)) {
            partial_writeback(d, chan);
            c->status &= ~(TNT_DBDMA_ACTIVE | TNT_DBDMA_DEAD);
            c->cursor = 0;
        }
        // WAKE restarts a channel parked by STOP (self-clearing): the
        // overwritten STOP descriptor is refetched from cmdptr.
        if (c->status & TNT_DBDMA_WAKE) {
            c->status &= ~(uint32_t)TNT_DBDMA_WAKE;
            if (c->status & TNT_DBDMA_RUN)
                c->status |= TNT_DBDMA_ACTIVE;
        }
        // FLUSH publishes an in-flight command's residual and self-clears
        // — there is no buffered data in this model, so it is complete
        // synchronously.
        if (c->status & TNT_DBDMA_FLUSH) {
            partial_writeback(d, chan);
            c->status &= ~(uint32_t)TNT_DBDMA_FLUSH;
        }
        run_channel(d, chan);
        break;
    }
    case TNT_DBDMA_REG_CMDPTRLO:
        // Loadable only while the channel is disarmed (drivers write it
        // before setting RUN; hardware ignores it mid-program).  The
        // shipping ROM's native sound driver does write it once on a
        // channel still parked on Open Firmware's beep STOP (RUN=1,
        // ACTIVE=0) at its init; nothing ever starts that program — the
        // Sound Manager's ring is loaded later with RUN cleared first —
        // so the parked case stays ignored, as the T9 ladder rung pins.
        if (c->status & (TNT_DBDMA_RUN | TNT_DBDMA_ACTIVE)) {
            LOG(1, "ch%d cmdptr write $%08X ignored while running", chan, value);
            break;
        }
        c->cmdptr = value;
        c->cursor = 0;
        break;
    case TNT_DBDMA_REG_INTRSEL:
        c->intr_sel = value & 0x00FF00FFu;
        break;
    case TNT_DBDMA_REG_BRSEL:
        c->br_sel = value & 0x00FF00FFu;
        break;
    case TNT_DBDMA_REG_WAITSEL:
        c->wait_sel = value & 0x00FF00FFu;
        break;
    default:
        LOG(2, "ch%d write of unimplemented reg +$%02X = $%08X", chan, offset, value);
        break;
    }
}

// ============================================================
// Device-side pacing
// ============================================================

void tnt_dbdma_kick(tnt_dbdma_t *d, int chan) {
    assert(chan >= 0 && chan < TNT_DBDMA_CHANNELS);
    run_channel(d, chan);
}

bool tnt_dbdma_active(tnt_dbdma_t *d, int chan) {
    assert(chan >= 0 && chan < TNT_DBDMA_CHANNELS);
    return runnable(&d->chan[chan]);
}
