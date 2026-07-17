// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit test for the two-mode scheduler pacing model
// (local/gs-docs/completed/proposal-scheduler-two-modes.md §7).
//
// Links the real scheduler.c against a fake, test-controlled host clock and a
// stub CPU whose sprints complete instantly (optionally advancing the fake
// clock to simulate emulation cost). scheduler_main_loop is then driven with
// synthetic tick sequences, checking:
//
//   - paced accumulator: long-term frame-unit rate converges to the Mac VBL
//     rate at 60.00, 59.94 and 120 Hz host clocks, per-tick bursts never
//     exceed PACED_MAX_CATCHUP
//   - no-spiral: a sustained slow host pins the per-tick count at the cap and
//     the accumulated debt saturates (bounded catch-up after recovery)
//   - backgrounded tab: a >1 s gap resets instead of fast-forwarding
//   - turbo first tick: no NaN/UB with host_secs_per_vbl unset; batching
//     ramps up once the estimator has data
//   - mode switch: estimators reset (no turbo burst leaking into paced),
//     cpu_cycles stays monotonic
//   - one guest timeline: the same frame-unit count yields identical
//     cpu_cycles and instruction counts under jittered-paced, turbo, and
//     headless-style back-to-back execution — with a self-rescheduling event
//     in flight to exercise sprint clamping
//   - CPI is mode-independent and settable as a single per-machine constant
//
// Accelerated mode (proposal-scheduler-accelerated-mode.md, stage 1 — fixed
// multiplier via fractional effective CPI):
//   - timebase invariance: for a sweep of speed multipliers, N frame-units
//     advance cpu_cycles by (nearly) the paced amount and a cycle-timestamped
//     repeating event (the VIA-timer proxy) fires the same number of times,
//     while instruction throughput scales ~linearly with the multiplier
//   - instruction budgets stay exact at fractional CPIs, and the carried
//     sub-cycle remainder never loses cycles across back-to-back runs
//   - mode-switch hygiene: leaving accelerated restores the authentic
//     integer-CPI timeline exactly; the speed setting is inert in paced
//   - the speed setter clamps to [1x, 8x]
//   - pacing: accelerated shares the paced wall-clock accumulator (rate
//     converges to VBL_HZ, bursts capped)
//
// Adaptive governor (stage 2 — scheduler.speed = 0/auto; the stub CPU's
// per-instruction host cost closes the control loop, since faster speeds
// retire more instructions per frame and thus consume more fake host time):
//   - steady fast host: climbs the quantized ladder one rung at a time,
//     monotonically, respecting the dwell slew limit, and settles at the cap
//   - steady slow host: never leaves the authentic floor (degrades to paced)
//   - load spike: multiplicative back-off within a couple of ticks, settling
//     at the highest rung whose utilization clears the ceiling
//   - audio-ring pressure (optional signal) forces a back-off even at low
//     measured utilization; signal absence (< 0) is ignored
//   - max_speed caps both the governor and pinned speeds
//   - pin/unpin: writing a speed pins (governor off), 0 returns to auto and
//     restarts from the authentic floor

#include "object.h"
#include "scheduler.h"
#include "test_assert.h"
#include "value.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// Must match scheduler.c (not exported; the tests below pin the contract).
#define VBL_HZ            60.15
#define VBL_PERIOD        (1.0 / VBL_HZ)
#define PACED_MAX_CATCHUP 4
#define DEFAULT_CPI       12

// --- Fake host clock -------------------------------------------------------

static double g_now; // seconds; returned by host_time()
static double g_secs_per_instr; // simulated emulation cost (0 = free)

double host_time(void) {
    return g_now;
}

// --- Fake audio-ring signal (governor feedback) ----------------------------

static double g_audio_fill = -1.0; // < 0 = no signal (the default contract)

double platform_audio_ring_fill(void) {
    return g_audio_fill;
}

// --- Stubs ------------------------------------------------------------------

static scheduler_t *g_sched;
static uint64_t g_vbls; // frame-units executed (trigger_vbl calls)

scheduler_t *system_scheduler(void) {
    return g_sched;
}

