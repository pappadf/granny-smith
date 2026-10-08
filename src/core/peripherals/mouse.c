// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mouse.c
// Implements Macintosh Plus mouse quadrature signal generation for the SCC (X1/Y1) and VIA (X2/Y2).

#include "mouse.h"
#include "adb.h"
#include "cpu.h"
#include "debug_mac.h"
#include "log.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mouse");

// The Plus mouse is a ball turning two slotted wheels, one per axis; each
// wheel's photo-interrupters give a quadrature pair, X1/X2 and Y1/Y2, and
// every edge of X1 or Y1 is a DCD interrupt on the SCC whose handler moves
// MTemp one count (the ROM's P_SCCInt_AChng_MouseH / BChng_MouseV).  An edge
// marks a fixed distance travelled, so the edges come as fast as the mouse
// moves: a slow drag spaces them out, a fast swipe packs them close.
//
// The host reports motion in batches -- so many counts since the last batch,
// in practice once per emulated frame -- and the model plays each batch the
// way the wheels would have: its counts spread evenly over the time the
// motion took (the interval since the previous batch, at most a frame).  A
// batch is played out by the time the next arrives, so the cursor follows the
// hand a frame behind and never further, at any speed.  Each axis is one
// pulse train: the counts still to go (signed: the direction) and the gap
// between its edges.  A new batch joins what is left and re-spreads it.
struct mouse {
    bool x1, y1; // Current interrupt line logic levels (SCC DCD inputs)
    int32_t pending_x; // counts still to play on each axis (signed)
    int32_t pending_y;
    uint64_t gap_ns_x; // the time between edges of the current train
    uint64_t gap_ns_y;
    double last_batch_ns; // when the previous host batch arrived
    double window_end_ns; // when the current batch is to be played out
    int8_t scale_rem_x; // Carried host-delta remainders from the 2:1 scaling,
    int8_t scale_rem_y; // so small deltas keep their X:Y ratio across events

    /* Pointers last */
    struct scheduler *scheduler; // Event scheduler (CPU-cycle aligned)
    scc_t *scc; // SCC for DCD (X1/Y1) updates
    via_t *via; // VIA for quadrature + button inputs
};

// The closest two edges on one axis may come.  Each is an interrupt, and an
// edge that lands before the handler has read the last one toggles DCD again
// unseen: the count is lost.  The ROM's handler with the SCC's interrupt
// dispatch takes a few hundred cycles; edges 500 cycles apart were enough to
// break up MusicWorks playback, so a train keeps ~1,000 cycles (at the Plus's
// 7.83 MHz) between edges -- ~7,800 counts/s, three times a violently swung
// real mouse.  Only a batch faster than that runs past its frame.
#define MOUSE_MIN_EDGE_NS 128000ULL

#define AXIS_X 0
#define AXIS_Y 1

// Emit one edge on an axis, in the direction `positive`, and set the
// quadrature (X2/Y2) bit that tells the handler which way it went.
static void mouse_edge(mouse_t *m, int axis, bool positive) {
    if (axis == AXIS_X) {
        // Toggle X1 edge (rising or falling depending on previous state)
        m->x1 = !m->x1;
        // For right motion X2 follows X1; for left motion X2 is inverted relative to X1
        bool x2 = positive ? m->x1 : !m->x1;
        scc_dcd(m->scc, 0, m->x1); // Update SCC DCD A (X1)
        via_input(m->via, 1, 4, x2); // Update VIA PB4 (X2)
    } else {
        // Toggle Y1 edge
        m->y1 = !m->y1;
        // For down motion Y2 is opposite Y1; for up motion Y2 equals Y1 (see table)
        bool y2 = positive ? !m->y1 : m->y1;
        scc_dcd(m->scc, 1, m->y1); // Update SCC DCD B (Y1)
        via_input(m->via, 1, 5, y2); // Update VIA PB5 (Y2)
    }
}

