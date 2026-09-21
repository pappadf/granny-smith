// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cops.c
// Apple Lisa COPS microcontroller. See cops.h and docs/machines/lisa/lisa.md §11.
//
// This first cut models the host↔COPS handshake faithfully (it acts only on
// VIA1 pin traffic — port-A jam, CRDY, CA1, PB0 reset) and emits the power-up
// reset/id codes so the boot ROM's COPS self-test (RSTSCAN) detects a connected
// keyboard and proceeds.  Live keyboard/mouse input injection and the RTC clock
// protocol layer on top of this same handshake in later steps.

#include "cops.h"

#include "checkpoint.h"
#include "log.h"
#include "mouse.h"
#include "scheduler.h"
#include "via.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("cops");

// VIA1 line assignments (docs/machines/lisa/lisa.md §10.1, §11).
#define COPS_CRDY_PIN  6 // VIA1 PB6: COPS-driven ready line
#define COPS_RESET_PIN 0 // VIA1 PB0: host-driven keyboard reset (active low)
#define IFR_CA1_BIT    0x02 // VIA1 IFR bit 1: port-A data-available (CA1)

// Reset/status response codes (docs/machines/lisa/lisa.md §11.2).
#define COPS_RSTCODE    0x80 // reset lead-in byte
#define COPS_KBD_ID     0x3F // final-US keyboard layout id (≤ $DF ⇒ "connected")
#define COPS_PWROFF     0xFB // soft power-off switch pressed (status response)
#define COPS_MOUSE_MARK 0x00 // "mouse data follows" marker (docs §11.4)

// Mouse-button keycode `d000 0110` (docs §11.4): d = 1 pressed, 0 released.
#define COPS_BTN_DOWN 0x86
#define COPS_BTN_UP   0x06

// Accumulated mouse movement clamps to the signed-byte report range
// (lisa.md §11.4).  This was a function-like macro that evaluated its argument
// three times; the shared input_clamp_delta() in mouse.h replaced it.
#define COPS_DELTA_MAX 127

// Mouse report interval: nnn (command low 3 bits) × 4 ms.  At the Lisa's
// 5.09375 MHz CPU, 4 ms ≈ 20375 cycles.
//
// The interval is when we CHECK for accumulated motion, not a heartbeat: a
// report goes out only if there is movement to report.  See cops_mouse_tick,
// which records why (flooding idle reports desynchronises the host's
// multi-byte decoder and made the keyboard unusable at the Xenix boot-loader
// prompt).
//
// This comment used to claim the opposite — that the COPS reports every
// interval even when idle and that those reports are "what keeps boot alive"
// — while the emitter 175 lines below said, at length, that it does not.  The
// emitter is right; ReadCOPS in the boot ROM (RM248.M.TEXT) is an unbounded
// spin on VIA1 IFR with no timeout, and every WT4INPUT caller is a menu state
// machine with nothing to do until input arrives, so nothing needs a
// heartbeat.  Leaving the wrong half here next to the constant was how a
// future editor would have reinstated the Xenix bug from inside this file.
#define COPS_MOUSE_4MS_CYCLES 20375

// Response pacing: re-check host consumption this many CPU cycles apart.  The
// host's GETDATA polls IFR within microseconds, so a tight cadence is safe and
// never overruns an unread byte (the pump waits while IFR CA1 is still set).
#define COPS_PUMP_CYCLES 48

// CRDY (PB6) is the COPS's free-running ready/busy line: the COP421 loops
// through its scan, periodically becoming "ready" (CRDY low) to accept a host
// command and "busy" (CRDY high) otherwise.  Both the boot ROM's COPSCMD and
// MacWorks' send routine synchronise to its edges (wait for a ready state, jam
// the byte while driving port A, wait for the next edge).  We model it as a
// steady toggle; the half-period must be well under the senders' ~10 ms
// per-edge timeout so each wait catches an edge promptly.
#define COPS_CRDY_HALF_CYCLES 1024

#define COPS_FIFO 32 // response queue depth

