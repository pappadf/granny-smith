// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.c
// Mac-specific debugging utilities: trap names, global variable lookup, and process inspection.

#include "debug_mac.h"
#include "gs_out.h"

#include "cpu.h"
#include "debug.h"
#include "debug_data.h"
#include "memory.h"
#include "mmu.h"
#include "mouse.h"
#include "rtc.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// debug_mac_atrap_name() lives in mac_traps_data.c, beside the table it reads.
uint32_t debug_mac_lookup_global_address(const char *name) {
    const mac_global_info_t *g = mac_global_find(name);
    return g ? g->address : 0; // 0 = not found
}

// Every 68k-world address in this file resolves through the mac-world
// translation first: identity on 68K machines, the user-data view on PDM
// (debug.h cpu_debug_if_t.translate_mac).
uint32_t debug_mac_xlate(uint32_t addr) {
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (dif && dif->translate_mac) {
        bool ok;
        uint32_t pa = dif->translate_mac(dif->ctx, addr, &ok);
        if (ok)
            return pa;
    }
    return addr;
}

// Memory read helpers (mac-world addresses; see debug_mac_xlate above)
static uint8_t read8(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_debug_read_uint8(debug_mac_xlate(addr));
}
static uint16_t read16(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_debug_read_uint16(debug_mac_xlate(addr));
}
static uint32_t read32(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_debug_read_uint32(debug_mac_xlate(addr));
}

// Memory write helpers — same mac-world address translation as the read
// helpers above.  Load-bearing on the PPC machines, where 68k logical low
// memory is relocated (framebuffer carve-out): an untranslated write to
// $0828 lands in the framebuffer instead of MTemp.
static void write8(uint32_t addr, uint8_t value) {
    if (system_memory())
        memory_debug_write_uint8(debug_mac_xlate(addr), value);
}
static void write16(uint32_t addr, uint16_t value) {
    if (system_memory())
        memory_debug_write_uint16(debug_mac_xlate(addr), value);
}
static void write32(uint32_t addr, uint32_t value) {
    if (system_memory())
        memory_debug_write_uint32(debug_mac_xlate(addr), value);
}

// Read the Pascal string (length byte, then that many characters) at addr
// into buffer as a C string, truncated to fit.  Returns the stored length.
static size_t read_pstring(uint32_t addr, char *buffer, size_t max_length) {
    if (max_length == 0)
        return 0; // No room for NUL terminator
    size_t length = read8(addr);
    if (length > max_length - 1)
        length = max_length - 1;
    for (size_t i = 0; i < length; i++)
        buffer[i] = (char)read8(addr + 1 + (uint32_t)i);
    buffer[length] = '\0';
    return length;
}

// Read the 32-bit low-memory global `name`; *found reports whether the
// globals table knows it (the value is 0 when it does not).
static uint32_t read_global32(const char *name, bool *found) {
    uint32_t addr = debug_mac_lookup_global_address(name);
    *found = addr != 0;
    return addr ? read32(addr) : 0;
}

