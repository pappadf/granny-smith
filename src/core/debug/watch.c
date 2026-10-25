// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// watch.c
// The machine watch (watch.h): the periodic status line and the "stuck"
// detectors.  Reads only: nothing here changes guest or host state, and the
// guest memory it samples goes through the debug readers (no side effects).

#include "watch.h"

#include "cpu.h" // cpu_get_pc
#include "debug.h" // debug_mac_xlate
#include "display.h" // display_t
#include "drive_activity.h" // DRIVE_KIND_*
#include "log.h"
#include "memory.h" // memory_debug_read_*
#include "platform_clock.h" // host_time_ms
#include "platform_hooks.h" // platform_checkpoint_auto_get
#include "ppc.h" // ppc_get_pc / ppc_get_gpr
#include "ppc_jit.h" // ppc_jit_status
#include "scheduler.h" // scheduler_instr_count / scheduler_is_running
#include "system.h"
#include "system_internal.h" // config_t::cpu / ppc

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Mac OS low-memory globals the watch samples (mac_globals_data.c)
#define MAC_TICKS      0x016Au // Ticks: 60 Hz count the VBL task advances
#define MAC_CUR_AP     0x0910u // CurApName: the current application, Str31
#define MAC_DS_ERRCODE 0x0AF0u // DSErrCode: the last system error ID

// The detectors' thresholds, in wall-clock seconds
#define WATCH_TICKS_FROZEN_S 15 // Ticks still this long while the CPU runs: the OS is hung
#define WATCH_CPU_STILL_S    15 // a running machine retiring no instructions this long
#define WATCH_GAP_S          3 // a poll this late: the emulator thread was blocked

// What the watch remembers between polls
typedef struct {
    bool primed; // a first sample has set the baselines
    config_t *machine; // the machine the baselines belong to
    double last_poll_ms; // the last poll (the once-a-second gate)
    double last_report_ms; // the last status line
    double start_ms; // the first poll (the line's uptime)
    // Baselines of the status line's rates
    uint64_t rep_instr, rep_jit;
    uint64_t rep_reads[DRIVE_KIND_COUNT], rep_writes[DRIVE_KIND_COUNT];
    unsigned long rep_alerts;
    // The detectors' state
    uint32_t ticks; // Ticks at the last change
    bool ticks_alive; // Ticks has been seen advancing (the OS runs a VBL)
    double ticks_since_ms; // when Ticks last changed
    bool ticks_alerted; // the frozen alert is out (until Ticks moves again)
    uint64_t instr; // instructions at the last change
    double instr_since_ms; // when the count last changed
    bool instr_alerted; // the still alert is out (until it moves again)
    uint16_t ds_err; // DSErrCode at the last poll
} watch_t;

static watch_t g_watch;
static uint32_t g_interval_s = 60; // log.status_every

// The devices' reporters (watch_add_reporter)
static struct {
    watch_reporter_fn fn;
    void *ctx;
} g_reporters[WATCH_MAX_REPORTERS];

void watch_add_reporter(watch_reporter_fn fn, void *ctx) {
    for (int i = 0; i < WATCH_MAX_REPORTERS; i++)
        if (!g_reporters[i].fn) {
            g_reporters[i].fn = fn;
            g_reporters[i].ctx = ctx;
            return;
        }
}

void watch_remove_reporter(void *ctx) {
    for (int i = 0; i < WATCH_MAX_REPORTERS; i++)
        if (g_reporters[i].ctx == ctx)
            g_reporters[i].fn = NULL, g_reporters[i].ctx = NULL;
}

uint32_t watch_get_interval(void) {
    return g_interval_s;
}

void watch_set_interval(uint32_t seconds) {
    g_interval_s = seconds;
}

// Guest reads in the Mac world's logical space (0 with no memory)
static uint16_t mac_read16(uint32_t addr) {
    return system_memory() ? memory_debug_read_uint16(debug_mac_xlate(addr)) : 0;
}
static uint32_t mac_read32(uint32_t addr) {
    return system_memory() ? memory_debug_read_uint32(debug_mac_xlate(addr)) : 0;
}