struct cops {
    // Plain data first so the checkpoint can read/write one contiguous block
    // bounded by offsetof(cops_t, via1) -- the convention the other nine
    // modules in this area follow (STYLE_GUIDE.md, "Device module
    // conventions").  This struct used to lead with its pointers, which is
    // the mechanical reason cops_checkpoint stayed a no-op: the obvious
    // offsetof bound would have written zero bytes.
    bool crdy; // current CRDY (PB6) level we drive: false = ready
    bool reset_asserted; // last observed PB0 reset state (true = held in reset)

    uint8_t fifo[COPS_FIFO]; // pending response bytes
    int head, tail; // ring indices
    bool pump_scheduled;

    // Minimal command state (effects deferred; POST only needs acceptance).
    bool port_on;
    bool mouse_enabled;
    uint64_t mouse_interval; // CPU cycles between reports (0 = disabled)
    bool mouse_scheduled;
    int8_t mouse_dx; // accumulated movement, reset on each report
    int8_t mouse_dy;
    int mouse_carry_x; // motion that did not fit the last clamp, re-added below
    int mouse_carry_y;
    bool mouse_button; // last host-injected button state (for edge detection)

    // Absolute-positioning "warp" (mouse.move x y "global").  The Lisa mouse is
    // relative and the OS scales deltas to pixels, so we can't place the cursor
    // with one delta.  Instead, each mouse report we read the OS's live cursor
    // globals ($CC00F0 = X, $CC00F2 = Y) and emit a corrective delta toward the
    // target — a closed loop that re-reads the cursor each report and corrects, so
    // it converges regardless of the OS's fixed per-axis scale (X×3/2, Y×1).
    bool warp_active;
    int warp_x, warp_y; // target screen pixel
    int warp_ticks; // convergence-loop safety counter

    // Pointers last (not checkpointed)
    via_t *via1;
    struct scheduler *sched;
};

// === Response FIFO ==========================================================

static bool fifo_empty(const cops_t *c) {
    return c->head == c->tail;
}

static int fifo_free(const cops_t *c) {
    int used = (c->tail - c->head + COPS_FIFO) % COPS_FIFO;
    return COPS_FIFO - 1 - used; // one slot always kept to distinguish full/empty
}

static void fifo_push_raw(cops_t *c, uint8_t byte) {
    c->fifo[c->tail] = byte;
    c->tail = (c->tail + 1) % COPS_FIFO;
}

// Enqueue a whole COPS response, or none of it.
//
// The FIFO carries FRAMED messages -- a 3-byte mouse report ($00 dx dy), a
// 2-byte reset/status reply, a 7-byte clock reply -- and the host decodes
// them as multi-byte sequences.  Pushing byte-at-a-time meant a FIFO with
// room for two bytes of a three-byte report stored a partial message and
// desynchronised that decoder.  That is the same failure the emitter below
// records as having made the keyboard unusable at the Xenix boot-loader
// prompt; it was simply reachable a second way.
//
// So the unit of overflow is the MESSAGE.  Note this is deliberately NOT the
// drop-oldest policy adb.c and keyboard.c use: their rings are unframed, one
// entry per key transition, so dropping the oldest byte loses one event and
// nothing else.  Dropping the oldest BYTE here would desynchronise the
// decoder exactly as a partial write does.  (06-io-controllers F-26, N-09.)
static void fifo_push_msg(cops_t *c, const uint8_t *bytes, int n) {
    if (n <= 0)
        return;
    if (fifo_free(c) < n) {
        LOG(1, "cops response FIFO full, dropping a %d-byte message (first byte 0x%02x)", n, bytes[0]);
        return;
    }
    for (int i = 0; i < n; i++)
        fifo_push_raw(c, bytes[i]);
}

// Single-byte messages (a key code, a reset lead-in) are still messages.
static void fifo_push(cops_t *c, uint8_t byte) {
    fifo_push_msg(c, &byte, 1);
}