// Print the current application's name and memory layout from the
// low-memory globals (CurApName, ApplZone, ApplLimit, CurrentA5,
// CurStackBase).
void debug_mac_print_process_info(void) {
    gs_outf("--- Current Application Info ---\n");

    // 1. The application name: CurApName is a Pascal string (Str31)
    char app_name[64] = "";
    uint32_t addr_name = debug_mac_lookup_global_address("CurApName");
    if (addr_name)
        read_pstring(addr_name, app_name, sizeof(app_name));
    gs_outf("Name: %s\n", app_name);

    // 2. The application's memory map
    gs_outf("\n--- Memory Map ---\n");

    bool have_zone, have_limit, have_a5, have_stack;
    uint32_t heap_start_ptr = read_global32("ApplZone", &have_zone);
    uint32_t heap_limit_ptr = read_global32("ApplLimit", &have_limit);
    uint32_t a5_world_ptr = read_global32("CurrentA5", &have_a5);
    uint32_t stack_base_ptr = read_global32("CurStackBase", &have_stack);
    if (!have_zone || !have_limit || !have_a5 || !have_stack) {
        gs_outf("(ApplZone/ApplLimit/CurrentA5/CurStackBase missing from the globals table)\n");
        return;
    }

    gs_outf("Application Partition Start: $%08X\n", heap_start_ptr);
    gs_outf("Application Partition Limit: $%08X\n", heap_limit_ptr);
    gs_outf("  Heap Start:                $%08X\n", heap_start_ptr);
    gs_outf("  A5 World (CurrentA5):      $%08X\n", a5_world_ptr);
    gs_outf("  Stack Base:                $%08X (grows downwards)\n", stack_base_ptr);
    if (heap_limit_ptr >= heap_start_ptr) {
        uint32_t partition_size_bytes = heap_limit_ptr - heap_start_ptr;
        gs_outf("Total Partition Size:        %u bytes (%u.%u KB)\n", partition_size_bytes, partition_size_bytes / 1024,
                (partition_size_bytes % 1024) * 10 / 1024);
    } else {
        gs_outf("Total Partition Size:        <invalid: heap limit $%08X < start $%08X>\n", heap_limit_ptr,
                heap_start_ptr);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Target (68K) backtrace and diagnostic functions
// ────────────────────────────────────────────────────────────────────────────

#define BACKTRACE_MAX_FRAMES 16

// Print target 68K backtrace by walking stack frames.  Frame #0 is the
// instruction at the PC, disassembled; the rest are return addresses, shown
// as addresses only (a return address points mid-routine, so disassembling
// there tells nothing about the frame).
void debug_mac_print_target_backtrace(void) {
    gs_outf("\n=== Target 68K backtrace ===\n");
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        gs_outf("(CPU not initialized)\n");
        return;
    }

    // Frame-walk using A6 as frame pointer, printing return addresses
    uint32_t pc = cpu_get_pc(cpu);
    char linebuf[160];
    debugger_disasm(linebuf, sizeof(linebuf), pc);
    gs_outf("#0  %s\n", linebuf);

    // Frame pointers already walked: a chain that comes back to any of them
    // (A -> B -> A, not just A -> A) is a cycle, and the walk stops.
    uint32_t visited[BACKTRACE_MAX_FRAMES];
    int n_visited = 0;
    uint32_t a6 = cpu_get_an(cpu, 6);
    for (int depth = 1; depth <= BACKTRACE_MAX_FRAMES; depth++) {
        if (a6 == 0)
            break; // end of chain
        // A frame pointer is word-aligned, above the vectors and inside the
        // address space; anything else is not a frame (e.g. A6 used as a
        // scratch register just before a LINK).
        if (a6 < 0x100 || a6 > g_address_mask || (a6 & 1))
            break;
        for (int i = 0; i < n_visited; i++)
            if (visited[i] == a6)
                return; // cycle
        visited[n_visited++] = a6;
        // Standard 68K frame: [0]: previous A6, [4]: return address
        uint32_t prev_a6 = read32(a6 + 0);
        uint32_t ret = read32(a6 + 4);
        if (ret == 0 || ret > g_address_mask)
            break;
        gs_outf("#%d  $%08X  (frame $%08X)\n", depth, ret, a6);
        a6 = prev_a6;
    }
}

// Print current Mac application process info (wrapper for diagnostic output)
void debug_mac_print_process_info_header(void) {
    gs_outf("\n=== Current Mac application ===\n");
    debug_mac_print_process_info();
}

// ────────────────────────────────────────────────────────────────────────────
// Mouse automation commands for E2E testing
// (Moved from test.c - these interact with Mac OS low-memory globals)
// ────────────────────────────────────────────────────────────────────────────

// Each set_mouse_* route returns -1 when it could not attempt the move at
// all (a missing global, no mouse device) and 0 otherwise; the reason for a
// failure has been printed by then.  The aux route is best-effort past that
// point (see set_mouse_aux).

// Payload of the mouse trace event (bits 0-15 h, bits 16-31 v).
static uint64_t mouse_point_pack(int16_t h, int16_t v) {
    return (uint64_t)(uint16_t)h | ((uint64_t)(uint16_t)v << 16);
}

// Writes absolute cursor position to Mac low-memory globals (MTemp, RawMouse, Mouse, CrsrNew).
// This is the classic technique used by ChromiVNC/MiniVNC and Basilisk II.
// Note: on SE/30 (and other NuBus-capable Macs), this updates the position globals
// but may not redraw the cursor image on screen until the next slot VBL fires.
static int set_mouse_global(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");

    if (!addr_MTemp || !addr_RawMouse || !addr_CrsrNew) {
        gs_outf("Error: could not resolve mouse-related globals.\n");
        return -1;
    }

    uint16_t v = (uint16_t)(y & 0xFFFF); // vertical in high word
    uint16_t h = (uint16_t)(x & 0xFFFF); // horizontal in low word

    // Write new position to MTemp and RawMouse (the interrupt-level inputs)
    write16(addr_MTemp, v);
    write16(addr_MTemp + 2, h);
    write16(addr_RawMouse, v);
    write16(addr_RawMouse + 2, h);

    // Also write Mouse directly so GetMouse returns the correct value immediately
    if (addr_Mouse) {
        write16(addr_Mouse, v);
        write16(addr_Mouse + 2, h);
    }

    // Signal the cursor VBL task: copy CrsrCouple → CrsrNew (standard technique)
    if (addr_CrsrCouple) {
        uint8_t couple = read8(addr_CrsrCouple);
        write8(addr_CrsrNew, couple);
    } else {
        write8(addr_CrsrNew, 0xFF);
    }
    return 0;
}

// Injects relative mouse movement through the hardware path (ADB or quadrature).
// Preserves the current button state on both ADB and non-ADB machines.
static int set_mouse_hw(long dx, long dy) {
    bool injected = system_mouse_move((int)dx, (int)dy);
    if (!injected) {
        gs_outf("Error: no mouse device available for hardware injection.\n");
        return -1;
    }
    return 0;
}

// Translate `va` against the cached MAE user CRP and write `value` to the
// resolved physical address as a 16-bit big-endian word, bypassing the
// SoA fast path entirely.  Returns true on success; false if the CRP
// snapshot is empty or the page isn't mapped in MAE's address space.
static bool aux_write_uint16(uint32_t va, uint16_t value) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_mmu->last_user_crp, &pa))
        return false;
    return mmu_write_physical_uint16(g_mmu, pa, value);
}