// The next edge of an axis's pulse train; `data` is the axis.
static void mouse_train_event(void *source, uint64_t data) {
    mouse_t *m = source;
    int axis = (int)data;
    int32_t *pending = axis == AXIS_X ? &m->pending_x : &m->pending_y;
    if (*pending == 0)
        return;
    bool positive = *pending > 0;
    mouse_edge(m, axis, positive);
    *pending += positive ? -1 : 1;
    if (*pending != 0)
        scheduler_new_cpu_event(m->scheduler, &mouse_train_event, m, (uint64_t)axis, 0,
                                axis == AXIS_X ? m->gap_ns_x : m->gap_ns_y);
}

// Spread an axis's remaining counts evenly over the time left in the window:
// the first edge one gap from now (half a gap for Y, so the axes' edges do
// not coincide), the last at the window's end.
static void mouse_train_start(mouse_t *m, int axis, double now_ns) {
    int32_t pending = axis == AXIS_X ? m->pending_x : m->pending_y;
    remove_event_by_data(m->scheduler, &mouse_train_event, m, (uint64_t)axis);
    if (pending == 0)
        return;
    uint32_t steps = (uint32_t)(pending < 0 ? -pending : pending);
    double window = m->window_end_ns - now_ns;
    uint64_t gap = window > 0 ? (uint64_t)(window / steps) : 0;
    if (gap < MOUSE_MIN_EDGE_NS)
        gap = MOUSE_MIN_EDGE_NS;
    if (axis == AXIS_X)
        m->gap_ns_x = gap;
    else
        m->gap_ns_y = gap;
    uint64_t first = axis == AXIS_X ? gap : gap / 2;
    LOG(3, "train %s: %+d count(s), %llu ns apart", axis == AXIS_X ? "X" : "Y", pending, (unsigned long long)gap);
    scheduler_new_cpu_event(m->scheduler, &mouse_train_event, m, (uint64_t)axis, 0, first);
}

// 2:1 host-pixel-to-count scaling with a carried remainder. Halving each
// event independently with integer division collapsed small deltas: at high
// pointer-event rates (1 kHz mice, uncoalesced trackpads) per-event deltas
// are +-1..3 on both axes, and 2 and 3 both floored to 1 — nearly every
// event contributed (+-1,+-1) and the cursor walked perfect 45-degree
// diagonals. Carrying the remainder preserves the true X:Y ratio across
// events (and stops odd deltas from silently losing half a count).
static int scale(int value, int8_t *rem) {
    if (value == -1 || value == 1)
        return value; // Preserve single-pixel nudges exactly
    int total = value + *rem;
    int out = total / 2; // Truncates toward zero
    *rem = (int8_t)(total - out * 2);
    LOG(3, "scale: %+d + rem %+d -> %+d, rem %+d", value, (int)(total - value), out, (int)*rem);
    return out;
}

// A host batch: dx/dy host pixels moved since the last one.  Scaled 2:1 to
// counts, joined to whatever the trains still have to play, and re-spread
// over the time the motion took -- the interval since the previous batch,
// at most a frame.  Batches that arrive together (host events between two
// emulated frames all land at the same instant) share one window.
static void mouse_motion(mouse_t *restrict m, int dx, int dy) {
    dx = scale(dx, &m->scale_rem_x);
    dy = scale(dy, &m->scale_rem_y);
    if (dx == 0 && dy == 0)
        return;

    double now = scheduler_time_ns(m->scheduler);
    double since = now - m->last_batch_ns;
    if (since > 0) {
        double window = since < (double)MAC_VBL_PERIOD_NS ? since : (double)MAC_VBL_PERIOD_NS;
        m->window_end_ns = now + window;
        m->last_batch_ns = now;
    }

    if (dx != 0) {
        m->pending_x += dx;
        mouse_train_start(m, AXIS_X, now);
    }
    if (dy != 0) {
        m->pending_y += dy;
        mouse_train_start(m, AXIS_Y, now);
    }
}

