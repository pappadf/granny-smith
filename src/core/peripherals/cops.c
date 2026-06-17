// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cops.c
// Apple Lisa COPS microcontroller. See cops.h and docs/lisa.md §11.
//
// This first cut models the host↔COPS handshake faithfully (it acts only on
// VIA1 pin traffic — port-A jam, CRDY, CA1, PB0 reset) and emits the power-up
// reset/id codes so the boot ROM's COPS self-test (RSTSCAN) detects a connected
// keyboard and proceeds.  Live keyboard/mouse input injection and the RTC clock
// protocol layer on top of this same handshake in later steps.

#include "cops.h"

#include "log.h"
#include "scheduler.h"
#include "via.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("cops");

// VIA1 line assignments (docs/lisa.md §10.1, §11).
#define COPS_CRDY_PIN  6 // VIA1 PB6: COPS-driven ready line
#define COPS_RESET_PIN 0 // VIA1 PB0: host-driven keyboard reset (active low)
#define IFR_CA1_BIT    0x02 // VIA1 IFR bit 1: port-A data-available (CA1)

// Reset/status response codes (docs/lisa.md §11.2).
#define COPS_RSTCODE    0x80 // reset lead-in byte
#define COPS_KBD_ID     0x3F // final-US keyboard layout id (≤ $DF ⇒ "connected")
#define COPS_MOUSE_MARK 0x00 // "mouse data follows" marker (docs §11.4)

// Mouse-button keycode `d000 0110` (docs §11.4): d = 1 pressed, 0 released.
#define COPS_BTN_DOWN 0x86
#define COPS_BTN_UP   0x06

// Clamp accumulated mouse movement to the signed-byte report range.
#define COPS_DELTA_CLAMP(v) ((v) > 127 ? 127 : ((v) < -127 ? -127 : (v)))

// Mouse report interval: nnn (command low 3 bits) × 4 ms.  At the Lisa's
// 5.09375 MHz CPU, 4 ms ≈ 20375 cycles.  Once enabled, the COPS reports every
// interval even when idle (dx=dy=0) — the boot ROM's COPS input loop (WT4INPUT)
// blocks on these, so the periodic report is what keeps boot alive.
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
    via_t *via1;
    struct scheduler *sched;

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
    bool mouse_button; // last host-injected button state (for edge detection)
};

// === Response FIFO ==========================================================

static bool fifo_empty(const cops_t *c) {
    return c->head == c->tail;
}

static void fifo_push(cops_t *c, uint8_t byte) {
    int next = (c->tail + 1) % COPS_FIFO;
    if (next == c->head) {
        LOG(1, "cops response FIFO full, dropping 0x%02x", byte);
        return;
    }
    c->fifo[c->tail] = byte;
    c->tail = next;
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
    fifo_push(c, COPS_MOUSE_MARK);
    fifo_push(c, (uint8_t)c->mouse_dx);
    fifo_push(c, (uint8_t)c->mouse_dy);
    c->mouse_dx = 0; // deltas reset once reported
    c->mouse_dy = 0;
    cops_kick_pump(c);
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

void cops_inject_mouse(cops_t *c, int dx, int dy, int button) {
    if (!c)
        return;
    // Accumulate deltas the way the real COPS sums pulse edges between reports;
    // cops_mouse_tick emits them (guest must have enabled mouse interrupts).
    c->mouse_dx = (int8_t)COPS_DELTA_CLAMP((int)c->mouse_dx + dx);
    c->mouse_dy = (int8_t)COPS_DELTA_CLAMP((int)c->mouse_dy + dy);
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

// Process a command byte jammed by the host (docs/lisa.md §11.1).  POST only
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
        fifo_push(c, COPS_RSTCODE); // $80
        fifo_push(c, 0xE0); // clock-data marker
        for (int i = 0; i < 5; i++)
            fifo_push(c, 0x00); // 5 time bytes
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
            fifo_push(c, COPS_RSTCODE);
            fifo_push(c, COPS_KBD_ID);
            cops_kick_pump(c);
            LOG(1, "cops reset released → keyboard id 0x%02x", COPS_KBD_ID);
        }
        c->reset_asserted = reset_now;
    }
}

// === Lifecycle =============================================================

cops_t *cops_init(via_t *via1, struct scheduler *scheduler, checkpoint_t *cp) {
    cops_t *c = (cops_t *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->via1 = via1;
    c->sched = scheduler;
    scheduler_new_event_type(scheduler, "cops", c, "pump", &cops_pump);
    scheduler_new_event_type(scheduler, "cops", c, "mouse", &cops_mouse_tick);
    scheduler_new_event_type(scheduler, "cops", c, "crdy", &cops_crdy_tick);
    // Start the free-running CRDY (PB6) toggle from the ready (low) state.
    cops_set_crdy(c, false);
    scheduler_new_cpu_event(scheduler, &cops_crdy_tick, c, 0, COPS_CRDY_HALF_CYCLES, 0);
    if (cp)
        cops_checkpoint(c, cp); // restore (symmetric with save below)
    return c;
}

void cops_delete(cops_t *c) {
    if (!c)
        return;
    if (c->sched) {
        remove_event(c->sched, &cops_pump, c);
        remove_event(c->sched, &cops_mouse_tick, c);
        remove_event(c->sched, &cops_crdy_tick, c);
    }
    free(c);
}

void cops_checkpoint(cops_t *c, checkpoint_t *cp) {
    // Symmetric no-op for now (same discipline as lisa_mmu_checkpoint): the
    // COPS reset handshake re-derives its state from VIA1 on the next scan.
    // Full save/restore of the FIFO + command state lands in Step 9 (R7).
    (void)c;
    (void)cp;
}