// Drive CRDY (PB6) — an input pin to the VIA, sourced by the COPS.
static void cops_set_crdy(cops_t *c, bool high) {
    c->crdy = high;
    via_input(c->via1, 1, COPS_CRDY_PIN, high);
}

// Free-running CRDY toggle: models the COPS scan loop cycling between ready
// (low) and busy (high).  Senders poll for these edges before handing over a
// command byte, so the line must keep toggling for the handshake to complete.
static void cops_crdy_tick(void *source, uint64_t data) {
    (void)data;
    cops_t *c = (cops_t *)source;
    cops_set_crdy(c, !c->crdy);
    scheduler_new_cpu_event(c->sched, &cops_crdy_tick, c, 0, COPS_CRDY_HALF_CYCLES, 0);
}

// Present `byte` on port A (input pins) and pulse CA1 to flag data-available.
static void cops_present_byte(cops_t *c, uint8_t byte) {
    for (int pin = 0; pin < 8; pin++)
        via_input(c->via1, 0, pin, (byte >> pin) & 1);
    via_input_c(c->via1, 0, 0, false); // ensure CA1 low …
    via_input_c(c->via1, 0, 0, true); // … then rising edge → IFR CA1
}

// Response pump: deliver the next queued byte once the host has consumed the
// previous one (IFR CA1 clear).  Reschedules itself while bytes remain.
static void cops_pump(void *source, uint64_t data) {
    (void)data;
    cops_t *c = (cops_t *)source;
    c->pump_scheduled = false;
    if (fifo_empty(c))
        return;
    if (via_get_ifr(c->via1) & IFR_CA1_BIT) {
        // Previous byte still unread — try again shortly.
        scheduler_new_cpu_event(c->sched, &cops_pump, c, 0, COPS_PUMP_CYCLES, 0);
        c->pump_scheduled = true;
        return;
    }
    uint8_t byte = c->fifo[c->head];
    c->head = (c->head + 1) % COPS_FIFO;
    cops_present_byte(c, byte);
    LOG(2, "cops delivered 0x%02x", byte);
    if (!fifo_empty(c)) {
        scheduler_new_cpu_event(c->sched, &cops_pump, c, 0, COPS_PUMP_CYCLES, 0);
        c->pump_scheduled = true;
    }
}

static void cops_kick_pump(cops_t *c) {
    if (c->pump_scheduled || fifo_empty(c))
        return;
    scheduler_new_cpu_event(c->sched, &cops_pump, c, 0, COPS_PUMP_CYCLES, 0);
    c->pump_scheduled = true;
}

// === Mouse periodic report =================================================