// Same as aux_write_uint16 but for a single byte (used for CrsrNew).
static bool aux_write_uint8(uint32_t va, uint8_t value) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_mmu->last_user_crp, &pa))
        return false;
    return mmu_write_physical_uint8(g_mmu, pa, value);
}

// Same as aux_write_uint16 but for an 8-bit read (used to sample CrsrCouple).
// Returns false (and leaves *out untouched) if the page isn't mapped.
static bool aux_read_uint8(uint32_t va, uint8_t *out) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_mmu->last_user_crp, &pa))
        return false;
    *out = mmu_read_physical_uint8(g_mmu, pa);
    return true;
}

// Count of the aux writes attempted and of those that landed
typedef struct {
    int ok;
    int total;
} aux_tally_t;

// Record one aux write's outcome in the tally
static void aux_tally(aux_tally_t *t, bool ok) {
    t->total++;
    if (ok)
        t->ok++;
}

// Set mouse position under A/UX 3.0 Mac OS compatibility (MAE).
//
// A/UX runs Mac OS apps under the Macintosh Application Environment, a
// per-process user-mode Toolbox emulator.  Each MAE process sees the
// standard Toolbox globals (MTemp $0828, RawMouse $082C, Mouse $0830,
// CrsrNew $08CE, CrsrCouple $08CF) in its own user virtual address
// space.  At the same VAs in supervisor mode A/UX has unrelated kernel
// data — so any write that rides the active SoA is correct only if the
// CPU happens to be in user mode at the instant of the write.
//
// `--global` ignores that distinction: it writes via the active SoA, so
// under A/UX a write made while supervisor is active lands on the kernel's
// $0828 region instead.  Forbidden under A/UX.
//
// `--aux` translates each VA against the *cached MAE CRP*
// (`mmu_state_t.last_user_crp`, snapshotted by cpu_internal.h on every
// supervisor→user transition) and writes directly to the resolved
// physical address via `mmu_write_physical_uint16`.  Three consequences:
//
//   1. The write reaches MAE's MTemp regardless of whether the CPU is
//      currently in supervisor or user mode — the translation uses MAE's
//      page tables, not the active CPU mode.
//   2. The kernel's $0828 region is never touched; A/UX kernel data is
//      safe.
//   3. The Toolbox globals are written exactly once per `set-mouse --aux`
//      call, so there is no recurring race against MAE's own cursor
//      updates.
//
// If no user-mode entry has been observed yet (`last_user_crp == 0`),
// or the snapshot CRP doesn't map a page for one of the target VAs, the
// write is reported as failed and silently skipped — better than landing
// on the wrong page.
static int set_mouse_aux(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");

    if (!addr_MTemp || !addr_RawMouse || !addr_CrsrNew) {
        gs_outf("Error: could not resolve mouse-related globals.\n");
        return -1;
    }
    if (!g_mmu || !g_mmu->enabled) {
        gs_outf("set-mouse --aux: MMU not enabled; falling back to active-SoA write.\n");
        return set_mouse_global(x, y);
    }
    if (g_mmu->last_user_crp == 0) {
        // Reported, not failed: the guest has not entered user mode yet,
        // which is a state of the guest rather than a fault in the request.
        gs_outf("set-mouse --aux: no user-mode CRP observed yet; run the guest into user mode first.\n");
        return 0;
    }

    uint16_t v = (uint16_t)(y & 0xFFFF); // vertical word
    uint16_t h = (uint16_t)(x & 0xFFFF); // horizontal word

    // Write Toolbox position globals into MAE's address space.  Each
    // write is independent so a partial mapping reports cleanly.
    aux_tally_t tally = {0, 0};
    aux_tally(&tally, aux_write_uint16(addr_MTemp, v));
    aux_tally(&tally, aux_write_uint16(addr_MTemp + 2, h));
    aux_tally(&tally, aux_write_uint16(addr_RawMouse, v));
    aux_tally(&tally, aux_write_uint16(addr_RawMouse + 2, h));
    if (addr_Mouse) {
        aux_tally(&tally, aux_write_uint16(addr_Mouse, v));
        aux_tally(&tally, aux_write_uint16(addr_Mouse + 2, h));
    }

    // Signal MAE's cursor VBL task: copy CrsrCouple → CrsrNew (standard
    // technique used by --global too).
    uint8_t couple = 0xFF;
    if (addr_CrsrCouple)
        (void)aux_read_uint8(addr_CrsrCouple, &couple);
    aux_tally(&tally, aux_write_uint8(addr_CrsrNew, couple));

    gs_outf("set-mouse --aux: wrote MTemp/RawMouse/Mouse = (h=%d, v=%d) via MAE CRP $%08X (%d/%d writes ok)\n", (int)x,
            (int)y, (uint32_t)(g_mmu->last_user_crp & 0xFFFFFFFF), tally.ok, tally.total);
    // Best-effort: a partial mapping (a process without MAE's globals mapped,
    // e.g. under X11) is reported above, not failed.
    return 0;
}