// The current application's name (CurApName), printable, or "" when the
// global holds no plausible Pascal string (not Mac OS, or not yet set)
static void mac_app_name(char *out, size_t size) {
    out[0] = '\0';
    if (!system_memory() || size < 2)
        return;
    uint8_t len = memory_debug_read_uint8(debug_mac_xlate(MAC_CUR_AP));
    if (len == 0 || len > 31)
        return;
    size_t n = 0;
    for (uint32_t i = 0; i < len && n + 1 < size; i++) {
        uint8_t c = memory_debug_read_uint8(debug_mac_xlate(MAC_CUR_AP + 1 + i));
        if (c < 0x20 || c >= 0x7F)
            c = '?'; // MacRoman high half and controls: keep the line ASCII
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

// The running CPU's PC: "PC=xxxxxxxx" (PowerPC: with the 68K emulator's r24)
static void format_pc(config_t *m, char *buf, size_t size) {
    if (m && m->ppc)
        snprintf(buf, size, "PC=%08x r24=%08x", (unsigned)ppc_get_pc(m->ppc), (unsigned)ppc_get_gpr(m->ppc, 24));
    else
        snprintf(buf, size, "PC=%08x", (unsigned)(m && m->cpu ? cpu_get_pc(m->cpu) : 0));
}

// The screen in a few words: geometry, format and whether it is one flat
// colour (a black screen reads "uniform 000000")
static void format_screen(char *buf, size_t size) {
    display_t *d = system_display();
    if (!d || !d->bits || !d->width || !d->height) {
        snprintf(buf, size, "screen none");
        return;
    }
    int k = snprintf(buf, size, "screen %ux%u %s", d->width, d->height, display_format_name(d->format));
    if (k < 0 || (size_t)k >= size)
        return;
    if (d->presented_externally) {
        snprintf(buf + k, size - (size_t)k, " (GPU overlay)");
        return;
    }
    // Sample an 8x8 grid of rows and bytes: all equal is a flat screen
    uint32_t row_bytes = d->stride;
    uint8_t first = d->bits[0];
    bool flat = true;
    for (uint32_t gy = 0; gy < 8 && flat; gy++) {
        const uint8_t *row = d->bits + (size_t)(d->height * gy / 8) * d->stride;
        for (uint32_t gx = 0; gx < 8; gx++)
            if (row[row_bytes * gx / 8] != first) {
                flat = false;
                break;
            }
    }
    if (flat)
        snprintf(buf + k, size - (size_t)k, " (uniform, every sampled byte %02x)", first);
}

// One status line: what the machine is doing, and what changed since the
// last line
static void report(config_t *m, double now_ms) {
    scheduler_t *sched = m ? m->scheduler : NULL;
    uint64_t instr = scheduler_instr_count(sched);
    ppc_jit_status_t js;
    ppc_jit_status(&js);
    uint64_t reads[DRIVE_KIND_COUNT], writes[DRIVE_KIND_COUNT];
    system_drive_io_counts(reads, writes);
    unsigned long alerts = log_alert_total();

    double secs = (now_ms - g_watch.last_report_ms) / 1000.0;
    uint64_t di = instr >= g_watch.rep_instr ? instr - g_watch.rep_instr : instr;
    uint64_t dj = js.insns >= g_watch.rep_jit ? js.insns - g_watch.rep_jit : js.insns;
    double mips = secs > 0 ? (double)di / secs / 1e6 : 0;

    char pc[48], screen[96], app[40];
    format_pc(m, pc, sizeof pc);
    format_screen(screen, sizeof screen);
    mac_app_name(app, sizeof app);

    // Disk traffic since the last line, per kind (read/write calls)
    uint64_t d[2 * DRIVE_KIND_COUNT];
    for (int k = 0; k < DRIVE_KIND_COUNT; k++) {
        d[2 * k] = reads[k] >= g_watch.rep_reads[k] ? reads[k] - g_watch.rep_reads[k] : reads[k];
        d[2 * k + 1] = writes[k] >= g_watch.rep_writes[k] ? writes[k] - g_watch.rep_writes[k] : writes[k];
    }

    log_status("t+%.0fs %s, %s, %.1f MIPS (jit %s %.0f%%), %" PRIu64 " instr; disk r/w hd %" PRIu64 "/%" PRIu64
               " cd %" PRIu64 "/%" PRIu64 " fd %" PRIu64 "/%" PRIu64
               "; mac Ticks=%u app=\"%s\" DSErrCode=%d; %s; checkpoint.auto=%s; alerts %lu (+%lu)",
               (now_ms - g_watch.start_ms) / 1000.0, sched && scheduler_is_running(sched) ? "running" : "stopped", pc,
               mips, js.on ? "on" : "off", di ? 100.0 * (double)dj / (double)di : 0.0, instr, d[2 * DRIVE_KIND_HD],
               d[2 * DRIVE_KIND_HD + 1], d[2 * DRIVE_KIND_CD], d[2 * DRIVE_KIND_CD + 1], d[2 * DRIVE_KIND_FD],
               d[2 * DRIVE_KIND_FD + 1], (unsigned)mac_read32(MAC_TICKS), app, (int)(int16_t)mac_read16(MAC_DS_ERRCODE),
               screen, platform_checkpoint_auto_get() ? "on" : "off", alerts, alerts - g_watch.rep_alerts);

    // Each device's own line
    for (int i = 0; i < WATCH_MAX_REPORTERS; i++) {
        if (!g_reporters[i].fn)
            continue;
        char line[512] = "";
        g_reporters[i].fn(g_reporters[i].ctx, line, sizeof line);
        if (line[0])
            log_status("%s", line);
    }

    g_watch.last_report_ms = now_ms;
    g_watch.rep_instr = instr;
    g_watch.rep_jit = js.insns;
    memcpy(g_watch.rep_reads, reads, sizeof reads);
    memcpy(g_watch.rep_writes, writes, sizeof writes);
    g_watch.rep_alerts = alerts;
}

// Re-baselines everything on `m` (a first poll, or another machine)
static void prime(config_t *m, double now_ms) {
    memset(&g_watch, 0, sizeof g_watch);
    g_watch.primed = true;
    g_watch.machine = m;
    g_watch.start_ms = g_watch.last_poll_ms = g_watch.last_report_ms = now_ms;
    g_watch.rep_instr = g_watch.instr = scheduler_instr_count(m ? m->scheduler : NULL);
    ppc_jit_status_t js;
    ppc_jit_status(&js);
    g_watch.rep_jit = js.insns;
    system_drive_io_counts(g_watch.rep_reads, g_watch.rep_writes);
    g_watch.rep_alerts = log_alert_total();
    g_watch.ticks = mac_read32(MAC_TICKS);
    g_watch.ticks_since_ms = g_watch.instr_since_ms = now_ms;
    g_watch.ds_err = mac_read16(MAC_DS_ERRCODE);
}

// The detectors: a system error, Ticks frozen, the CPU not advancing
static void detect(config_t *m, double now_ms) {
    scheduler_t *sched = m->scheduler;
    bool running = sched && scheduler_is_running(sched);
    char pc[48];

    // A Mac OS system error (the bomb box, or a debugger break with one):
    // DSErrCode changes to the error's ID.  Those are positive; the start-up
    // code leaves negative status values there in passing (-14134, -1).
    uint16_t ds = mac_read16(MAC_DS_ERRCODE);
    if (ds != g_watch.ds_err && (int16_t)ds > 0 && g_watch.ticks_alive) {
        format_pc(m, pc, sizeof pc);
        LOG_ALERT("Mac OS system error %d (DSErrCode changed from %d): the guest crashed (%s)", (int)(int16_t)ds,
                  (int)(int16_t)g_watch.ds_err, pc);
    }
    g_watch.ds_err = ds;

    if (!running) {
        // A stopped machine is not stuck: restart the clocks
        g_watch.ticks_since_ms = g_watch.instr_since_ms = now_ms;
        return;
    }

    // The CPU retires instructions
    uint64_t instr = scheduler_instr_count(sched);
    if (instr != g_watch.instr) {
        g_watch.instr = instr;
        g_watch.instr_since_ms = now_ms;
        g_watch.instr_alerted = false;
    } else if (!g_watch.instr_alerted && now_ms - g_watch.instr_since_ms >= WATCH_CPU_STILL_S * 1000.0) {
        g_watch.instr_alerted = true;
        format_pc(m, pc, sizeof pc);
        LOG_ALERT("the machine is running but its CPU has retired no instruction for %d s (%s): halted or wedged",
                  WATCH_CPU_STILL_S, pc);
    }

    // Ticks advances (the OS's 60 Hz interrupt is being taken).  Armed only
    // once it has been seen moving, so a ROM boot or another OS does not trip it.
    uint32_t ticks = mac_read32(MAC_TICKS);
    if (ticks != g_watch.ticks) {
        if (g_watch.ticks_alerted)
            log_status("Mac OS Ticks advances again (%u)", (unsigned)ticks);
        g_watch.ticks_alive = g_watch.ticks_alive || g_watch.ticks != 0;
        g_watch.ticks = ticks;
        g_watch.ticks_since_ms = now_ms;
        g_watch.ticks_alerted = false;
    } else if (g_watch.ticks_alive && !g_watch.ticks_alerted &&
               now_ms - g_watch.ticks_since_ms >= WATCH_TICKS_FROZEN_S * 1000.0 && !g_watch.instr_alerted) {
        g_watch.ticks_alerted = true;
        format_pc(m, pc, sizeof pc);
        LOG_ALERT("Mac OS Ticks has been stuck at %u for %d s while the CPU runs (%s): the guest looks hung "
                  "(interrupts masked, or spinning)",
                  (unsigned)ticks, WATCH_TICKS_FROZEN_S, pc);
    }
}

void watch_poll(double now_ms) {
    config_t *m = system_running();
    if (!g_watch.primed || m != g_watch.machine) {
        prime(m, now_ms);
        return;
    }
    double gap = now_ms - g_watch.last_poll_ms;
    if (gap < 1000.0)
        return;
    g_watch.last_poll_ms = now_ms;
    // The thread that polls is the emulator's: a late poll means it was
    // busy elsewhere that long (the page's 3 s heartbeat watch fires on this)
    if (gap >= WATCH_GAP_S * 1000.0)
        log_status("the emulator thread was blocked for %.1f s", gap / 1000.0);
    if (!m)
        return;
    detect(m, now_ms);
    if (g_interval_s && now_ms - g_watch.last_report_ms >= g_interval_s * 1000.0)
        report(m, now_ms);
}

void watch_report_now(void) {
    double now_ms = host_time_ms();
    if (!g_watch.primed || system_running() != g_watch.machine)
        prime(system_running(), now_ms - 1.0); // fresh baselines: the rates cover the last millisecond
    report(g_watch.machine, now_ms);
}