typedef struct debug debug_t;
debug_t *system_debug(void) {
    return NULL;
}
bool debug_active(debug_t *debug) {
    (void)debug;
    return false;
}
int debug_break_and_trace(void) {
    return 0;
}

void trigger_vbl(config_t *restrict config) {
    (void)config;
    g_vbls++;
}

// Stub CPU: every sprint retires all its instructions instantly, advancing
// the fake host clock by the configured per-instruction cost.
void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions) {
    (void)cpu;
    g_now += (double)*instructions * g_secs_per_instr;
    *instructions = 0;
}
bool cpu_is_stopped(cpu_t *restrict cpu) {
    (void)cpu;
    return false;
}
void cpu_poll_interrupt(cpu_t *restrict cpu) {
    (void)cpu;
}

uint32_t g_io_penalty_remainder = 0;
uint32_t g_io_phantom_instructions = 0;
uint32_t g_io_cpi_x256 = 0;
uint32_t *g_sprint_burndown_ptr = NULL;
// E-clock sync state: normally defined in memory.c and written by scheduler.c
// (scheduler_set_frequency / the sprint loop). memory.c is not linked into
// this isolated scheduler suite, so stub the storage here.
uint64_t g_sprint_base_cycles = 0;
uint32_t g_sprint_frac_x256 = 0;
uint32_t g_sprint_total_slots = 0;
uint32_t g_esync_period_x256 = 0;

// Checkpointing is not exercised here (integration checkpoint tests cover it).
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *file, int line) {
    (void)checkpoint;
    (void)data;
    (void)size;
    (void)file;
    (void)line;
}
void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *file,
                                      int line) {
    (void)checkpoint;
    (void)data;
    (void)size;
    (void)file;
    (void)line;
}

// Object tree: the scheduler tolerates a NULL binding (object_new failure
// path), so the whole surface stubs to no-ops.
struct object *object_root(void) {
    return NULL;
}
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    (void)cls;
    (void)instance_data;
    (void)name;
    return NULL;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void object_detach(struct object *o) {
    (void)o;
}
void object_delete(struct object *o) {
    (void)o;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}

// Values: only referenced from the (never-dispatched) class descriptor.
value_t val_none(void) {
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}
value_t val_bool(bool b) {
    (void)b;
    return val_none();
}
value_t val_uint(uint8_t width, uint64_t u) {
    (void)width;
    (void)u;
    return val_none();
}
value_t val_str(const char *s) {
    (void)s;
    return val_none();
}
value_t val_float(double f) {
    (void)f;
    return val_none();
}
value_t val_err(const char *fmt, ...) {
    (void)fmt;
    return val_none();
}
void value_free(value_t *v) {
    (void)v;
}

// --- Helpers -----------------------------------------------------------------

static int g_dummy_cpu; // opaque non-NULL cpu pointer
static int g_dummy_cfg; // opaque non-NULL config pointer
#define TEST_CPU ((cpu_t *)&g_dummy_cpu)
#define TEST_CFG ((config_t *)&g_dummy_cfg)

// Self-rescheduling event: fires every 77,777 cycles forever, so sprints get
// clamped at event boundaries and the interleaving is non-trivial.
static uint64_t g_pings;
static void ping_event(void *source, uint64_t data) {
    (void)data;
    g_pings++;
    scheduler_new_cpu_event(g_sched, ping_event, source, 0, 77777, 0);
}

static scheduler_t *fresh_scheduler(bool with_ping) {
    g_now = 0.0;
    g_secs_per_instr = 0.0;
    g_audio_fill = -1.0;
    g_vbls = 0;
    g_pings = 0;
    scheduler_t *s = scheduler_init(TEST_CPU, NULL);
    ASSERT_TRUE(s != NULL);
    g_sched = s;
    if (with_ping) {
        scheduler_new_event_type(s, "test", &g_dummy_cfg, "ping", ping_event);
        scheduler_new_cpu_event(s, ping_event, &g_dummy_cfg, 0, 77777, 0);
    }
    return s;
}

static void teardown(scheduler_t *s) {
    scheduler_delete(s);
    g_sched = NULL;
}