// Default set-mouse: absolute coordinates, platform-dependent strategy.
// ADB (SE/30): computes deltas from current position and injects via ADB hardware.
// Non-ADB (Plus): writes globals directly.
static int set_mouse_default(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    if (!addr_MTemp) {
        gs_outf("Error: could not resolve MTemp.\n");
        return -1;
    }

    // Read current cursor position from MTemp
    int16_t cur_v = (int16_t)read16(addr_MTemp);
    int16_t cur_h = (int16_t)read16(addr_MTemp + 2);
    int dx = (int)x - (int)cur_h;
    int dy = (int)y - (int)cur_v;

    // Closed-loop correction: MTemp lags the injected deltas (ADB poll
    // plus — on machines whose cursor rides the Cursor Device Manager,
    // like the PCI PowerMacs — a VBL task before MTemp moves).  Host
    // events arrive faster than that loop turns around, so subtract
    // whatever is still queued at the ADB device or the correction gets
    // injected once per host event instead of once — overshoot and
    // rubber-banding.
    int pend_dx = 0, pend_dy = 0;
    bool has_adb = system_mouse_pending_adb(&pend_dx, &pend_dy);
    dx -= pend_dx;
    dy -= pend_dy;

    // On ADB machines, inject deltas through ADB so the ROM ISR naturally
    // updates the cursor position (including the screen cursor image).
    // Non-ADB machines (Plus) fall through to global writes.
    if (has_adb) {
        if (dx != 0 || dy != 0)
            system_mouse_move(dx, dy); // routes through ADB on an ADB machine
        return 0;
    }
    return set_mouse_global(x, y);
}