// Host motion and the button (VIA PB3, active low: 0 = pressed).
void mouse_update(mouse_t *restrict m, bool button, int dx, int dy) {
    via_input(m->via, 1, 3, !button);
    mouse_motion(m, dx, dy);
}

// Host motion without a button change.
void mouse_move(mouse_t *restrict m, int dx, int dy) {
    mouse_motion(m, dx, dy);
}

// Allocates and initializes a mouse instance with default timing state
mouse_t *mouse_init(struct scheduler *scheduler, scc_t *scc, via_t *restrict via, checkpoint_t *checkpoint) {
    mouse_t *mouse = (mouse_t *)malloc(sizeof(mouse_t));
    if (!mouse)
        return NULL;
    memset(mouse, 0, sizeof(mouse_t));
    mouse->scheduler = scheduler;
    mouse->scc = scc;
    mouse->via = via;

    // Register event type for checkpointing
    scheduler_new_event_type(scheduler, "mouse", mouse, "train", &mouse_train_event);

    // Load from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(mouse_t, scheduler);
        system_read_checkpoint_data(checkpoint, mouse, data_size);
    }

    return mouse;
}

// Free resources associated with a mouse instance
void mouse_delete(mouse_t *mouse) {
    if (!mouse)
        return;
    // Drop everything the scheduler still holds for this object before any
    // of it is torn down.
    scheduler_forget_source(mouse->scheduler, mouse);
    free(mouse);
}

// Save mouse state to a checkpoint
void mouse_checkpoint(mouse_t *restrict mouse, checkpoint_t *checkpoint) {

    // Save mouse state
    if (!mouse || !checkpoint)
        return;
    size_t data_size = offsetof(mouse_t, scheduler);
    system_write_checkpoint_data(checkpoint, mouse, data_size);
}

// === Object-model class descriptor =========================================
//
// `mouse`. Methods move(x, y), click(down), trace(enabled).  Each acts on the
// current machine's input (cfg->host_input) and answers "no machine" when
// none is booted.

// Mode-string → mouse_route_t for debug_mac_*_mode().
// The single mode parser, declared in mouse.h and also used by the machine
// side (mac_host_io.c).  See the header for the mapping.
mouse_route_t input_mouse_mode_parse(const char *mode) {
    if (!mode || !*mode || strcmp(mode, "default") == 0)
        return MOUSE_ROUTE_DEFAULT;
    if (strcmp(mode, "global") == 0)
        return MOUSE_ROUTE_GLOBAL;
    if (strcmp(mode, "hw") == 0 || strcmp(mode, "relative") == 0)
        return MOUSE_ROUTE_HW;
    if (strcmp(mode, "aux") == 0)
        return MOUSE_ROUTE_AUX;
    return MOUSE_ROUTE_INVALID;
}

// The route a mode argument names (default when absent / not a string)
static mouse_route_t mouse_mode_route(const value_t *v) {
    if (!v || v->kind != V_STRING || !v->s)
        return MOUSE_ROUTE_DEFAULT;
    return input_mouse_mode_parse(v->s);
}

static DEF_METHOD(mouse_method_move) {
    if (!global_emulator)
        return val_err("mouse.move: no machine");
    int64_t x = argv[0].i;
    int64_t y = argv[1].i;
    const char *modestr = (argc >= 3 && argv[2].kind == V_STRING && argv[2].s) ? argv[2].s : "default";
    // Validate the cursor mode up front so a bad mode gives a clear error.
    if ((argc >= 3) && mouse_mode_route(&argv[2]) == MOUSE_ROUTE_INVALID)
        return val_err("mouse.move: mode must be one of \"default\"/\"relative\"/\"global\"/\"hw\"/\"aux\"");
    // Inject through the machine substrate: Mac Toolbox cursor / Lisa COPS —
    // one uniform path.
    if (system_input_mouse_move((int)x, (int)y, modestr) < 0)
        return val_err("mouse.move: machine rejected request");
    return val_bool(true);
}