// Advance the synthetic host clock to `now_s` and run one main-loop tick.
// Returns the number of frame-units the tick executed.
static int tick_at(double now_s) {
    uint64_t before = g_vbls;
    if (now_s > g_now)
        g_now = now_s; // keep host_time() >= the tick timestamps we feed
    scheduler_main_loop(TEST_CFG, now_s * 1000.0);
    return (int)(g_vbls - before);
}

// Drive a fixed-rate host clock for `ticks` ticks at `hz`, starting at
// `*now_s`. Returns the largest per-tick frame-unit count seen.
static int run_fixed_rate(double *now_s, double hz, int ticks) {
    int max_burst = 0;
    for (int i = 0; i < ticks; i++) {
        *now_s += 1.0 / hz;
        int n = tick_at(*now_s);
        if (n > max_burst)
            max_burst = n;
    }
    return max_burst;
}

// --- Tests --------------------------------------------------------------------

// Paced accumulator: long-term rate converges to VBL_HZ for common host
// refresh rates, and no tick ever bursts past PACED_MAX_CATCHUP.
static void paced_rate_at(double host_hz) {
    scheduler_t *s = fresh_scheduler(false);
    double now = 0.0;
    int ticks = (int)(100.0 * host_hz); // ~100 seconds of host time
    int max_burst = run_fixed_rate(&now, host_hz, ticks);

    double expected = now * VBL_HZ;
    double error = (double)g_vbls - expected;
    ASSERT_TRUE(fabs(error) <= 2.0);
    ASSERT_TRUE(max_burst <= PACED_MAX_CATCHUP);
    teardown(s);
}

TEST(test_paced_rate_60hz) {
    paced_rate_at(60.0);
}
TEST(test_paced_rate_5994hz) {
    paced_rate_at(59.94);
}
TEST(test_paced_rate_120hz) {
    paced_rate_at(120.0);
}

// Sustained slow host: per-tick count pins at the cap, and the banked debt
// saturates — after the host recovers, catch-up drains within a few ticks
// instead of bursting for the whole deficit.
TEST(test_paced_no_spiral) {
    scheduler_t *s = fresh_scheduler(false);
    double now = 0.0;

    // 5 seconds of a 10 Hz host (honest demand ~6 frame-units per tick).
    for (int i = 0; i < 50; i++) {
        now += 0.1;
        int n = tick_at(now);
        ASSERT_TRUE(n <= PACED_MAX_CATCHUP);
    }

    // Host recovers to 60 Hz: the saturated debt drains capped, and the
    // per-tick count settles to steady state within a handful of ticks.
    for (int i = 0; i < 10; i++) {
        now += 1.0 / 60.0;
        int n = tick_at(now);
        ASSERT_TRUE(n <= PACED_MAX_CATCHUP);
    }
    for (int i = 0; i < 20; i++) {
        now += 1.0 / 60.0;
        int n = tick_at(now);
        ASSERT_TRUE(n <= 2); // steady state again — debt was bounded
    }
    teardown(s);
}

// A >1 s host gap (backgrounded tab) resets: the gap tick runs nothing and
// the next normal tick doesn't fast-forward.
TEST(test_paced_background_reset) {
    scheduler_t *s = fresh_scheduler(false);
    double now = 0.0;
    run_fixed_rate(&now, 60.0, 120);

    now += 2.5; // tab in background for 2.5 s
    ASSERT_EQ_INT(tick_at(now), 0);

    now += 1.0 / 60.0;
    ASSERT_TRUE(tick_at(now) <= 2);
    teardown(s);
}

