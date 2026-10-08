// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mouse_class.c
// The `mouse` object-model class: a process-singleton facade whose methods
// forward to the current machine's input substrate (Mac Toolbox cursor,
// Lisa COPS).  It holds no mouse_t -- the Plus quadrature model itself is
// in mouse.c.

#include "adb.h"
#include "debug_mac.h"
#include "mouse.h"
#include "object.h"
#include "system.h"
#include "system_internal.h"
#include "value.h"

#include <string.h>

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