// Set the mouse cursor position via the requested route (mouse_route_t).
// Returns 0 on success, -1 if memory is unavailable or the route failed.
// Coordinates are clamped to int16 for the absolute routes (global / aux /
// default) since the Mac OS Point type is 16-bit signed; the hardware route
// passes deltas through unchanged.
int debug_mac_set_mouse_mode(long x, long y, mouse_route_t route) {
    if (!system_memory())
        return -1;
    if (route != MOUSE_ROUTE_HW) {
        if (x < INT16_MIN)
            x = INT16_MIN;
        else if (x > INT16_MAX)
            x = INT16_MAX;
        if (y < INT16_MIN)
            y = INT16_MIN;
        else if (y > INT16_MAX)
            y = INT16_MAX;
    }
    switch (route) {
    case MOUSE_ROUTE_GLOBAL:
        return set_mouse_global(x, y);
    case MOUSE_ROUTE_HW:
        return set_mouse_hw(x, y);
    case MOUSE_ROUTE_AUX:
        return set_mouse_aux(x, y);
    case MOUSE_ROUTE_DEFAULT:
        return set_mouse_default(x, y);
    case MOUSE_ROUTE_INVALID:
        break;
    }
    return -1;
}

// ---- trace-mouse implementation ----
// A 1 Hz event that reads the classic Mac low-memory Mouse (Point {v,h}) and
// prints it when it changes.  The event is the trace's only state: the
// payload is the last sample printed (bit 32 = there is one), and the trace
// is on exactly while the event is pending.

#define TRACE_MOUSE_INTERVAL_NS 1000000000ULL // 1 s
#define TRACE_MOUSE_HAVE_LAST   (1ull << 32)

void debug_mac_mouse_trace_tick(void *source, uint64_t data) {
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    if (!addr_Mouse) {
        // Not rescheduling ends the trace (its event IS its state).
        gs_outf("[trace-mouse] the Mouse global is unknown; trace stopped\n");
        return;
    }
    int16_t v = (int16_t)memory_debug_read_uint16(addr_Mouse);
    int16_t h = (int16_t)memory_debug_read_uint16(addr_Mouse + 2);
    uint64_t sample = mouse_point_pack(h, v) | TRACE_MOUSE_HAVE_LAST;
    if (sample != data)
        gs_outf("[trace-mouse] h=%d v=%d\n", h, v);
    scheduler_new_cpu_event(system_scheduler(), &debug_mac_mouse_trace_tick, source, sample, 0,
                            TRACE_MOUSE_INTERVAL_NS);
}

// === Public mouse / trace control =====================================
//
// Thin wrappers around the file-private helpers used by the typed
// `mouse.move` / `mouse.click` / `mouse.trace` root methods.

void debug_mac_set_trace_mouse(struct host_input *hi, bool enabled) {
    scheduler_t *sched = system_scheduler();
    if (enabled == has_event(sched, &debug_mac_mouse_trace_tick))
        return;
    if (enabled)
        scheduler_new_cpu_event(sched, &debug_mac_mouse_trace_tick, hi, 0, 0, TRACE_MOUSE_INTERVAL_NS);
    else
        remove_event(sched, &debug_mac_mouse_trace_tick, hi);
}