// Turbo first tick: host_secs_per_vbl starts NAN; the guard must produce a
// sane batch (>= 1) instead of converting NaN to int (UB). Once the
// estimator has data, batching ramps up beyond 1 frame-unit per tick.
TEST(test_turbo_first_tick_and_batching) {
    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_unthrottled);
    // Simulated emulation speed: ~2 ms of host time per frame-unit's worth
    // of instructions, so ~4 frame-units fit in half of a 60 Hz tick.
    g_secs_per_instr = 0.002 / 10852.0;

    double now = 0.0;
    now += 1.0 / 60.0;
    int first = tick_at(now);
    ASSERT_TRUE(first >= 1 && first <= PACED_MAX_CATCHUP * 4);

    int max_burst = 0;
    for (int i = 0; i < 200; i++) {
        // The fake clock advances during emulation; keep host ticks 16.7 ms
        // apart in host time (as a real RAF would).
        now = g_now + 1.0 / 60.0;
        int n = tick_at(now);
        if (n > max_burst)
            max_burst = n;
        ASSERT_TRUE(n >= 1);
    }
    ASSERT_TRUE(max_burst >= 2); // batching engaged
    ASSERT_TRUE(max_burst <= 32); // ...but stayed sane
    teardown(s);
}

// Mode switch hygiene: turbo's estimators must not leak into paced pacing,
// and cpu_cycles must stay monotonic across the switch.
TEST(test_mode_switch_estimator_reset) {
    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_unthrottled);
    g_secs_per_instr = 0.002 / 10852.0;

    double now = 0.0;
    for (int i = 0; i < 100; i++) {
        now = g_now + 1.0 / 60.0;
        tick_at(now);
    }

    uint64_t cycles_at_switch = scheduler_cpu_cycles(s);
    scheduler_set_mode(s, schedule_paced);
    g_secs_per_instr = 0.0;

    for (int i = 0; i < 50; i++) {
        now += 1.0 / 60.0;
        int n = tick_at(now);
        ASSERT_TRUE(n <= 2); // no burst carried over from turbo
        ASSERT_TRUE(scheduler_cpu_cycles(s) >= cycles_at_switch);
        cycles_at_switch = scheduler_cpu_cycles(s);
    }
    teardown(s);
}

// One guest timeline: with a self-rescheduling event in flight, the same
// number of frame-units produces identical cpu_cycles, instruction and event
// counts under (a) paced with a jittered host clock, (b) turbo, and (c)
// headless-style back-to-back scheduler_run_frame calls.
TEST(test_single_timeline_across_modes) {
    // (a) paced, jittery host clock (13/20/17 ms tick pattern)
    scheduler_t *s = fresh_scheduler(true);
    static const double jitter[3] = {0.013, 0.020, 0.017};
    double now = 0.0;
    int i = 0;
    while (g_vbls < 500) {
        now += jitter[i++ % 3];
        tick_at(now);
    }
    uint64_t frames = g_vbls; // may have overshot 500 inside a burst
    uint64_t paced_cycles = scheduler_cpu_cycles(s);
    uint64_t paced_instr = cpu_instr_count();
    uint64_t paced_pings = g_pings;
    teardown(s);

    // (b) turbo until the same frame count
    s = fresh_scheduler(true);
    scheduler_set_mode(s, schedule_unthrottled);
    g_secs_per_instr = 0.002 / 10852.0;
    now = 0.0;
    while (g_vbls < frames) {
        now = g_now + 1.0 / 60.0;
        tick_at(now);
        // A turbo batch may overshoot the target; compare on the actual
        // count instead and drop the (now mismatched) paced reference.
        if (g_vbls > frames) {
            frames = g_vbls;
            paced_cycles = 0; // flag: paced reference no longer comparable
        }
    }
    uint64_t turbo_cycles = scheduler_cpu_cycles(s);
    uint64_t turbo_instr = cpu_instr_count();
    uint64_t turbo_pings = g_pings;
    teardown(s);

    // (c) headless-style: exactly `frames` back-to-back frame-units
    s = fresh_scheduler(true);
    for (uint64_t f = 0; f < frames; f++)
        scheduler_run_frame(s, TEST_CFG);
    uint64_t headless_cycles = scheduler_cpu_cycles(s);
    uint64_t headless_instr = cpu_instr_count();
    uint64_t headless_pings = g_pings;
    teardown(s);

    ASSERT_TRUE(headless_cycles == turbo_cycles);
    ASSERT_TRUE(headless_instr == turbo_instr);
    ASSERT_TRUE(headless_pings == turbo_pings);
    if (paced_cycles != 0) { // no turbo overshoot: paced ran the same count
        ASSERT_TRUE(headless_cycles == paced_cycles);
        ASSERT_TRUE(headless_instr == paced_instr);
        ASSERT_TRUE(headless_pings == paced_pings);
    }
}

