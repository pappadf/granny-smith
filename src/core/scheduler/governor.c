// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// governor.c
// The adaptive speed governor (paced and accelerated with scheduler.speed =
// auto). AIMD on a quantized speed ladder: additive rung-up only after
// dwelling at the current speed (the slew limit — instruction-counted guest
// delays calibrated at one speed must not be replayed at a glided-away one),
// multiplicative rung-down (each rung is ~x1.5) as soon as sustained
// utilization threatens the real-time deadline. Utilization = host seconds
// spent emulating one frame-unit / MAC_VBL_PERIOD; overrunning it stalls the
// paced accumulator, which surfaces as audio underruns and stutter — hence
// the headroom target.  Below 1x the same loop is what keeps a host that
// cannot emulate a whole frame-unit per VBL period on the real-time clock:
// fewer instructions per frame-unit, the frame-units themselves on time.
// The scheduler turns the rung into an effective CPI (scheduler.c,
// scheduler_update_cpi_eff); nothing here touches emulated time.

#include "governor.h"

#include "gs_assert.h"

#include <stddef.h>

// ============================================================================
// Tuning
// ============================================================================

#define GOV_UTIL_TARGET     0.85 // climb (or land a back-off) only where the *projected* utilization stays below this
#define GOV_UTIL_CEILING    0.90 // no climb attempts while the smoothed utilization sits above this
#define GOV_DWELL_SECS      2.0 // minimum residence at a rung before climbing, at and above 1x
#define GOV_DWELL_SLOW_SECS 0.5 // ...and below 1x, where getting back to the authentic speed is the point
#define GOV_HOLDOFF_SECS    1.0 // after a back-off, no climb attempts for this long
#define GOV_AUDIO_LOW       0.5 // audio ring below half its target depth = draining (optional signal)
// A draining ring blocks climbing at once, but backs the guest off only once
// it has stayed below for this long: the ring also sits below half while it
// fills from empty at a sound's start and while it drains at a sound's end
// (the signal stays live for half a second after the last push), neither of
// which a slower guest CPU would help.  A real overload shows up first in
// the accumulator anyway; this is the safety net behind it.
#define GOV_AUDIO_SECS 0.75
// Utilization EWMA gains: pressure registers fast, relief a little slower.
#define GOV_EWMA_UP   0.35
#define GOV_EWMA_DOWN 0.25
// Largest utilization one sample may feed the EWMA.  A sample past this says
// only "far over" -- the back-off lands by projection, and 0.85 / 2 already
// lands below the lowest rung -- and unclipped, one hitch drove the EWMA so
// high that it took the decay many ticks to come back under the ceiling.
#define GOV_UTIL_SAMPLE_MAX 2.0
// A back-off needs the host to be *behind* real time: the paced accumulator
// capped at PACED_MAX_CATCHUP with the debt still growing on this many
// consecutive governor ticks, or for this long of consecutive host time --
// whichever comes first.  One capped
// tick is a hitch the next tick or two repay -- the JIT translating a fresh
// page, a disk burst; three in a row, at the 50-100 ms a capped tick lasts
// on a somewhat overloaded host, is a quarter second of lagging, which is
// what breaks sound up; on a badly overloaded host (the interpreter in
// early boot) a capped tick alone lasts several hundred ms, and the time
// bound keeps the reaction at a fraction of a second there too.  Being
// behind counts only while the frame loop's own cost is a real share of the
// budget: a host whose *ticks* are slow (a 10 Hz RAF, a throttled tab) caps
// the accumulator too, and a slower guest CPU helps nothing there.
// When the frame loop *alone* is already over the budget (EWMA above
// GOV_UTIL_OVER), the first behind tick is enough: that is not a hitch the
// next bursts repay but the host unable to fit even one frame-unit per VBL
// -- the interpreter under the boot chime -- and every further tick at this
// speed is audible.  A lone over-budget tick does not get the EWMA there
// (0.3 -> 0.9 on a clipped sample), two in a row with the accumulator still
// catching up do not count as behind.
#define GOV_BACKOFF_TICKS 3
#define GOV_BACKOFF_SECS  0.25
#define GOV_UTIL_RELEVANT 0.5
#define GOV_UTIL_OVER     1.0

// Quantized speed steps (x256): 0.25x, 0.375x, 0.5x, 0.625x, 0.75x, 0.875x,
// 1x, 1.5x, 2x, 3x, 4x, 6x, 8x -- every value exact in x256.  Eighths below
// 1x, so a host that falls short runs the guest close to what it can do;
// ~1.33-1.5 apart above it.  Rung GOV_RUNG_ONE is the authentic speed: where
// every reset starts, paced mode's ceiling, and the floor unless the host
// falls behind.
static const uint32_t gov_ladder_x256[] = {64, 96, 128, 160, 192, 224, 256, 384, 512, 768, 1024, 1536, 2048};
#define GOV_NUM_RUNGS ((int)(sizeof(gov_ladder_x256) / sizeof(gov_ladder_x256[0])))
#define GOV_RUNG_ONE  6 // the ladder's ends are SPEED_X256_MIN / _MAX and this rung 1x (checked in governor_reset)