// Emit one mouse report (marker + accumulated dx/dy) and reschedule.  Runs
// only while the mouse is enabled; the boot ROM's COPS wait depends on it.
static void cops_mouse_tick(void *source, uint64_t data) {
    (void)data;
    cops_t *c = (cops_t *)source;
    c->mouse_scheduled = false;
    if (!c->mouse_enabled || c->mouse_interval == 0)
        return;
    // Closed-loop absolute positioning: steer the OS cursor toward warp_x/warp_y
    // by emitting a small corrective delta each report (small chunks stay under
    // the OS's acceleration threshold, so movement is ~1:1 and the loop converges).
    if (c->warp_active) {
        extern bool lisa_mmu_get_cursor(int ctx, int *x, int *y);
        // Read the live on-screen cursor (OS globals $CC00F0/$CC00F2, supervisor
        // context).  The OS scales COPS mouse deltas into screen pixels by a fixed
        // per-axis factor — X ×3/2 (the 720×364 pixel aspect), Y ×1 — measured
        // exactly and linearly (no acceleration threshold).  So to move the cursor
        // toward the target we inject err/scale mickeys: dx = errX×2/3, dy = errY.
        // The loop re-reads each report, so integer-division residue self-corrects.
        int cx = 0, cy = 0;
        bool got = lisa_mmu_get_cursor(0, &cx, &cy);
        if (got && cx >= -64 && cx <= 1023 && cy >= -64 && cy <= 511) {
            int ex = c->warp_x - cx, ey = c->warp_y - cy;
            const int tol = 3;
            if ((ex >= -tol && ex <= tol && ey >= -tol && ey <= tol) || ++c->warp_ticks > 120) {
                c->warp_active = false; // arrived (sub-pixel) or timed out
            } else {
                int idx = (ex * 2) / 3; // undo the ×3/2 X scaling
                int idy = ey; // Y is 1:1
                const int cap = 120; // stay within the signed-byte report range
                if (idx > cap)
                    idx = cap;
                else if (idx < -cap)
                    idx = -cap;
                if (idy > cap)
                    idy = cap;
                else if (idy < -cap)
                    idy = -cap;
                // Nudge past the dead reckoning when the residual error rounds to a
                // zero inject but is still outside tolerance (small X errors).
                if (idx == 0 && ex > tol)
                    idx = 1;
                else if (idx == 0 && ex < -tol)
                    idx = -1;
                c->mouse_dx = (int8_t)idx;
                c->mouse_dy = (int8_t)idy;
            }
        } else {
            c->warp_active = false; // cursor globals unreadable; give up
        }
    }
    // Emit a report only when there is accumulated motion.  The real COPS detects
    // movement by quadrature pulse edges (Hardware Manual 1983, §8.x "Mouse
    // Movement Waveforms") and reports it; it does NOT stream idle (0,0,0) updates.
    // Flooding idle reports corrupts the host's multi-byte COPS protocol decoder:
    // the 0x00 mouse marker repeatedly re-enters the "expect dx/dy" state, so a
    // keystroke that lands off the 3-byte report boundary is consumed as a
    // coordinate.  This made the keyboard unusable at the Xenix boot-loader prompt.
    // LOS cursor warp/move still works — it sets non-zero deltas while converging.
    if (c->mouse_dx != 0 || c->mouse_dy != 0) {
        const uint8_t report[3] = {COPS_MOUSE_MARK, (uint8_t)c->mouse_dx, (uint8_t)c->mouse_dy};
        fifo_push_msg(c, report, 3);
        // Reported deltas clear, but the CARRY does not: it is motion that has
        // not been reported yet, so it moves into the accumulator and goes out
        // in the next report.  That is what makes a large mouse.move arrive in
        // full rather than being truncated to one report's worth.
        c->mouse_dx = (int8_t)input_clamp_delta(c->mouse_carry_x, -COPS_DELTA_MAX, COPS_DELTA_MAX, &c->mouse_carry_x);
        c->mouse_dy = (int8_t)input_clamp_delta(c->mouse_carry_y, -COPS_DELTA_MAX, COPS_DELTA_MAX, &c->mouse_carry_y);
        cops_kick_pump(c);
    }
    scheduler_new_cpu_event(c->sched, &cops_mouse_tick, c, 0, c->mouse_interval, 0);
    c->mouse_scheduled = true;
}

static void cops_set_mouse(cops_t *c, bool enable, int nnn) {
    c->mouse_enabled = enable;
    c->mouse_interval = enable ? (uint64_t)nnn * COPS_MOUSE_4MS_CYCLES : 0;
    if (enable && c->mouse_interval && !c->mouse_scheduled) {
        scheduler_new_cpu_event(c->sched, &cops_mouse_tick, c, 0, c->mouse_interval, 0);
        c->mouse_scheduled = true;
    }
    if (!enable) {
        remove_event(c->sched, &cops_mouse_tick, c);
        c->mouse_scheduled = false;
    }
}

// === Host input injection ===================================================

void cops_inject_key(cops_t *c, uint8_t code) {
    if (!c)
        return;
    fifo_push(c, code);
    cops_kick_pump(c);
    LOG(2, "cops inject key 0x%02x", code);
}

void cops_soft_power_off(cops_t *c) {
    if (!c)
        return;
    // The soft power-off switch is reported like a reset/status response: the
    // $80 lead-in byte followed by the $FB code (docs/machines/lisa/lisa.md §11.2).
    const uint8_t pwroff[2] = {COPS_RSTCODE, COPS_PWROFF};
    fifo_push_msg(c, pwroff, 2);
    cops_kick_pump(c);
    LOG(1, "cops soft power-off ($80 $FB)");
}