// ---- mouse-button implementation ----
// Injects a mouse button state change.
// --hw (default): routes through hardware emulation (ADB or VIA PB3), which causes
//   the ROM's device handler to write MBState and post mouseDown/mouseUp events.
// --global: writes MBState directly.  On Mac Plus, sets MBTicks to a future value
//   to prevent the VIA interrupt from overwriting MBState (the debounce hack).
//   No event is posted, so event-driven code won't see the click — use --hw for that.

// Writes button state directly to MBState, with MBTicks hack for Mac Plus safety.
static void mouse_button_global(bool button_down) {
    uint32_t addr_MBState = debug_mac_lookup_global_address("MBState");
    uint32_t addr_MBTicks = debug_mac_lookup_global_address("MBTicks");
    uint32_t addr_Ticks = debug_mac_lookup_global_address("Ticks");

    if (!addr_MBState) {
        gs_outf("Error: could not resolve MBState.\n");
        return;
    }

    // MBState bit 7: 0 = button down, 0x80 = button up
    write8(addr_MBState, button_down ? 0x00 : 0x80);

    // MBTicks hack: set MBTicks to a far-future value to prevent the VIA
    // interrupt from overwriting MBState.  Required on Mac Plus where the
    // VIA ISR continuously polls the physical button.  Safe on ADB machines
    // too (the field is unused there).
    if (addr_MBTicks && addr_Ticks) {
        uint32_t ticks = read32(addr_Ticks);
        write32(addr_MBTicks, ticks + 100);
    }
}

// Inject a mouse button up/down event via the requested route.
//   GLOBAL = Mac OS Toolbox MBState write
//   HW / DEFAULT / AUX = ADB/VIA PB3 hardware emulation.  AUX has no
//   button-specific path: the button is hardware state, not a per-process
//   Toolbox global, so the hardware route already reaches MAE.
void debug_mac_mouse_button_mode(bool button_down, mouse_route_t route) {
    switch (route) {
    case MOUSE_ROUTE_GLOBAL:
        mouse_button_global(button_down);
        break;
    case MOUSE_ROUTE_HW:
    case MOUSE_ROUTE_DEFAULT:
    case MOUSE_ROUTE_AUX:
    case MOUSE_ROUTE_INVALID:
        system_mouse_update(button_down, 0, 0);
        break;
    }
}

// Resolves one printable ASCII character to the US-layout ADB raw keycode
// that produces it (for these keys the same as the virtual code), setting
// *shift when the character needs the Shift key.
// Returns -1 for a character the layout cannot type.  Tab and newline resolve
// to their keys so a typed line can carry its own terminator.
int debug_mac_resolve_ascii(char c, bool *shift) {
    // Unshifted keycodes, indexed the way the Apple II-descended ADB layout
    // numbers them (Inside Macintosh: Toolbox Essentials, "Virtual Key Codes").
    static const struct {
        char ch;
        uint8_t code;
    } plain[] = {
        {'a',  0x00},
        {'b',  0x0B},
        {'c',  0x08},
        {'d',  0x02},
        {'e',  0x0E},
        {'f',  0x03},
        {'g',  0x05},
        {'h',  0x04},
        {'i',  0x22},
        {'j',  0x26},
        {'k',  0x28},
        {'l',  0x25},
        {'m',  0x2E},
        {'n',  0x2D},
        {'o',  0x1F},
        {'p',  0x23},
        {'q',  0x0C},
        {'r',  0x0F},
        {'s',  0x01},
        {'t',  0x11},
        {'u',  0x20},
        {'v',  0x09},
        {'w',  0x0D},
        {'x',  0x07},
        {'y',  0x10},
        {'z',  0x06},
        {'0',  0x1D},
        {'1',  0x12},
        {'2',  0x13},
        {'3',  0x14},
        {'4',  0x15},
        {'5',  0x17},
        {'6',  0x16},
        {'7',  0x1A},
        {'8',  0x1C},
        {'9',  0x19},
        {' ',  0x31},
        {'`',  0x32},
        {'-',  0x1B},
        {'=',  0x18},
        {'[',  0x21},
        {']',  0x1E},
        {'\\', 0x2A},
        {';',  0x29},
        {'\'', 0x27},
        {',',  0x2B},
        {'.',  0x2F},
        {'/',  0x2C},
        {'\n', 0x24},
        {'\r', 0x24},
        {'\t', 0x30},
    };
    // Shifted characters and the unshifted character sharing their key.
    static const struct {
        char ch;
        char base;
    } shifted[] = {
        {'!', '1' },
        {'@', '2' },
        {'#', '3' },
        {'$', '4' },
        {'%', '5' },
        {'^', '6' },
        {'&', '7' },
        {'*', '8' },
        {'(', '9' },
        {')', '0' },
        {'~', '`' },
        {'_', '-' },
        {'+', '=' },
        {'{', '[' },
        {'}', ']' },
        {'|', '\\'},
        {':', ';' },
        {'"', '\''},
        {'<', ',' },
        {'>', '.' },
        {'?', '/' },
    };

    if (shift)
        *shift = false;
    if (c >= 'A' && c <= 'Z') {
        if (shift)
            *shift = true;
        c = (char)(c - 'A' + 'a');
    } else {
        for (size_t i = 0; i < sizeof(shifted) / sizeof(shifted[0]); i++) {
            if (shifted[i].ch == c) {
                if (shift)
                    *shift = true;
                c = shifted[i].base;
                break;
            }
        }
    }
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
        if (plain[i].ch == c)
            return plain[i].code;
    return -1;
}