// CPI is a single, mode-independent constant: N instructions always advance
// cpu_cycles by N * CPI, in either mode, and scheduler_set_cpi rebases it.
TEST(test_cpi_mode_independent) {
    scheduler_t *s = fresh_scheduler(false);

    uint64_t c0 = scheduler_cpu_cycles(s);
    scheduler_run_instructions(s, 1000);
    ASSERT_TRUE(scheduler_cpu_cycles(s) - c0 == 1000ULL * DEFAULT_CPI);

    scheduler_set_mode(s, schedule_unthrottled);
    c0 = scheduler_cpu_cycles(s);
    scheduler_run_instructions(s, 1000);
    ASSERT_TRUE(scheduler_cpu_cycles(s) - c0 == 1000ULL * DEFAULT_CPI);

    scheduler_set_cpi(s, 4);
    c0 = scheduler_cpu_cycles(s);
    scheduler_run_instructions(s, 1000);
    ASSERT_TRUE(scheduler_cpu_cycles(s) - c0 == 1000ULL * 4);

    teardown(s);
}

// --- Accelerated mode (fixed multiplier via fractional effective CPI) --------

// Timebase invariance — the core correctness property of accelerated mode:
// the cycle timebase (which VIA φ2, sound and SCC divide down from) must not
// move with the speed multiplier. For a sweep of speeds, N frame-units advance
// cpu_cycles by the paced amount within one authentic-CPI instruction per
// frame of conversion truncation, and the self-rescheduling 77,777-cycle event
// (the VIA-timer proxy) fires the same number of times (±1) — while retired
// instructions scale ~linearly with the multiplier.
TEST(test_accelerated_timebase_invariant) {
    const int frames = 400;

    // Reference: authentic paced run
    scheduler_t *s = fresh_scheduler(true);
    for (int f = 0; f < frames; f++)
        scheduler_run_frame(s, TEST_CFG);
    uint64_t ref_cycles = scheduler_cpu_cycles(s);
    uint64_t ref_instr = cpu_instr_count();
    uint64_t ref_pings = g_pings;
    teardown(s);

    static const double speeds[] = {1.0, 2.0, 3.0, 4.0, 8.0};
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        s = fresh_scheduler(true);
        scheduler_set_mode(s, schedule_accelerated);
        scheduler_set_speed(s, speeds[i]);
        for (int f = 0; f < frames; f++)
            scheduler_run_frame(s, TEST_CFG);

        // Timebase: total cycles within one instruction's truncation per frame
        int64_t drift = (int64_t)scheduler_cpu_cycles(s) - (int64_t)ref_cycles;
        int64_t tol = (int64_t)frames * DEFAULT_CPI;
        ASSERT_TRUE(drift >= -tol && drift <= tol);

        // Event cadence: the cycle-timestamped event stays real-time
        int64_t dpings = (int64_t)g_pings - (int64_t)ref_pings;
        ASSERT_TRUE(dpings >= -1 && dpings <= 1);

        // Throughput: instructions per frame scale with the multiplier
        double ratio = (double)cpu_instr_count() / (double)ref_instr;
        ASSERT_TRUE(ratio > speeds[i] * 0.99 && ratio < speeds[i] * 1.01);
        teardown(s);
    }
}

// Instruction budgets stay exact at a fractional CPI, and the carried
// sub-cycle remainder never loses cycles: 10 back-to-back 1000-instruction
// runs at 5x (effective CPI 12*65536/1280 = 614 x256 = 2.3984...) retire
// exactly 10,000 instructions and advance exactly floor(10000*614/256) cycles
// (the remainder telescopes across sprint and run boundaries).
TEST(test_accelerated_budget_exact) {
    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    scheduler_set_speed(s, 5.0); // deliberately non-dyadic: exercises the carry
    const uint64_t eff_x256 = (12ULL << 16) / (5 * 256); // = 614

    uint64_t i0 = cpu_instr_count();
    uint64_t c0 = scheduler_cpu_cycles(s);
    for (int k = 0; k < 10; k++)
        scheduler_run_instructions(s, 1000);
    ASSERT_TRUE(cpu_instr_count() - i0 == 10000);
    ASSERT_TRUE(scheduler_cpu_cycles(s) - c0 == (10000 * eff_x256) >> 8);
    teardown(s);
}