// ============================================================================
// Controller
// ============================================================================

bool governor_active(const host_pacing_t *pacing) {
    GS_ASSERT(pacing != NULL);
    return pacing->speed_x256 == SPEED_X256_AUTO &&
           (pacing->mode == schedule_paced || pacing->mode == schedule_accelerated);
}

// The governor's range on the ladder under the current pacing: the lowest
// rung at or above the floor, and the highest rung at or below the cap --
// the cap being 1x in paced mode, whose CPU never runs faster than the
// original.  lo <= GOV_RUNG_ONE <= hi always holds (the floor is clamped to
// at most 1x, the cap to at least 1x).
static void gov_rung_bounds(const host_pacing_t *pacing, int *lo, int *hi) {
    int l = GOV_RUNG_ONE;
    while (l > 0 && gov_ladder_x256[l - 1] >= pacing->min_speed_x256)
        l--;
    int h = GOV_RUNG_ONE;
    if (pacing->mode == schedule_accelerated)
        while (h + 1 < GOV_NUM_RUNGS && gov_ladder_x256[h + 1] <= pacing->max_speed_x256)
            h++;
    *lo = l;
    *hi = h;
}

void governor_reset(governor_t *g) {
    GS_ASSERT(g != NULL);
    // The ladder's ends and its 1x rung are what SPEED_X256_MIN / _MAX and
    // GOV_RUNG_ONE assume
    GS_ASSERT(gov_ladder_x256[GOV_RUNG_ONE] == SPEED_X256_ONE && gov_ladder_x256[0] == SPEED_X256_MIN &&
              gov_ladder_x256[GOV_NUM_RUNGS - 1] == SPEED_X256_MAX);
    g->rung = GOV_RUNG_ONE;
    g->util_ewma = 0.0;
    g->dwell_secs = 0.0;
    g->holdoff_secs = 0.0;
    g->pressure_ticks = 0;
    g->pressure_secs = 0.0;
    g->audio_secs = 0.0;
    g->primed = false;
}

uint32_t governor_speed_x256(const governor_t *g) {
    GS_ASSERT(g != NULL);
    GS_ASSERT(g->rung >= 0 && g->rung < GOV_NUM_RUNGS);
    return gov_ladder_x256[g->rung];
}