// Resolves a key name to an ADB raw keycode, or -1 if unknown
int debug_mac_resolve_key_name(const char *name) {
    // Named keys, matched case-insensitively.
    static const struct {
        const char *name;
        uint8_t code;
    } named[] = {
        {"return",    0x24},
        {"enter",     0x24},
        {"space",     0x31},
        {"escape",    0x35},
        {"esc",       0x35},
        {"tab",       0x30},
        {"delete",    0x33},
        {"backspace", 0x33},
        // The arrows' RAW codes, $3B-$3E.  These were the virtual codes
        // $7B-$7E, which on the ADB wire are the right-hand modifiers:
        // "left" pressed Right Shift.
        {"up",        0x3E},
        {"down",      0x3D},
        {"left",      0x3B},
        {"right",     0x3C},
        {"command",   0x37},
        {"cmd",       0x37},
        {"shift",     0x38},
        {"option",    0x3A},
        {"alt",       0x3A},
        {"control",   0x36},
        {"ctrl",      0x36},
        // Caps Lock is a locking switch on real Apple keyboards: hold it with
        // keyboard.down and it stays reported in ADB Register 2 until
        // keyboard.up.
        {"capslock",  0x39},
        {"caps",      0x39},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcasecmp(name, named[i].name))
            return named[i].code;

    // Hex keycode (e.g., 0x24)
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char *endp = NULL;
        long val = strtol(name, &endp, 16);
        if (*endp == '\0' && val >= 0 && val <= 0x7F)
            return (int)val;
    }

    // A single printable character -- "a", "7", ";".  This is what makes
    // keyboard.press("a") work, and it had been missing: the resolver knew
    // fourteen named keys and the hex form and nothing else, while adb.c's
    // argument doc promised `Key name ("return"/"esc"/"a"/...)` and its
    // keyboard.type doc promised a-z and 0-9.  So keyboard.press("a") failed
    // on EVERY machine, Mac included.
    //
    // debug_mac_resolve_ascii already holds the layout; it simply was never
    // consulted from here.  Shift is deliberately ignored: this resolves the
    // KEY, and a caller wanting a capital presses shift itself with
    // keyboard.down "shift".  A character that only exists shifted (e.g. "!")
    // therefore resolves to its unshifted key, which is the honest answer for
    // a key-press API.
    if (name[1] == '\0') {
        bool shift = false;
        int code = debug_mac_resolve_ascii(name[0], &shift);
        if (code >= 0)
            return code;
    }

    return -1;
}