// Mode-switch hygiene: leaving accelerated clears the fractional carry and
// restores the authentic timeline — a paced frame is pure integer CPI again —
// and the speed setting is inert while paced.
TEST(test_accelerated_mode_switch_hygiene) {
    scheduler_t *s = fresh_scheduler(false);

    // Authentic per-frame instruction count (paced, speed setting present)
    scheduler_set_speed(s, 8.0); // must be inert outside accelerated
    uint64_t i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    uint64_t paced_instr = cpu_instr_count() - i0;

    // Accelerated at 8x: ~8x the instructions per frame
    scheduler_set_mode(s, schedule_accelerated);
    i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    uint64_t accel_instr = cpu_instr_count() - i0;
    ASSERT_TRUE(accel_instr > paced_instr * 7 && accel_instr < paced_instr * 9);

    // Back to paced: per-frame count returns to authentic exactly, and the
    // frame advances instructions * CPI cycles (no leftover fractional carry)
    scheduler_set_mode(s, schedule_paced);
    i0 = cpu_instr_count();
    uint64_t c0 = scheduler_cpu_cycles(s);
    scheduler_run_frame(s, TEST_CFG);
    uint64_t di = cpu_instr_count() - i0;
    ASSERT_TRUE(di == paced_instr);
    ASSERT_TRUE(scheduler_cpu_cycles(s) - c0 == di * DEFAULT_CPI);
    teardown(s);
}

// The speed setter clamps to [1x, 8x]: below-floor requests run authentic,
// above-cap requests run at exactly the 8x cap.
TEST(test_accelerated_speed_clamp) {
    // Authentic reference frame
    scheduler_t *s = fresh_scheduler(false);
    uint64_t i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    uint64_t ref = cpu_instr_count() - i0;
    teardown(s);

    // 0.25x clamps up to the 1x floor (never slower than real hardware)
    s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    scheduler_set_speed(s, 0.25);
    i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    ASSERT_TRUE(cpu_instr_count() - i0 == ref);
    teardown(s);

    // 100x clamps down to the 8x cap
    s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    scheduler_set_speed(s, 100.0);
    i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    double ratio = (double)(cpu_instr_count() - i0) / (double)ref;
    ASSERT_TRUE(ratio > 8.0 * 0.99 && ratio < 8.0 * 1.01);
    teardown(s);
}

// Accelerated shares the paced wall-clock accumulator: the frame-unit rate
// converges to the Mac VBL rate and per-tick bursts stay capped — the speed
// multiplier must never change how many frame-units a host tick earns.
TEST(test_accelerated_paced_pacing) {
    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    scheduler_set_speed(s, 4.0);

    double now = 0.0;
    int ticks = (int)(100.0 * 60.0); // ~100 seconds of a 60 Hz host
    int max_burst = run_fixed_rate(&now, 60.0, ticks);

    double expected = now * VBL_HZ;
    ASSERT_TRUE(fabs((double)g_vbls - expected) <= 2.0);
    ASSERT_TRUE(max_burst <= PACED_MAX_CATCHUP);
    teardown(s);
}

// --- Adaptive governor (stage 2) ---------------------------------------------

// Authentic instructions per frame-unit, measured (12 CPI at 7.8336 MHz).
static uint64_t authentic_per_frame(void) {
    scheduler_t *s = fresh_scheduler(false);
    uint64_t i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    uint64_t pf = cpu_instr_count() - i0;
    teardown(s);
    return pf;
}

// Drive `ticks` 60 Hz main-loop ticks against the governor, tracking the
// per-frame instruction count (the observable for the current speed rung).
// Asserts every change is a single monotonic step when `expect_monotonic`,
// and records the smallest host-time gap between consecutive changes.
typedef struct {
    uint64_t final_pf; // per-frame instructions after the last clean sample
    int changes; // number of distinct per-frame-count transitions
    double min_gap; // smallest host-seconds gap between transitions
    uint64_t max_pf; // largest per-frame count ever observed
} gov_trace_t;