// AIMD on the quantized ladder:
//   - back off once the host has fallen behind on GOV_BACKOFF_TICKS
//     consecutive ticks or for GOV_BACKOFF_SECS of consecutive host time
//     while the frame loop itself is a real share of the budget (EWMA above
//     GOV_UTIL_RELEVANT) -- on the first such tick where the frame loop
//     alone is over the budget (above GOV_UTIL_OVER) -- or the audio ring
//     has drained for GOV_AUDIO_SECS,
//     straight to the highest rung the measurement projects under the target
//     -- the multiplicative decrease that keeps a spike from becoming a
//     spiral, without a lone hitch costing anything;
//   - climb only after the dwell (GOV_DWELL_SECS at and above 1x,
//     GOV_DWELL_SLOW_SECS below it), outside the post-back-off holdoff, with
//     no pressure and the EWMA under the ceiling, and only as far as the
//     utilization *projected* at the rung climbed to stays under the target
//     -- the slew limit required so instruction-counted guest delays see a
//     stable speed, plus headroom.
// Below 1x a climb goes straight to the highest rung projected under the
// target, up to the authentic speed: the governor *starts* at the floor on
// its first tick (a fresh boot, a pacing change) and the first dwell there
// shows what fits, so a host that keeps up is at 1x after one dwell and a
// host that cannot never ran 1x at all -- the boot chime on an interpreter
// is the case: at 1x the first bursts run late before anything is known.
// Above 1x it stays one rung per dwell.  The ladder's resting rung is 1x
// until that first tick, which is what keeps the headless pump (no main
// loop, no governor ticks) at the authentic CPI.
// The utilization EWMA is rescaled on every step (utilization is proportional
// to instructions per frame), so the estimator stays meaningful across steps.
bool governor_tick(governor_t *g, const host_pacing_t *pacing, double host_secs_this_vbl, double elapsed_secs,
                   bool behind, double audio_fill) {
    GS_ASSERT(g != NULL);
    GS_ASSERT(governor_active(pacing));
    GS_ASSERT(g->rung >= 0 && g->rung < GOV_NUM_RUNGS);
    int from_rung = g->rung;

    // The rungs the floor and the cap allow under this pacing
    int min_rung, max_rung;
    gov_rung_bounds(pacing, &min_rung, &max_rung);

    if (!g->primed) {
        // First tick since the reset: drop to the floor and let the first
        // dwell show what fits.  The sample just taken was at the resting
        // rung; scale it to the one the estimator now tracks.
        g->primed = true;
        if (min_rung != g->rung) {
            host_secs_this_vbl *= (double)gov_ladder_x256[min_rung] / gov_ladder_x256[g->rung];
            g->rung = min_rung;
            g->dwell_secs = 0.0;
        }
    }

    // Utilization of the real-time frame budget, smoothed. Pressure registers
    // fast (protect the deadline); relief a little slower.
    double u = host_secs_this_vbl / MAC_VBL_PERIOD;
    if (u > GOV_UTIL_SAMPLE_MAX)
        u = GOV_UTIL_SAMPLE_MAX;
    double alpha = (u > g->util_ewma) ? GOV_EWMA_UP : GOV_EWMA_DOWN;
    g->util_ewma += alpha * (u - g->util_ewma);

    g->dwell_secs += elapsed_secs;
    if (g->holdoff_secs > 0.0)
        g->holdoff_secs -= elapsed_secs;

    // Optional audio feedback: a draining ring means the deadline is already
    // being missed where it hurts first.
    bool audio_low = (audio_fill >= 0.0 && audio_fill < GOV_AUDIO_LOW);
    g->audio_secs = audio_low ? g->audio_secs + elapsed_secs : 0.0;

    // Pressure = behind real time with the frame loop a real share of the
    // budget (a slow host *tick* with a cheap frame loop is not the guest's
    // fault), or the audio ring drained for long enough to be overload rather
    // than a sound starting or ending. Counted in consecutive ticks so a lone
    // hitch never triggers, and in host time so a badly overloaded host (few,
    // long ticks) is answered just as promptly.
    bool pressure = (behind && g->util_ewma > GOV_UTIL_RELEVANT) || g->audio_secs >= GOV_AUDIO_SECS;
    g->pressure_ticks = pressure ? g->pressure_ticks + 1 : 0;
    g->pressure_secs = pressure ? g->pressure_secs + elapsed_secs : 0.0;

    int new_rung = g->rung;
    bool over = behind && g->util_ewma > GOV_UTIL_OVER; // the frame loop alone does not fit
    if ((g->pressure_ticks >= GOV_BACKOFF_TICKS || g->pressure_secs >= GOV_BACKOFF_SECS || over) &&
        new_rung > min_rung) {
        // Back off to the highest rung projected under the target -- at
        // least one rung, since the host said this one does not fit
        new_rung--;
        while (new_rung > min_rung &&
               g->util_ewma * (double)gov_ladder_x256[new_rung] / gov_ladder_x256[g->rung] >= GOV_UTIL_TARGET)
            new_rung--;
        g->pressure_ticks = 0;
        g->pressure_secs = 0.0;
        g->audio_secs = 0.0;
        g->holdoff_secs = GOV_HOLDOFF_SECS;
    } else if (!pressure && !audio_low && g->util_ewma < GOV_UTIL_CEILING && new_rung < max_rung &&
               g->holdoff_secs <= 0.0 &&
               g->dwell_secs >= (new_rung < GOV_RUNG_ONE ? GOV_DWELL_SLOW_SECS : GOV_DWELL_SECS)) {
        // Climb only with headroom at the rung climbed to, not just the
        // current one: below 1x as far as the authentic speed in one step,
        // above it one rung
        int reach = new_rung < GOV_RUNG_ONE ? GOV_RUNG_ONE : new_rung + 1;
        if (reach > max_rung)
            reach = max_rung;
        for (int r = new_rung + 1; r <= reach; r++) {
            if (g->util_ewma * (double)gov_ladder_x256[r] / gov_ladder_x256[g->rung] >= GOV_UTIL_TARGET)
                break;
            new_rung = r;
        }
    }
    if (new_rung > max_rung)
        new_rung = max_rung; // cap lowered mid-run
    if (new_rung < min_rung)
        new_rung = min_rung; // floor raised mid-run

    if (new_rung != g->rung) {
        // Rescale the estimator to the new speed and require a fresh dwell
        g->util_ewma *= (double)gov_ladder_x256[new_rung] / gov_ladder_x256[g->rung];
        g->rung = new_rung;
        g->dwell_secs = 0.0;
    }
    return g->rung != from_rung;
}
