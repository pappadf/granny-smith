// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_class.c
// The `checkpoint` object: its methods and attributes over the checkpoint
// I/O in checkpoint.c and the system layer's save/load.  Split from
// checkpoint.c as storage_class.c is from storage.c, so the format code
// carries no object-model surface.

#include "checkpoint.h"

#include "object.h"
#include "status.h"
#include "system.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Object-model class descriptor
// ============================================================================
//
// `checkpoint` is a process-singleton (registered at shell_init), so its
// methods resolve before any machine has been booted — that matters for
// the `checkpoint.probe` / `checkpoint.load` calls the WASM startup path
// uses to detect and resume from a quick-saved state. None of the methods
// read object_data; they all go through the platform-level helpers.

static DEF_METHOD(checkpoint_method_probe) {
    return val_bool(system_checkpoint_probe());
}

static DEF_METHOD(checkpoint_method_clear) {
    checkpoint_quick_wait(); // a publish in flight lands first, or the clear would race its rename
    return val_bool(gs_checkpoint_clear() == 0);
}

// `checkpoint.load([path])` — load the named checkpoint file or, when path is
// omitted/empty, auto-load the latest valid checkpoint for the active machine.
//
// This used to build a fake argv[] and hand it to cmd_load_checkpoint, the
// retired command shape, which then string-matched its way back out.  That was
// the last place the pre-object-model command layer was load-bearing, and it
// carried a live collision: cmd_load_checkpoint tested argv[1] against the
// literal "probe", and argv[1] is where this method put the user's path -- so
// `checkpoint.load("probe")` ran a probe instead of loading a file called
// probe.  `checkpoint.probe()` above has been the real entry point all along,
// so that string-match was vestigial -- reachable, but only by accident.
static DEF_METHOD(checkpoint_method_load) {
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : NULL;
    checkpoint_quick_wait(); // load the file the publish in flight is about to complete
    return val_bool(system_checkpoint_load(path) == 0);
}

// `checkpoint.save(path)` — write a consolidated checkpoint to the given path.
// A consolidated checkpoint is self-contained: every file and every disk is
// embedded in full.  (A checkpoint that references its disks is a quick one,
// `checkpoint.snapshot`.)
static DEF_METHOD(checkpoint_method_save) {
    if (argc < 1 || !argv[0].s || !*argv[0].s)
        return val_err("checkpoint.save: path is required");
    return val_bool(system_checkpoint(argv[0].s, CHECKPOINT_KIND_CONSOLIDATED) == STATUS_OK);
}

// `checkpoint.snapshot(name)` — capture a quick (background) checkpoint
// under the given label, filed under the registered machine identity.  Routes
// to gs_background_checkpoint (system.c), which every platform shares.
static DEF_METHOD(checkpoint_method_snapshot) {
    return val_bool(gs_background_checkpoint(argv[0].s) == 0);
}

// `checkpoint.auto` (V_BOOL, RW) — exposes the WASM background-checkpoint
// loop's enabled flag.  A platform with no such loop (headless) reads false
// and refuses the set.
static DEF_GETTER(checkpoint_attr_auto_get) {
    return val_bool(gs_checkpoint_auto_get());
}

static DEF_SETTER(checkpoint_attr_auto_set) {
    if (gs_checkpoint_auto_set(in.b) != 0)
        return val_err("checkpoint.auto: not supported on this platform");
    return val_none();
}

static const arg_decl_t checkpoint_load_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Checkpoint path; empty auto-loads the latest"},
};

static const arg_decl_t checkpoint_save_args[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Checkpoint output path"},
};

static const arg_decl_t checkpoint_snapshot_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Snapshot label"},
};

static const member_t checkpoint_members[] = {
    {.kind = M_ATTR,
     .name = "auto",
     .doc = "Automatic background checkpoints enabled: the periodic save and the tab-hidden save (WASM only)",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = checkpoint_attr_auto_get, .set = checkpoint_attr_auto_set}},
    {.kind = M_METHOD,
     .name = "probe",
     .examples = EXAMPLES("checkpoint.probe"),
     .doc = "True if a valid checkpoint exists for the active machine",
     .method = {.result_doc = "true when one exists",
                .args = NULL,
                .nargs = 0,
                .result = V_BOOL,
                .fn = checkpoint_method_probe}},
    {.kind = M_METHOD,
     .name = "clear",
     .examples = EXAMPLES("checkpoint.clear"),
     .doc = "Remove all checkpoint files for the active machine, and the image deltas no open image holds",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = checkpoint_method_clear}},
    {.kind = M_METHOD,
     .name = "load",
     .examples = EXAMPLES("checkpoint.load", "checkpoint.load \"/opfs/checkpoints/before-install.gscp\""),
     .doc = "Load a checkpoint",
     .method = {.result_doc = "true when it loaded",
                .args = checkpoint_load_args,
                .nargs = 1,
                .result = V_BOOL,
                .fn = checkpoint_method_load}},
    {.kind = M_METHOD,
     .name = "save",
     .examples = EXAMPLES("checkpoint.save \"/opfs/checkpoints/before-install.gscp\""),
     .doc = "Save the current machine state to a checkpoint file",
     .method = {.result_doc = "true when it was written",
                .args = checkpoint_save_args,
                .nargs = 1,
                .result = V_BOOL,
                .fn = checkpoint_method_save}},
    {.kind = M_METHOD,
     .name = "snapshot",
     .examples = EXAMPLES("checkpoint.snapshot \"before-install\""),
     .doc = "Capture a quick (background) checkpoint under the given label",
     .method = {.args = checkpoint_snapshot_args, .nargs = 1, .result = V_BOOL, .fn = checkpoint_method_snapshot}},
};

static const class_desc_t checkpoint_class = {
    .name = "checkpoint",
    .members = checkpoint_members,
    .n_members = sizeof(checkpoint_members) / sizeof(checkpoint_members[0]),
    .doc = "Saves and restores the whole machine state",
};

// ============================================================================
// Lifecycle (process-singleton, idempotent)
// ============================================================================

static struct object *s_checkpoint_object = NULL;

void checkpoint_init(void) {
    if (s_checkpoint_object)
        return;
    s_checkpoint_object = object_new(&checkpoint_class, NULL, "checkpoint");
    if (s_checkpoint_object) {
        object_set_order(s_checkpoint_object, 20);
        object_attach(object_root(), s_checkpoint_object);
    }
}

void checkpoint_delete(void) {
    if (s_checkpoint_object) {
        object_detach(s_checkpoint_object);
        object_delete(s_checkpoint_object);
        s_checkpoint_object = NULL;
    }
}