static gov_trace_t gov_drive(scheduler_t *s, double *now, int ticks, bool expect_monotonic) {
    gov_trace_t t = {0, 0, 1e9, 0};
    uint64_t prev_instr = cpu_instr_count();
    uint64_t prev_vbls = g_vbls;
    double last_change = *now;
    for (int i = 0; i < ticks; i++) {
        *now += 1.0 / 60.0;
        tick_at(*now);
        uint64_t vd = g_vbls - prev_vbls;
        uint64_t id = cpu_instr_count() - prev_instr;
        prev_vbls = g_vbls;
        prev_instr = cpu_instr_count();
        if (vd != 1)
            continue; // only clean single-frame ticks give a per-frame sample
        if (id > t.max_pf)
            t.max_pf = id;
        if (t.final_pf != 0 && id != t.final_pf) {
            double gap = *now - last_change;
            if (gap < t.min_gap)
                t.min_gap = gap;
            if (expect_monotonic)
                ASSERT_TRUE(id > t.final_pf);
            t.changes++;
            last_change = *now;
        } else if (t.final_pf == 0) {
            last_change = *now;
        }
        t.final_pf = id;
    }
    return t;
}

// Steady fast host: the governor climbs the ladder one rung at a time —
// monotonically, each step after at least the dwell time — and settles at
// the 8x cap without ever overshooting it.
TEST(test_governor_climbs_to_cap) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated); // speed defaults to auto
    // 1x utilization ~0.05: plenty of headroom all the way to 8x (~0.40)
    g_secs_per_instr = 0.05 * VBL_PERIOD / (double)pf1;

    double now = 0.0;
    gov_trace_t t = gov_drive(s, &now, 25 * 60, true);

    ASSERT_TRUE(t.changes == 6); // rung 0 -> 6, one step at a time
    ASSERT_TRUE(t.min_gap > 1.8); // dwell slew limit respected (2 s nominal)
    double ratio = (double)t.final_pf / (double)pf1;
    ASSERT_TRUE(ratio > 7.9 && ratio < 8.1); // settled at the cap
    ASSERT_TRUE((double)t.max_pf / (double)pf1 < 8.1); // never above it
    teardown(s);
}

// Steady slow host: utilization at 1x already breaches the ceiling, so the
// governor never leaves the authentic floor — accelerated degrades to paced.
TEST(test_governor_slow_host_stays_authentic) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    g_secs_per_instr = 0.95 * VBL_PERIOD / (double)pf1; // 1x utilization ~0.95

    double now = 0.0;
    gov_trace_t t = gov_drive(s, &now, 10 * 60, true);

    ASSERT_TRUE(t.changes == 0); // never climbed
    ASSERT_TRUE(t.final_pf == pf1); // authentic instructions per frame
    teardown(s);
}

// Load spike: after climbing on a fast host, a 4x cost spike drives
// utilization far past the ceiling; the governor backs off within a couple
// of ticks per rung — no dwell on the way down — and settles at the highest
// rung whose utilization clears the ceiling (3x here: 0.06 * 4 * 3 = 0.72).
TEST(test_governor_spike_backoff) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    g_secs_per_instr = 0.06 * VBL_PERIOD / (double)pf1;

    double now = 0.0;
    gov_trace_t t = gov_drive(s, &now, 25 * 60, true);
    ASSERT_TRUE((double)t.final_pf / (double)pf1 > 7.9); // reached the cap

    g_secs_per_instr *= 4.0; // spike: 8x now costs ~1.92 of the budget
    gov_trace_t back = gov_drive(s, &now, 12 * 60, false);
    double settled = (double)back.final_pf / (double)pf1;
    ASSERT_TRUE(settled > 2.9 && settled < 3.1); // backed off to 3x
    ASSERT_TRUE(back.changes >= 3); // stepped down through the rungs
    teardown(s);
}