// Begin an absolute "warp" of the OS cursor to screen pixel (x,y).  The mouse
// report tick (cops_mouse_tick) does the convergence by reading $CC00F0/$CC00F2
// and emitting corrective deltas.  Requires the OS to have enabled mouse reports.
void cops_set_warp(cops_t *c, int x, int y) {
    if (!c)
        return;
    c->warp_x = x;
    c->warp_y = y;
    c->warp_active = true;
    c->warp_ticks = 0;
    c->mouse_dx = 0;
    c->mouse_dy = 0;
}

void cops_inject_mouse(cops_t *c, int dx, int dy, int button) {
    if (!c)
        return;
    c->warp_active = false; // an explicit relative move cancels any pending warp
    // Accumulate deltas the way the real COPS sums pulse edges between reports;
    // cops_mouse_tick emits them (guest must have enabled mouse interrupts).
    // Clamp with CARRY, not destructively.  The leftover is re-added to the
    // accumulator so the next report continues the motion, the way a real
    // counter would -- adb.c has always done this and cops.c discarded the
    // overflow, so a large synthetic mouse.move silently lost distance here.
    // Unreachable from a human hand (a real mouse never produces >127 counts
    // in one 4 ms interval), which is exactly why only injection saw it.
    // The carry from last time is part of this injection's motion.
    int total_x = (int)c->mouse_dx + dx + c->mouse_carry_x;
    int total_y = (int)c->mouse_dy + dy + c->mouse_carry_y;
    c->mouse_dx = (int8_t)input_clamp_delta(total_x, -COPS_DELTA_MAX, COPS_DELTA_MAX, &c->mouse_carry_x);
    c->mouse_dy = (int8_t)input_clamp_delta(total_y, -COPS_DELTA_MAX, COPS_DELTA_MAX, &c->mouse_carry_y);
    if (button >= 0) {
        bool down = button != 0;
        if (down != c->mouse_button) {
            c->mouse_button = down;
            cops_inject_key(c, down ? COPS_BTN_DOWN : COPS_BTN_UP);
        }
    }
    LOG(2, "cops inject mouse dx=%d dy=%d button=%d", dx, dy, button);
}

// === Command handling =======================================================

// Process a command byte jammed by the host (docs/machines/lisa/lisa.md §11.1).  POST only
// requires that we accept these; the effects matter once input is injected.
static void cops_command(cops_t *c, uint8_t cmd) {
    if (cmd == 0x00) {
        c->port_on = true; // turn I/O port on
    } else if (cmd == 0x01) {
        c->port_on = false; // turn I/O port off
    } else if ((cmd & 0xF0) == 0x70) {
        // #111 ennn: e (bit 3) = mouse-interrupt enable, nnn = interval units.
        cops_set_mouse(c, (cmd & 0x08) != 0, cmd & 0x07);
    } else if (cmd == 0x02) {
        // Read clock: the COPS replies with a $80 lead-in, an $Ey clock-data
        // marker (y = year nibble), then 5 packed time bytes (docs §11.5).  A
        // zeroed time is a valid default; the deterministic/real clock layers
        // in with the RTC work (Step 9).
        // The clock reply is one 7-byte message: $80, the $E0 clock-data
        // marker, then five time bytes.  READCLK in the boot ROM
        // (RM248.M.TEXT) reads exactly that shape, so a partial enqueue would
        // leave it waiting mid-sequence.  The five bytes are zero because the
        // COPS clock is not modelled -- see 06-io-controllers F-27, which is
        // deferred pending a decision on the Ey,dd,dh,hm,ms,st format.
        const uint8_t clock_reply[7] = {COPS_RSTCODE, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};
        fifo_push_msg(c, clock_reply, 7);
        cops_kick_pump(c);
    }
    // 0x1n write-clock, 0x2x set-modes, 0x5n/0x6n NMI-key: accepted.
    LOG(2, "cops command 0x%02x", cmd);
}