static DEF_METHOD(mouse_method_click) {
    if (!global_emulator)
        return val_err("mouse.click: no machine");
    bool down = (argc >= 1 && argv[0].kind == V_BOOL) ? argv[0].b : true;
    const char *modestr = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s) ? argv[1].s : "default";
    // Validate the cursor mode up front so a bad mode gives a clear error.
    if ((argc >= 2) && mouse_mode_route(&argv[1]) == MOUSE_ROUTE_INVALID)
        return val_err("mouse.click: mode must be one of \"default\"/\"relative\"/\"global\"/\"hw\"/\"aux\"");
    // Inject through the machine substrate (Mac Toolbox cursor / Lisa COPS).
    if (system_input_mouse_button(down, modestr) < 0)
        return val_err("mouse.click: machine rejected request");
    return val_bool(true);
}

static DEF_METHOD(mouse_method_trace) {
    if (!global_emulator)
        return val_err("mouse.trace: no machine");
    debug_mac_set_trace_mouse(global_emulator->host_input, argv[0].b);
    return val_none();
}

// Omitting `mode` is the mode named "default".
static const value_t mouse_def_mode = {.kind = V_STRING, .s = (char *)"default"};

static const arg_decl_t mouse_move_args[] = {
    {.name = "x", .kind = V_INT, .doc = "Target X coordinate"},
    {.name = "y", .kind = V_INT, .doc = "Target Y coordinate"},
    {.name = "mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &mouse_def_mode,
     .doc = "\"default\" (a Mac: absolute Toolbox cursor; a Lisa: deltas), \"relative\" (deltas, every machine), "
            "\"global\" (Toolbox MTemp), \"hw\" (= relative), or \"aux\" (A/UX MAE)"},
};
// `mouse.click()` with no arguments is a press, so the slot has a real
// default rather than none.
static const value_t mouse_click_def_down = {.kind = V_BOOL, .width = 1, .b = true};

static const arg_decl_t mouse_click_args[] = {
    {.name = "down",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &mouse_click_def_down,
     .doc = "true = press, false = release"                                                                },
    {.name = "mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &mouse_def_mode,
     .doc = "\"default\" (per-platform), \"global\" (Toolbox MBState), \"hw\" (raw), or \"aux\" (A/UX MAE)"},
};
static const arg_decl_t mouse_trace_args[] = {
    {.name = "enabled", .kind = V_BOOL, .doc = "true = log mouse position once per second"},
};

static const member_t mouse_members[] = {
    {.kind = M_METHOD,
     .name = "move",
     .doc = "Set mouse position; optional mode chooses the routing path",
     .method = {.args = mouse_move_args, .nargs = 3, .result = V_BOOL, .fn = mouse_method_move}  },
    {.kind = M_METHOD,
     .name = "click",
     .doc = "Press or release the mouse button; optional mode chooses the routing path",
     .method = {.args = mouse_click_args, .nargs = 2, .result = V_BOOL, .fn = mouse_method_click}},
    {.kind = M_METHOD,
     .name = "trace",
     .doc = "Toggle the 1 Hz mouse-position trace logger",
     .method = {.args = mouse_trace_args, .nargs = 1, .result = V_NONE, .fn = mouse_method_trace}},
};

static const class_desc_t mouse_class = {
    .name = "mouse",
    .doc = "The host mouse as the guest sees it: move, click, trace",
    .members = mouse_members,
    .n_members = sizeof(mouse_members) / sizeof(mouse_members[0]),
};

// === Process-singleton lifecycle ============================================
//
// `mouse` is a stateless facade: every method forwards to whatever machine
// is current, so the node itself outlives machines.  Register once at
// shell_init time (idempotent).

static struct object *s_mouse_object = NULL;

void mouse_class_register(void) {
    if (s_mouse_object)
        return;
    s_mouse_object = object_new(&mouse_class, NULL, "mouse");
    if (s_mouse_object) {
        object_set_label(s_mouse_object, "Mouse");
        object_set_order(s_mouse_object, 20);
        object_attach(adb_bus_object(), s_mouse_object);
    }
}