// Audio-ring pressure: a draining host ring forces a back-off even when the
// measured utilization is comfortable; a recovered ring lets it climb again.
TEST(test_governor_audio_pressure) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    g_secs_per_instr = 0.05 * VBL_PERIOD / (double)pf1;

    double now = 0.0;
    gov_trace_t t = gov_drive(s, &now, 7 * 60, true);
    ASSERT_TRUE(t.final_pf > pf1); // climbed at least one rung

    uint64_t before = t.final_pf;
    g_audio_fill = 0.2; // ring draining: pressure regardless of utilization
    gov_trace_t drop = gov_drive(s, &now, 3 * 60, false);
    ASSERT_TRUE(drop.final_pf < before); // backed off

    g_audio_fill = 1.0; // ring healthy again: climbing resumes
    gov_trace_t re = gov_drive(s, &now, 10 * 60, false);
    ASSERT_TRUE(re.final_pf > drop.final_pf);
    teardown(s);
}

// max_speed caps the governor's ceiling — and clamps pinned speeds too.
TEST(test_governor_max_speed_cap) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);
    scheduler_set_max_speed(s, 3.0);
    g_secs_per_instr = 0.05 * VBL_PERIOD / (double)pf1;

    double now = 0.0;
    gov_trace_t t = gov_drive(s, &now, 15 * 60, true);
    double ratio = (double)t.final_pf / (double)pf1;
    ASSERT_TRUE(ratio > 2.9 && ratio < 3.1); // settled exactly at the 3x cap
    ASSERT_TRUE((double)t.max_pf / (double)pf1 < 3.1); // never above it

    // A pinned speed is clamped to the cap as well
    scheduler_set_speed(s, 8.0);
    uint64_t i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    double pinned = (double)(cpu_instr_count() - i0) / (double)pf1;
    ASSERT_TRUE(pinned > 2.9 && pinned < 3.1);
    teardown(s);
}

// Pin/unpin: auto starts from the authentic floor; a written speed pins the
// multiplier immediately; writing 0 returns to auto at the floor again.
TEST(test_governor_pin_unpin) {
    uint64_t pf1 = authentic_per_frame();

    scheduler_t *s = fresh_scheduler(false);
    scheduler_set_mode(s, schedule_accelerated);

    // Auto, no main-loop ticks yet: the governor sits at the authentic floor
    uint64_t i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    ASSERT_TRUE(cpu_instr_count() - i0 == pf1);

    // Pin 4x: takes effect immediately, no governor involved
    scheduler_set_speed(s, 4.0);
    i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    double pinned = (double)(cpu_instr_count() - i0) / (double)pf1;
    ASSERT_TRUE(pinned > 3.9 && pinned < 4.1);

    // Unpin (0 = auto): back to the floor until the governor earns headroom
    scheduler_set_speed(s, 0.0);
    i0 = cpu_instr_count();
    scheduler_run_frame(s, TEST_CFG);
    ASSERT_TRUE(cpu_instr_count() - i0 == pf1);
    teardown(s);
}

int main(void) {
    RUN(test_paced_rate_60hz);
    RUN(test_paced_rate_5994hz);
    RUN(test_paced_rate_120hz);
    RUN(test_paced_no_spiral);
    RUN(test_paced_background_reset);
    RUN(test_turbo_first_tick_and_batching);
    RUN(test_mode_switch_estimator_reset);
    RUN(test_single_timeline_across_modes);
    RUN(test_cpi_mode_independent);
    RUN(test_accelerated_timebase_invariant);
    RUN(test_accelerated_budget_exact);
    RUN(test_accelerated_mode_switch_hygiene);
    RUN(test_accelerated_speed_clamp);
    RUN(test_accelerated_paced_pacing);
    RUN(test_governor_climbs_to_cap);
    RUN(test_governor_slow_host_stays_authentic);
    RUN(test_governor_spike_backoff);
    RUN(test_governor_audio_pressure);
    RUN(test_governor_max_speed_cap);
    RUN(test_governor_pin_unpin);
    fprintf(stderr, "[OK  ] scheduler suite passed\n");
    return 0;
}