void cops_via_output(cops_t *c, uint8_t port, uint8_t value) {
    if (!c)
        return;
    if (port == 0) {
        // Port A: the host jams a command only while driving the whole byte
        // (DDRA = $FF).  CRDY toggles independently (cops_crdy_tick); we just
        // latch the command on the jam.  Writing DDRA fires this callback, so
        // the command byte (already in ORA) is read at the moment it is driven.
        uint8_t ddra = via_port_direction(c->via1, 0);
        if (ddra == 0xFF) {
            uint8_t cmd = via_port_output(c->via1, 0); // ORA = the command byte
            cops_command(c, cmd);
        }
    } else {
        // Port B: track PB0 reset line.  The low→high edge (CLRRST) makes the
        // keyboard COPS emit its reset/id codes.
        bool reset_now = (value & (1u << COPS_RESET_PIN)) == 0; // active low
        if (c->reset_asserted && !reset_now) {
            // Reset released → report a connected keyboard ($80, id).  No mouse
            // codes are sent, which RSTSCAN reads as "mouse connected".
            const uint8_t reset_id[2] = {COPS_RSTCODE, COPS_KBD_ID};
            fifo_push_msg(c, reset_id, 2);
            cops_kick_pump(c);
            LOG(1, "cops reset released → keyboard id 0x%02x", COPS_KBD_ID);
        }
        c->reset_asserted = reset_now;
    }
}

// === Lifecycle =============================================================

// Mirror of cops_checkpoint; defined below, used while constructing.
static void cops_restore(cops_t *c, checkpoint_t *cp);

cops_t *cops_init(via_t *via1, struct scheduler *scheduler, checkpoint_t *cp) {
    cops_t *c = (cops_t *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->via1 = via1;
    c->sched = scheduler;
    scheduler_new_event_type(scheduler, "cops", c, "pump", &cops_pump);
    scheduler_new_event_type(scheduler, "cops", c, "mouse", &cops_mouse_tick);
    scheduler_new_event_type(scheduler, "cops", c, "crdy", &cops_crdy_tick);
    if (cp) {
        // Restore the plain-data block.  Do NOT arm any events here: the
        // scheduler's own checkpointed queue brings back this source's crdy,
        // pump and mouse events in scheduler_start(), matching the
        // pump_scheduled / mouse_scheduled flags we just read.
        //
        // Arming unconditionally (as this did before) meant a restored Lisa
        // ran TWO free-running CRDY togglers: the one armed here and the one
        // the saved queue brought back.  rtc_init has the correct shape and
        // is the pattern followed here.
        cops_restore(c, cp);
        via_input(c->via1, 1, 6, c->crdy); // re-drive the restored level
    } else {
        // Cold boot: start the free-running CRDY (PB6) toggle from ready (low).
        cops_set_crdy(c, false);
        scheduler_new_cpu_event(scheduler, &cops_crdy_tick, c, 0, COPS_CRDY_HALF_CYCLES, 0);
    }
    return c;
}

void cops_delete(cops_t *c) {
    if (!c)
        return;
    // Seven scheduling sites, three callbacks removed here.
    scheduler_forget_source(c->sched, c);
    free(c);
}

// Save the COPS's plain-data region: the response FIFO and its indices, the
// CRDY phase, the command/mouse state and the warp target.  None of it is
// re-derived by the reset handshake -- a restored Lisa without this comes up
// with an empty FIFO, the mouse disabled and any in-flight warp forgotten.
//
// The stream is positional and unversioned (build-ID gated), so this and
// cops_restore must change together, in one commit.
void cops_checkpoint(cops_t *c, checkpoint_t *cp) {
    if (!c || !cp)
        return;
    system_write_checkpoint_data(cp, c, offsetof(cops_t, via1));
}

// The mirror, called from cops_init while constructing.
static void cops_restore(cops_t *c, checkpoint_t *cp) {
    if (!c || !cp)
        return;
    system_read_checkpoint_data(cp, c, offsetof(cops_t, via1));
}
