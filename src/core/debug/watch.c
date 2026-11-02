// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// watch.c
// The machine watch (watch.h): the periodic status line and the "stuck"
// detectors.  Reads only: nothing here changes guest or host state, and the
// guest memory it samples goes through the debug readers (no side effects).

#include "watch.h"

#include "breadcrumb.h" // the GPU wait totals
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
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Mac OS low-memory globals the watch samples (mac_globals_data.c)
#define MAC_TICKS       0x016Au // Ticks: 60 Hz count the VBL task advances
#define MAC_CUR_AP      0x0910u // CurApName: the current application, Str31
#define MAC_DS_ERRCODE  0x0AF0u // DSErrCode: the last system error ID
#define MAC_DS_GREETING 40 // dsGreeting: the start-up "Welcome to Macintosh", not an error

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
static uint32_t g_perf_s; // log.perf_every (0: off)

// The tick's parts since the last perf line (watch_note_phase)
#define WATCH_PHASES 8
static struct {
    const char *what;
    uint64_t n;
    double sum, max;
} g_parts[WATCH_PHASES];

// The perf line's baselines
static struct {
    bool primed;
    config_t *machine;
    double ms; // host time of the last line
    uint64_t instr;
    double time_ns; // emulated time
    double sleep_ms;
    ppc_jit_perf_t jit;
    uint32_t gpu_waits, gpu_wait_ms;
    double gap_max; // the longest gap between two polls since the line
} g_perf;

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

uint32_t watch_get_perf_interval(void) {
    return g_perf_s;
}

void watch_set_perf_interval(uint32_t seconds) {
    g_perf_s = seconds;
    g_perf.primed = false;
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

// The longest part of a tick since the last poll (watch_note_phase)
static struct {
    const char *what;
    double ms;
} g_phase;

void watch_note_phase(const char *what, double ms) {
    if (ms > g_phase.ms) {
        g_phase.what = what;
        g_phase.ms = ms;
    }
    for (int i = 0; i < WATCH_PHASES; i++)
        if (g_parts[i].what == what || !g_parts[i].what) {
            g_parts[i].what = what;
            g_parts[i].n++;
            g_parts[i].sum += ms;
            if (ms > g_parts[i].max)
                g_parts[i].max = ms;
            break;
        }
}

// The perf line (log.perf_every): everything the run loop, the scheduler,
// the translation tier and the devices measured since the last one, as
// rates over the interval.  Several lines, one subject each, "perf:" first.
static void perf_take_baseline(config_t *m, double now_ms) {
    scheduler_t *sched = m ? m->scheduler : NULL;
    g_perf.primed = true;
    g_perf.machine = m;
    g_perf.ms = now_ms;
    g_perf.instr = scheduler_instr_count(sched);
    g_perf.time_ns = sched ? scheduler_time_ns(sched) : 0;
    g_perf.sleep_ms = sched ? scheduler_sleep_host_ms(sched) : 0;
    ppc_jit_perf(&g_perf.jit);
    volatile uint32_t *crumb = crumb_block();
    g_perf.gpu_waits = crumb[CRUMB_GPU_WAITS];
    g_perf.gpu_wait_ms = crumb[CRUMB_GPU_WAIT_MS];
    g_perf.gap_max = 0;
    memset(g_parts, 0, sizeof g_parts);
    scheduler_perf_t sp;
    scheduler_perf_take(sched, &sp); // restart its interval counters
}

static void perf_report(config_t *m, double now_ms) {
    scheduler_t *sched = m ? m->scheduler : NULL;
    double secs = (now_ms - g_perf.ms) / 1000.0;
    if (secs <= 0)
        return;
    uint64_t instr = scheduler_instr_count(sched);
    double time_ns = sched ? scheduler_time_ns(sched) : 0;
    double sleep_ms = sched ? scheduler_sleep_host_ms(sched) : 0;
    uint64_t di = instr >= g_perf.instr ? instr - g_perf.instr : 0;
    scheduler_perf_t sp;
    scheduler_perf_take(sched, &sp);
    ppc_jit_perf_t j;
    ppc_jit_perf(&j);
    const ppc_jit_perf_t *b = &g_perf.jit;
    char pc[48];
    format_pc(m, pc, sizeof pc);

    // The machine: throughput against the wall clock and against the time
    // the CPU actually had (run time less the scheduler's sleeps)
    double run_ms = sp.run_ms - (sleep_ms - g_perf.sleep_ms);
    char turbo[32] = "";
    if (!isnan(sp.turbo_ratio))
        snprintf(turbo, sizeof turbo, ", turbo ratio %.2f", sp.turbo_ratio);
    log_status("perf: %.2fs %s %s, %.1f MIPS wall, %.1f MIPS while running (%.0f%% of wall), guest %.3fx real time "
               "(speed %.2fx, governor rung %d%s), %s",
               secs, sched && scheduler_is_running(sched) ? "running" : "stopped", sp.mode, di / secs / 1e6,
               run_ms > 0 ? di / (run_ms * 1000.0) : 0.0, 100.0 * sp.run_ms / (secs * 1000.0),
               time_ns > g_perf.time_ns ? (time_ns - g_perf.time_ns) / (secs * 1e9) : 0.0, sp.speed, sp.governor_rung,
               turbo, pc);

    // The host loop: ticks, the frame-units they ran, and where each tick's
    // time went (count, total, longest)
    char parts[400];
    int k = 0;
    parts[0] = 0;
    for (int i = 0; i < WATCH_PHASES && g_parts[i].what && k >= 0 && (size_t)k < sizeof parts; i++)
        k += snprintf(parts + k, sizeof parts - (size_t)k, " %s %llux %.0f/%.1fms", g_parts[i].what,
                      (unsigned long long)g_parts[i].n, g_parts[i].sum, g_parts[i].max);
    log_status("perf: loop %llu ticks ran %llu frame-units (most %u in one, %llu behind), run %.0fms (longest tick "
               "%.1fms), est %.2fms/frame-unit %.2fms/loop; longest gap between polls %.0fms; parts:%s",
               (unsigned long long)sp.ticks, (unsigned long long)sp.vbls, sp.vbls_max, (unsigned long long)sp.behind,
               sp.run_ms, sp.run_ms_max, sp.host_ms_per_vbl, sp.host_ms_per_loop, g_perf.gap_max, parts);

    // The translation tier, per second
    if (j.on || j.calls) {
        uint64_t dins = j.insns - b->insns;
        log_status("perf: jit %.0f%% of instructions, %.0f calls/s, %.0f deopts/s, %.0f lookups/s, %.1f compiles/s, "
                   "%.1f functions/s, %.0f KB/s generated, %.0f ms compiling (%.0f%%), %.0f deferred/s, "
                   "%.0f block invalidations/s, async %llu sent %llu installed %llu dropped (%lld in flight); "
                   "total %llu regions %llu KB, budget %u%%",
                   di ? 100.0 * (double)dins / (double)di : 0.0, (j.calls - b->calls) / secs,
                   (j.deopts - b->deopts) / secs, (j.lookups - b->lookups) / secs, (j.compiles - b->compiles) / secs,
                   (j.functions - b->functions) / secs, (j.bytes - b->bytes) / 1024.0 / secs,
                   j.compile_ms - b->compile_ms, 100.0 * (j.compile_ms - b->compile_ms) / (secs * 1000.0),
                   (j.deferred - b->deferred) / secs, (j.block_invals - b->block_invals) / secs,
                   (unsigned long long)(j.async_issued - b->async_issued),
                   (unsigned long long)(j.async_landed - b->async_landed),
                   (unsigned long long)(j.async_dropped - b->async_dropped),
                   (long long)(j.async_issued - j.async_landed - j.async_dropped), (unsigned long long)j.regions,
                   (unsigned long long)(j.bytes / 1024), j.budget_pct);
    }

    // Waits on GPU workers (every card's), then each device's own line
    volatile uint32_t *crumb = crumb_block();
    uint32_t waits = crumb[CRUMB_GPU_WAITS] - g_perf.gpu_waits;
    uint32_t wait_ms = crumb[CRUMB_GPU_WAIT_MS] - g_perf.gpu_wait_ms;
    if (waits)
        log_status("perf: gpu waits %u, %u ms (%.0f%% of wall)", waits, wait_ms, 100.0 * wait_ms / (secs * 1000.0));
    for (int i = 0; i < WATCH_MAX_REPORTERS; i++) {
        if (!g_reporters[i].fn)
            continue;
        char line[1536] = "";
        g_reporters[i].fn(g_reporters[i].ctx, line, sizeof line);
        if (line[0])
            log_status("perf: %s", line);
    }

    perf_take_baseline(m, now_ms);
}

// The detectors: a system error, Ticks frozen, the CPU not advancing
static void detect(config_t *m, double now_ms) {
    scheduler_t *sched = m->scheduler;
    bool running = sched && scheduler_is_running(sched);
    char pc[48];

    // A Mac OS system error (the bomb box, or a debugger break with one):
    // DSErrCode changes to the error's ID.  Those are positive; the start-up
    // code leaves negative status values there in passing (-14134, -1), and
    // the "Welcome to Macintosh" box goes through SysError as ID 40
    // (dsGreeting) on every boot.
    uint16_t ds = mac_read16(MAC_DS_ERRCODE);
    if (ds != g_watch.ds_err && (int16_t)ds > 0 && ds != MAC_DS_GREETING && g_watch.ticks_alive) {
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
    if (gap > g_perf.gap_max)
        g_perf.gap_max = gap;
    g_watch.last_poll_ms = now_ms;
    // The thread that polls is the emulator's: a late poll means it was
    // busy elsewhere that long (the page's 3 s heartbeat watch fires on this)
    if (gap >= WATCH_GAP_S * 1000.0) {
        if (g_phase.what)
            log_status("the emulator thread was blocked for %.1f s (longest: %s %.0f ms)", gap / 1000.0, g_phase.what,
                       g_phase.ms);
        else
            log_status("the emulator thread was blocked for %.1f s", gap / 1000.0);
    }
    g_phase.what = NULL;
    g_phase.ms = 0;
    if (!m)
        return;
    detect(m, now_ms);
    if (g_interval_s && now_ms - g_watch.last_report_ms >= g_interval_s * 1000.0)
        report(m, now_ms);
    if (g_perf_s) {
        if (!g_perf.primed || g_perf.machine != m)
            perf_take_baseline(m, now_ms);
        else if (now_ms - g_perf.ms >= g_perf_s * 1000.0 - 1.0)
            perf_report(m, now_ms);
    }
}

void watch_report_now(void) {
    double now_ms = host_time_ms();
    if (!g_watch.primed || system_running() != g_watch.machine)
        prime(system_running(), now_ms - 1.0); // fresh baselines: the rates cover the last millisecond
    report(g_watch.machine, now_ms);
}
