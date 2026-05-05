// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.c
// Generic emulator lifecycle: creation, destruction, checkpointing, and
// shared device coordination. Machine-specific init/teardown logic lives in
// the machine's own source file (e.g., src/machines/plus.c) and is invoked
// through the hw_profile_t callback interface.

#include "system_config.h" // full config_t definition (includes system.h transitively)

#include "appletalk.h"
#include "build_id.h"
#include "checkpoint_machine.h"
#include "cmd_io.h"
#include "cmd_parse.h"
#include "cmd_types.h"
#include "cpu.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "image.h"
#include "image_vfs.h"
#include "keyboard.h"
#include "log.h"
#include "machine.h"
#include "memory.h"
#include "mouse.h"
#include "rom.h"
#include "root.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "shell.h"
#include "sound.h"
#include "via.h"
#include "vrom.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

LOG_USE_CATEGORY_NAME("setup");

// Global emulator pointer (definition)
config_t *global_emulator = NULL;

// Forward declarations for SCSI device attachment functions
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id);

// Pending RAM override (KB). Set by `setup --ram` or headless `ram=` arg.
// Consumed by system_create(); 0 means use machine default.
static uint32_t g_pending_ram_kb = 0;

void system_set_pending_ram_kb(uint32_t kb) {
    g_pending_ram_kb = kb;
}
uint32_t system_get_pending_ram_kb(void) {
    return g_pending_ram_kb;
}

// Pick the delta directory for a fresh writable mount.  Default is the
// active machine directory (so deltas live alongside state.checkpoint and
// the manifest, §2.1).  For volatile bases under /tmp/ — typically test
// artifacts uploaded to memfs — fall back to NULL so image_create places
// deltas adjacent to the base, preserving memfs-only I/O performance.
static const char *pick_delta_dir(const char *path) {
    if (path && strncmp(path, "/tmp/", 5) == 0)
        return NULL;
    return checkpoint_machine_dir();
}

// Config field accessors for opaque handle access
image_t *config_get_image(config_t *cfg, int index) {
    return cfg ? cfg->images[index] : NULL;
}
int config_get_n_images(config_t *cfg) {
    return cfg ? cfg->n_images : 0;
}

// Add an image to the config's tracked image list
void config_add_image(config_t *cfg, image_t *image) {
    assert(cfg != NULL);
    assert(cfg->n_images < MAX_IMAGES);
    cfg->images[cfg->n_images] = image;
    cfg->n_images++;
}

// Find an image object by its filename path
image_t *setup_get_image_by_filename(const char *filename) {
    struct config *config = global_emulator;
    if (!config || !filename)
        return NULL;
    for (int i = 0; i < config->n_images; ++i) {
        const char *name = image_get_filename(config->images[i]);
        if (name && strcmp(name, filename) == 0)
            return config->images[i];
    }
    return NULL;
}

// System-level mouse input wrapper: routes input to appropriate mouse device model
void system_mouse_update(bool button, int dx, int dy) {
    if (!global_emulator)
        return;
    if (global_emulator->adb)
        adb_mouse_event(global_emulator->adb, button, dx, dy);
    else if (global_emulator->mouse)
        mouse_update(global_emulator->mouse, button, dx, dy);
}

// Injects mouse movement deltas without changing button state.
// Routes to the appropriate hardware path (ADB or quadrature).
// Returns true if deltas were injected, false if no mouse device is available.
bool system_mouse_move(int dx, int dy) {
    if (!global_emulator)
        return false;
    if (global_emulator->adb) {
        adb_mouse_move(global_emulator->adb, dx, dy);
        return true;
    }
    if (global_emulator->mouse) {
        mouse_move(global_emulator->mouse, dx, dy);
        return true;
    }
    return false;
}

// Injects mouse movement deltas through ADB only (no button change).
// Returns true if injected through ADB, false on non-ADB machines.
// Used by the default set-mouse path to preserve the original behavior where
// ADB machines use delta injection and non-ADB machines fall through to
// direct global writes.
bool system_mouse_move_adb(int dx, int dy) {
    if (!global_emulator || !global_emulator->adb)
        return false;
    adb_mouse_move(global_emulator->adb, dx, dy);
    return true;
}

// System-level keyboard input wrapper: routes input to appropriate keyboard device model
void system_keyboard_update(key_event_t event, int key) {
    if (!global_emulator)
        return;
    if (global_emulator->adb)
        adb_keyboard_event(global_emulator->adb, event, key);
    else if (global_emulator->keyboard)
        keyboard_update(global_emulator->keyboard, event, key);
}

// Hardware RESET line: calls the machine's reset handler to reinitialize
// peripherals.  On SE/30: VIA1 re-enables ROM overlay, MMU disabled.
void system_hardware_reset(void) {
    if (global_emulator && global_emulator->machine && global_emulator->machine->reset)
        global_emulator->machine->reset(global_emulator);
}

// System-level scheduler accessor: returns the current scheduler object
scheduler_t *system_scheduler(void) {
    return global_emulator ? global_emulator->scheduler : NULL;
}

// System-level memory accessor: returns the current memory object
memory_map_t *system_memory(void) {
    return global_emulator ? global_emulator->mem_map : NULL;
}

// System-level debug accessor: returns the current debugger object
debug_t *system_debug(void) {
    return global_emulator ? global_emulator->debugger : NULL;
}

// System-level CPU accessor: returns the current CPU object
cpu_t *system_cpu(void) {
    return global_emulator ? global_emulator->cpu : NULL;
}

// System-level RTC accessor: returns the current RTC object
rtc_t *system_rtc(void) {
    return global_emulator ? global_emulator->rtc : NULL;
}

// System-level framebuffer accessor: returns pointer to video RAM buffer
uint8_t *system_framebuffer(void) {
    return global_emulator ? global_emulator->ram_vbuf : NULL;
}

// Check if emulator is initialized and running
bool system_is_initialized(void) {
    return global_emulator != NULL;
}

// Return the model_id of the current machine, or NULL if none is active
const char *system_machine_model_id(void) {
    if (!global_emulator || !global_emulator->machine)
        return NULL;
    return global_emulator->machine->model_id;
}

// Ensure the correct machine is active for the given model_id.
// Creates a new machine if none exists, or tears down and recreates if the
// current machine's model_id doesn't match.  Returns 0 on success, -1 on error.
int system_ensure_machine(const char *model_id) {
    if (!model_id)
        return -1;

    const hw_profile_t *needed = machine_find(model_id);
    if (!needed) {
        printf("system_ensure_machine: unknown model '%s'\n", model_id);
        return -1;
    }

    // Already have the right machine?
    const char *current = system_machine_model_id();
    if (current && strcmp(current, model_id) == 0)
        return 0;

    // Teardown existing machine if wrong type
    if (global_emulator) {
        printf("Switching machine from %s to %s\n", global_emulator->machine->model_id, model_id);
        system_destroy(global_emulator);
        global_emulator = NULL;
    }

    // Create the new machine
    config_t *cfg = system_create(needed, NULL);
    if (!cfg) {
        printf("system_ensure_machine: failed to create %s\n", model_id);
        return -1;
    }

    printf("Machine created: %s (%s)\n", needed->model_name, needed->model_id);
    return 0;
}

// Helpers to abstract floppy insertion
static bool sys_fd_is_inserted(config_t *cfg, int drive) {
    if (cfg->floppy)
        return floppy_is_inserted(cfg->floppy, drive);
    return true; // no controller → treat as occupied
}

static int sys_fd_insert(config_t *cfg, int drive, image_t *disk) {
    if (cfg->floppy)
        return floppy_insert(cfg->floppy, drive, disk);
    return -1;
}

// Trigger a vertical blanking interval event (delegates to machine callback)
void trigger_vbl(struct config *restrict config) {
    if (config && config->machine && config->machine->trigger_vbl) {
        config->machine->trigger_vbl(config);
    }
}

// ============================================================================
// Static helpers for inlined command logic (shared by unified handlers)
// ============================================================================

// Insert a floppy disk image into the first free (or preferred) drive.
// writable: 1=writable, 0=read-only, -1=default (writable).
// preferred: drive number (0 or 1), or -1 for auto-select.
static int do_insert_fd(const char *path, int preferred, int writable_flag) {
    bool writable = (writable_flag != 0); // default to writable unless explicitly 0

    // Persist volatile images (/tmp/, /fd/) to OPFS so they survive page reload
    char *persistent_path = image_persist_volatile(path);
    if (persistent_path)
        path = persistent_path;

    image_t *disk = writable ? image_create(path, pick_delta_dir(path)) : image_open_readonly(path);
    if (!disk) {
        printf("fd insert: failed to open disk image: %s\n", path);
        free(persistent_path);
        return -1;
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("fd insert: emulator config not initialized.\n");
        free(persistent_path);
        return -1;
    }

    bool d0_free = !sys_fd_is_inserted(config, 0);
    bool d1_free = !sys_fd_is_inserted(config, 1);

    int target = -1;
    if (preferred != -1 && (preferred == 0 ? d0_free : d1_free)) {
        target = preferred;
    } else if (d0_free) {
        target = 0;
    } else if (d1_free) {
        target = 1;
    }

    if (target == -1) {
        printf("fd insert: both floppy drives are already occupied.\n");
        free(persistent_path);
        return -1;
    }

    add_image(config, disk);
    sys_fd_insert(config, target, disk);
    printf("fd insert: inserted %s into floppy drive %d.\n", path, target);
    free(persistent_path);
    return 0;
}

// Probe a file to check if it's a valid floppy image (without inserting).
// Returns 0 if valid floppy, 1 if not.
int system_probe_floppy(const char *path) {
    // Persist volatile images (/tmp/, /fd/) to OPFS so they survive page reload
    char *persistent_path = image_persist_volatile(path);
    if (persistent_path)
        path = persistent_path;

    image_t *disk = image_open_readonly(path);
    if (!disk) {
        printf("%s: NOT a supported format\n", path);
        free(persistent_path);
        return 1;
    }

    if (disk->type != image_fd_ss && disk->type != image_fd_ds && disk->type != image_fd_hd) {
        printf("%s: Valid disk image but not a floppy (size: %zu bytes)\n", path, disk->raw_size);
        image_close(disk);
        free(persistent_path);
        return 1;
    }

    const char *type_str = "unknown";
    if (disk->type == image_fd_ss)
        type_str = "single-sided 400KB";
    else if (disk->type == image_fd_ds)
        type_str = "double-sided 800KB";
    else if (disk->type == image_fd_hd)
        type_str = "high-density 1440KB";

    printf("%s: Valid floppy image (%s)\n", path, type_str);
    image_close(disk);
    free(persistent_path);
    return 0;
}

// Create a new blank floppy image and insert it.
// Returns 0 on success, -1 on failure.
int system_create_floppy(const char *path, bool high_density, int preferred) {
    config_t *config = global_emulator;
    if (!config) {
        printf("fd create: emulator config not initialized.\n");
        return -1;
    }

    bool d0_free = !sys_fd_is_inserted(config, 0);
    bool d1_free = !sys_fd_is_inserted(config, 1);

    int target = -1;
    if (preferred != -1 && (preferred == 0 ? d0_free : d1_free)) {
        target = preferred;
    } else if (d0_free) {
        target = 0;
    } else if (d1_free) {
        target = 1;
    }

    if (target == -1) {
        printf("fd create: both floppy drives are already occupied.\n");
        return -1;
    }

    int rc = image_create_blank_floppy(path, false, high_density);
    if (rc != 0) {
        if (rc == -2)
            printf("fd create: file already exists: %s (won't overwrite)\n", path);
        else
            printf("fd create: failed to create blank floppy file: %s\n", path);
        return -1;
    }

    image_t *disk = image_create(path, pick_delta_dir(path));
    if (!disk) {
        printf("fd create: failed to open newly created image: %s\n", path);
        return -1;
    }

    add_image(config, disk);
    sys_fd_insert(config, target, disk);
    printf("fd create: created %s (%s) and inserted into drive %d.\n", path, high_density ? "1440K" : "800K", target);
    return 0;
}

// Size limits for hd create
#define HD_CREATE_MAX_SIZE (2ULL * 1024 * 1024 * 1024) // 2 GiB

// Floppy sizes that should be rejected
#define FLOPPY_400K  409600
#define FLOPPY_800K  819200
#define FLOPPY_1440K 1474560

// Create a new blank hard disk image at the given path.
// Returns 0 on success, -1 on failure.
static int do_create_hd(const char *path, const char *size_str) {
    size_t size = drive_catalog_parse_size(size_str);
    if (size == 0) {
        printf("hd create: invalid size: %s\n", size_str);
        printf("  Use a drive model (e.g. HD20SC), human size (e.g. 40mb),\n");
        printf("  or exact bytes/suffix (e.g. 20M, 512K, 21411840)\n");
        printf("  Run 'hd models' to see available drive sizes.\n");
        return -1;
    }
    if (size > HD_CREATE_MAX_SIZE) {
        printf("hd create: size %zu exceeds maximum (2 GiB)\n", size);
        return -1;
    }
    // reject floppy-sized images
    if (size == FLOPPY_400K || size == FLOPPY_800K || size == FLOPPY_1440K) {
        printf("hd create: size %zu matches a floppy format, use fd create instead\n", size);
        return -1;
    }
    // refuse to overwrite existing files
    FILE *exist = fopen(path, "rb");
    if (exist) {
        fclose(exist);
        printf("hd create: file already exists: %s (won't overwrite)\n", path);
        return -1;
    }
    int rc = image_create_empty(path, size);
    if (rc != 0) {
        printf("hd create: failed to create image: %s\n", path);
        return -1;
    }
    printf("hd create: created %s (%zu bytes)\n", path, size);
    return 0;
}

// Find an already-attached image by path, or NULL if not found.
static image_t *find_attached_image(const char *path) {
    config_t *cfg = global_emulator;
    if (!cfg)
        return NULL;
    int n = config_get_n_images(cfg);
    for (int i = 0; i < n; i++) {
        image_t *img = config_get_image(cfg, i);
        if (img && img->filename && strcmp(img->filename, path) == 0)
            return img;
    }
    return NULL;
}

// Download (export) a hard disk image: merge base + delta into a new file.
// Prefers the live attached image (has current in-memory bitmaps) over
// opening a fresh instance (which may have stale on-disk bitmaps).
// Returns 0 on success, -1 on failure.
int system_download_hd(const char *src_path, const char *dest_path) {
    // Try the live attached image first (has up-to-date bitmaps)
    image_t *live = find_attached_image(src_path);
    if (live) {
        size_t size = live->raw_size;
        int rc = image_export_to(live, dest_path);
        if (rc != 0) {
            printf("hd download: failed to export %s to %s\n", src_path, dest_path);
            return -1;
        }
        printf("hd download: exported %s to %s (%zu bytes)\n", src_path, dest_path, size);
        return 0;
    }
    // Fall back to opening a fresh image (for detached / offline images)
    image_t *img = image_open_readonly(src_path);
    if (!img) {
        printf("hd download: cannot open image: %s\n", src_path);
        return -1;
    }
    size_t size = img->raw_size;
    int rc = image_export_to(img, dest_path);
    image_close(img);
    if (rc != 0) {
        printf("hd download: failed to export %s to %s\n", src_path, dest_path);
        return -1;
    }
    printf("hd download: exported %s to %s (%zu bytes)\n", src_path, dest_path, size);
    return 0;
}

// Attach a SCSI hard disk image. Delegates to add_scsi_drive().
// Returns 0 on success, -1 on error.
static int do_attach_hd(const char *path, int scsi_id) {
    if (scsi_id < 0 || scsi_id > 7) {
        printf("hd attach: invalid SCSI ID %d (expected 0..7)\n", scsi_id);
        return -1;
    }
    config_t *config = global_emulator;
    if (!config) {
        printf("hd attach: emulator not initialized.\n");
        return -1;
    }
    add_scsi_drive(config, path, scsi_id);
    return 0;
}

// Enable/disable/query SCC external loopback.
// state: "on", "off", or NULL (query). Returns 0 on success, -1 on error.
static int do_scc_loopback(const char *state) {
    if (!global_emulator || !global_emulator->scc) {
        printf("No SCC available\n");
        return -1;
    }
    if (!state) {
        const char *s = scc_get_external_loopback(global_emulator->scc) ? "on" : "off";
        printf("scc-loopback: %s\n", s);
        printf("  (Use \"scc loopback on\" or \"scc loopback off\" to change.)\n");
        return 0;
    }
    if (strcmp(state, "on") == 0) {
        scc_set_external_loopback(global_emulator->scc, true);
        printf("SCC external loopback enabled\n");
    } else if (strcmp(state, "off") == 0) {
        scc_set_external_loopback(global_emulator->scc, false);
        printf("SCC external loopback disabled\n");
    } else {
        printf("Usage: scc loopback [on|off]\n");
        return -1;
    }
    return 0;
}

// Enable/disable/query SCSI loopback test card.
// state: "on", "off", or NULL (query). Returns 0 on success, -1 on error.
static int do_scsi_loopback(const char *state) {
    if (!global_emulator || !global_emulator->scsi) {
        printf("No SCSI controller available\n");
        return -1;
    }
    if (!state) {
        const char *s = scsi_get_loopback(global_emulator->scsi) ? "on" : "off";
        printf("scsi-loopback: %s\n", s);
        printf("  (Use \"hd loopback on\" or \"hd loopback off\" to change.)\n");
        return 0;
    }
    if (strcmp(state, "on") == 0) {
        scsi_set_loopback(global_emulator->scsi, true);
        printf("SCSI loopback test card enabled\n");
    } else if (strcmp(state, "off") == 0) {
        scsi_set_loopback(global_emulator->scsi, false);
        printf("SCSI loopback test card disabled\n");
    } else {
        printf("Usage: hd loopback [on|off]\n");
        return -1;
    }
    return 0;
}

// ============================================================================
// Command handlers for disk, scsi, scc, setup, rom, vrom
// ============================================================================

// --- disk (unified: insert/create/eject/probe) ---
static void cmd_fd_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: fd <insert|create|probe|validate> <path>");
        return;
    }

    if (strcmp(subcmd, "insert") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: fd insert <path> [drive] [writable]");
            return;
        }
        int preferred = ctx->args[1].present ? (int)ctx->args[1].as_int : -1;
        // Default to writable (matches legacy insert-disk behavior)
        int writable = ctx->args[2].present ? (ctx->args[2].as_bool ? 1 : 0) : 1;
        int rc = do_insert_fd(ctx->args[0].as_str, preferred, writable);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "create") == 0) {
        // Manual arg parsing: fd create [--hd] <path> [drive]
        // The --hd flag confuses the positional arg parser, so parse raw_argv.
        bool has_hd = false;
        const char *path = NULL;
        int preferred = -1;
        for (int i = 2; i < ctx->raw_argc; i++) {
            if (strcmp(ctx->raw_argv[i], "--hd") == 0) {
                has_hd = true;
            } else if (!path) {
                path = ctx->raw_argv[i];
            } else {
                // Drive number
                preferred = ctx->raw_argv[i][0] - '0';
            }
        }
        if (!path) {
            cmd_err(res, "usage: fd create [--hd] <path> [drive]");
            return;
        }
        int rc = system_create_floppy(path, has_hd, preferred);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "probe") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: fd probe <path>");
            return;
        }
        int rc = system_probe_floppy(ctx->args[0].as_str);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "validate") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: fd validate <path>");
            return;
        }
        const char *path = ctx->args[0].as_str;
        image_t *img = image_open_readonly(path);
        if (!img) {
            cmd_printf(ctx, "invalid floppy image: cannot open %s\n", path);
            cmd_bool(res, false);
            return;
        }
        const char *desc = NULL;
        switch (img->type) {
        case image_fd_ss:
            desc = img->from_diskcopy ? "400K floppy (single-sided, DiskCopy 4.2)" : "400K floppy (single-sided, raw)";
            break;
        case image_fd_ds:
            desc = img->from_diskcopy ? "800K floppy (double-sided, DiskCopy 4.2)" : "800K floppy (double-sided, raw)";
            break;
        case image_fd_hd:
            desc =
                img->from_diskcopy ? "1.4MB floppy (high-density, DiskCopy 4.2)" : "1.4MB floppy (high-density, raw)";
            break;
        default:
            break;
        }
        if (desc) {
            cmd_printf(ctx, "valid %s\n", desc);
            cmd_bool(res, true);
        } else {
            cmd_printf(ctx, "invalid floppy image: not a floppy (%zu bytes)\n", img->raw_size);
            cmd_bool(res, false);
        }
        image_close(img);
        return;
    }
    if (strcmp(subcmd, "eject") == 0) {
        cmd_printf(ctx, "fd eject: not yet implemented\n");
        cmd_ok(res);
        return;
    }
    cmd_err(res, "unknown fd subcommand: %s", subcmd);
}

// --- scsi (unified: attach/loopback) ---
static void cmd_hd_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: hd <create|attach|loopback|validate|download> [args...]");
        return;
    }

    if (strcmp(subcmd, "create") == 0) {
        if (!ctx->args[0].present || !ctx->args[1].present) {
            cmd_err(res, "usage: hd create <path> <size>");
            return;
        }
        int rc = do_create_hd(ctx->args[0].as_str, ctx->args[1].as_str);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "attach") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: hd attach <path> [id]");
            return;
        }
        int id = ctx->args[1].present ? (int)ctx->args[1].as_int : 0;
        int rc = do_attach_hd(ctx->args[0].as_str, id);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "loopback") == 0) {
        const char *state = ctx->args[0].present ? ctx->args[0].as_str : NULL;
        int rc = do_scsi_loopback(state);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "validate") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: hd validate <path>");
            return;
        }
        const char *path = ctx->args[0].as_str;
        image_t *img = image_open_readonly(path);
        if (!img) {
            cmd_printf(ctx, "invalid SCSI HD image: cannot open %s\n", path);
            cmd_bool(res, false);
            return;
        }
        // Reject floppy-sized images
        if (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd) {
            cmd_printf(ctx, "invalid SCSI HD image: size matches floppy (%zu bytes), use fd validate\n", img->raw_size);
            image_close(img);
            cmd_bool(res, false);
            return;
        }
        // Match against known drive models via catalog
        size_t sz = img->raw_size;
        const struct drive_model *best = drive_catalog_find_closest(sz);
        if (sz == best->size)
            cmd_printf(ctx, "valid SCSI HD image: %zu bytes, matches %s %s\n", sz, best->vendor, best->product);
        else
            cmd_printf(ctx, "valid SCSI HD image: %zu bytes, nearest model %s %s\n", sz, best->vendor, best->product);
        image_close(img);
        cmd_bool(res, true);
        return;
    }
    if (strcmp(subcmd, "download") == 0) {
        if (!ctx->args[0].present || !ctx->args[1].present) {
            cmd_err(res, "usage: hd download <source> <dest>");
            return;
        }
        int rc = system_download_hd(ctx->args[0].as_str, ctx->args[1].as_str);
        cmd_int(res, (int64_t)rc);
        return;
    }
    if (strcmp(subcmd, "models") == 0) {
        // list known drive models as human-readable table or JSON
        const char *fmt = ctx->args[0].present ? ctx->args[0].as_str : NULL;
        int json = fmt && strcmp(fmt, "--json") == 0;
        int count = drive_catalog_count();
        if (json) {
            cmd_printf(ctx, "[");
            for (int i = 0; i < count; i++) {
                const struct drive_model *m = drive_catalog_get(i);
                cmd_printf(ctx, "%s{\"label\":\"%s\",\"vendor\":\"%s\",\"product\":\"%s\",\"size\":%zu}", i ? "," : "",
                           m->label, m->vendor, m->product, m->size);
            }
            cmd_printf(ctx, "]\n");
        } else {
            cmd_printf(ctx, "%-10s %-10s %-10s %12s\n", "Label", "Vendor", "Product", "Size");
            for (int i = 0; i < count; i++) {
                const struct drive_model *m = drive_catalog_get(i);
                // compute approximate MB for display
                unsigned mb = (unsigned)(m->size / (1000 * 1000));
                cmd_printf(ctx, "%-10s %-10s %-10s %12zu  (%u MB)\n", m->label, m->vendor, m->product, m->size, mb);
            }
        }
        cmd_ok(res);
        return;
    }
    cmd_err(res, "unknown hd subcommand: %s", subcmd);
}

// Registration tables

// disk subcommands
static const struct arg_spec fd_path_args[] = {
    {"path",     ARG_PATH,                "disk image path"},
    {"drive",    ARG_INT | ARG_OPTIONAL,  "drive number"   },
    {"writable", ARG_BOOL | ARG_OPTIONAL, "writable"       },
};
// fd create uses ARG_REST to handle --hd flag mixed with positional args
static const struct arg_spec fd_create_args[] = {
    {"args", ARG_REST, "[--hd] <path> [drive]"},
};
static const struct subcmd_spec fd_subcmds[] = {
    {"insert",   NULL, fd_path_args,   3, "auto-detect and insert"            },
    {"create",   NULL, fd_create_args, 1, "create blank floppy"               },
    {"probe",    NULL, fd_path_args,   1, "validate without inserting"        },
    {"validate", NULL, fd_path_args,   1, "validate a floppy image (detailed)"},
    {"eject",    NULL, NULL,           0, "eject disk (future)"               },
};

// scsi subcommands
static const struct arg_spec hd_attach_args[] = {
    {"path", ARG_PATH,               "hard disk image path"},
    {"id",   ARG_INT | ARG_OPTIONAL, "SCSI ID"             },
};
static const char *loopback_values[] = {"on", "off", NULL};
static const struct arg_spec loopback_args[] = {
    {"state", ARG_ENUM | ARG_OPTIONAL, "on or off", loopback_values},
};
static const struct arg_spec hd_create_args[] = {
    {"path", ARG_PATH,   "image file path"                             },
    {"size", ARG_STRING, "size: model (HD20SC), human (40mb), or bytes"},
};
static const struct arg_spec hd_download_args[] = {
    {"source", ARG_PATH, "source hard disk image path"},
    {"dest",   ARG_PATH, "destination file path"      },
};
static const struct arg_spec hd_models_args[] = {
    {"format", ARG_STRING | ARG_OPTIONAL, "--json for machine-readable output"},
};
static const struct subcmd_spec hd_subcmds[] = {
    {"create",   NULL, hd_create_args,   2, "create a blank hard disk image"        },
    {"attach",   NULL, hd_attach_args,   2, "attach hard disk image"                },
    {"loopback", NULL, loopback_args,    1, "passive terminator"                    },
    {"validate", NULL, hd_attach_args,   1, "validate a hard disk image"            },
    {"models",   NULL, hd_models_args,   1, "list known drive models and sizes"     },
    {"download", NULL, hd_download_args, 2, "export disk image (base+delta) to file"},
};

// image subcommands — inspect disk image contents without attaching to
// a SCSI bus.  Phase 1: partmap + probe work; list + unmount are stubs.
static const struct arg_spec image_partmap_args[] = {
    {"path",   ARG_PATH,                  "image file path"                   },
    {"format", ARG_STRING | ARG_OPTIONAL, "--json for machine-readable output"},
};
static const struct arg_spec image_probe_args[] = {
    {"path", ARG_PATH, "image file path"},
};
static const struct arg_spec image_list_args[] = {
    {"format", ARG_STRING | ARG_OPTIONAL, "--json for machine-readable output"},
};
static const struct arg_spec image_unmount_args[] = {
    {"path", ARG_PATH, "image file path"},
};
static const struct subcmd_spec image_subcmds[] = {
    {"partmap", NULL, image_partmap_args, 2, "parse and print partition map"                   },
    {"probe",   NULL, image_probe_args,   1, "print detected format without descent"           },
    {"list",    NULL, image_list_args,    1, "show currently-cached auto-mounts (Phase 2 stub)"},
    {"unmount", NULL, image_unmount_args, 1, "force-close a cached auto-mount (Phase 2 stub)"  },
};
extern void cmd_image_handler(struct cmd_context *ctx, struct cmd_result *res);

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Register built-in machine profiles so machine_find() can look them up
    machine_register(&machine_plus);
    machine_register(&machine_se30);

    // Ensure logging categories of interest appear in `log list` even before any messages are emitted.
    // shell_init() (called earlier) already invoked log_init(); categories default to level 0 (OFF).
    (void)log_register_category("appletalk");

    // Phase 5c — legacy `register_command` calls retired. The typed
    // object-model bridge (root.c) now provides every operation;
    // the underlying handlers (cmd_fd_handler, cmd_hd_handler, …)
    // remain so shell_fd_argv / shell_hd_argv can call them directly.
    image_init(NULL);
}

// Platform hook called after system_create completes.
// The weak default is a no-op; the WASM platform overrides to install
// the assertion callback (which requires the debug object to exist).
__attribute__((weak)) void system_post_create(config_t *cfg) {
    (void)cfg;
}

// Background-checkpoint auto state. WASM-only at the moment — the
// headless build has no auto-checkpoint loop, so the weak defaults
// just stub out; em_main.c overrides them to read/write the live
// `checkpoint_auto_enabled` flag.
__attribute__((weak)) bool gs_checkpoint_auto_get(void) {
    return false;
}

__attribute__((weak)) void gs_checkpoint_auto_set(bool enabled) {
    (void)enabled;
}

// Platform-specific entry points (see system.h).  Headless gets the
// "not supported" stubs by default; em_main.c overrides them on WASM.
__attribute__((weak)) void gs_quit(void) {}
__attribute__((weak)) int gs_download(const char *path) {
    (void)path;
    printf("download: only supported in the WASM build\n");
    return -1;
}
__attribute__((weak)) int gs_background_checkpoint(const char *reason) {
    (void)reason;
    printf("background-checkpoint: only supported in the WASM build\n");
    return -1;
}
__attribute__((weak)) int gs_checkpoint_clear(void) {
    printf("checkpoint clear: only supported in the WASM build\n");
    return -1;
}
__attribute__((weak)) int gs_register_machine(const char *machine_id, const char *created) {
    (void)machine_id;
    (void)created;
    return 0; // headless has no per-machine checkpoint scoping; treat as no-op success
}

__attribute__((weak)) int gs_find_media(const char *dir_path, const char *dest) {
    (void)dir_path;
    (void)dest;
    printf("find-media: only supported in the WASM build\n");
    return 1;
}

// Create an emulator instance for the given machine profile.
// Allocates config_t, wires the machine descriptor, and calls profile->init().
config_t *system_create(const hw_profile_t *profile, checkpoint_t *checkpoint) {
    assert(profile != NULL);
    assert(profile->init != NULL);

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg)
        return NULL;
    memset(cfg, 0, sizeof(config_t));

    cfg->machine = profile;
    global_emulator = cfg;

    // Compute RAM size: use pending override if set, otherwise machine default
    if (g_pending_ram_kb > 0) {
        cfg->ram_size = g_pending_ram_kb * 1024;
        if (cfg->ram_size > profile->ram_size_max)
            cfg->ram_size = profile->ram_size_max;
        g_pending_ram_kb = 0; // consume the override
    } else {
        cfg->ram_size = profile->ram_size_default;
    }

    // Delegate all machine-specific initialisation to the profile
    profile->init(cfg, checkpoint);

    // Stand up the object-model root (M2): attaches stub classes for
    // cpu/memory/scheduler/machine/shell/storage so `eval` can read
    // runtime state. The legacy shell remains primary.
    root_install(cfg);

    // Notify the platform (e.g., install assertion callback)
    system_post_create(cfg);

    // Cold boot: stamp out a manifest documenting what was set up.  Skipped
    // on checkpoint restore — the manifest is fixed at original creation
    // time and is purely informational (§2.7).  Failure is non-fatal.
    if (!checkpoint && checkpoint_machine_dir())
        checkpoint_machine_write_manifest();

    return cfg;
}

// Destroy an emulator instance: call machine teardown and free all resources.
void system_destroy(config_t *config) {
    if (!config)
        return;

    // Tear down the object-model root before machine teardown so stub
    // getters cannot dereference half-freed subsystem state.  Use the
    // _if variant so the destroy of an already-replaced config (e.g.,
    // after `checkpoint --load` ran system_create(new) before us) does
    // NOT wipe the just-installed new-cfg stubs.
    //
    // Note: rom and vrom are deliberately NOT torn down here. They are
    // process-scoped singletons (no per-config state); their object nodes
    // outlive any specific emulator instance and are reclaimed at process
    // exit. Calling rom_delete here would break checkpoint reload, where
    // system_destroy(old) runs *after* system_create(new) has already
    // pinned a fresh emulator that still references the rom object.
    root_uninstall_if(config);

    // Delegate machine-specific teardown to the profile
    if (config->machine && config->machine->teardown) {
        config->machine->teardown(config);
    }

    // Free all tracked images (managed at the system level)
    for (int i = 0; i < config->n_images; ++i) {
        if (config->images[i]) {
            image_close(config->images[i]);
            config->images[i] = NULL;
        }
    }
    config->n_images = 0;

    free(config);
}

// Reset Mac hardware to initial state
void mac_reset(config_t *restrict sim) {
    scc_reset(sim->scc);
}

// Add a SCSI hard disk to the configuration
void add_scsi_drive(struct config *restrict config, const char *filename, int scsi_id) {
    // Persist volatile images to OPFS
    char *persistent_path = image_persist_volatile(filename);
    if (persistent_path)
        filename = persistent_path;

    image_t *img = image_create(filename, pick_delta_dir(filename));
    if (!img) {
        printf("Failed to open image: %s\n", filename);
        free(persistent_path);
        return;
    }

    size_t sz = disk_size(img);

    // Find the closest drive model from the catalog
    const struct drive_model *best = drive_catalog_find_closest(sz);

    printf("Attaching SCSI drive: %s as %s %s (size: %zu bytes, SCSI ID: %d)\n", filename, best->vendor, best->product,
           sz, scsi_id);

    add_image(config, img);
    scsi_add_device(config->scsi, scsi_id, best->vendor, best->product, "1.0", img, scsi_dev_hd, 512, false);
    // Block the VFS auto-mount cache from serving reads on the same file
    // while the emulator holds writable handles against it (§2.9).
    image_vfs_notify_attached(filename);
    free(persistent_path);
}

// Add a SCSI CD-ROM to the configuration (AppleCD SC Plus / Sony CDU-8002)
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    // Persist volatile images to OPFS
    char *persistent_path = image_persist_volatile(filename);
    if (persistent_path)
        filename = persistent_path;

    // CD-ROM images are always opened read-only
    image_t *img = image_open_readonly(filename);
    if (!img) {
        printf("Failed to open CD-ROM image: %s\n", filename);
        free(persistent_path);
        return;
    }

    img->type = image_cdrom;

    printf("Attaching SCSI CD-ROM: %s as SONY CD-ROM CDU-8002 (size: %zu bytes, SCSI ID: %d)\n", filename,
           disk_size(img), scsi_id);

    add_image(config, img);
    scsi_add_device(config->scsi, scsi_id, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, 2048, true);
    image_vfs_notify_attached(filename);
    free(persistent_path);
}

// Save current machine state to a checkpoint file.
// Returns GS_SUCCESS on success, GS_ERROR on failure.
int system_checkpoint(const char *filename, checkpoint_kind_t kind) {
    if (!global_emulator) {
        printf("Error: No emulator instance to checkpoint\n");
        return GS_ERROR;
    }
    if (!global_emulator->machine || !global_emulator->machine->checkpoint_save) {
        printf("Error: Machine has no checkpoint_save callback\n");
        return GS_ERROR;
    }

    double start_time = host_time_ms();

    // Quick checkpoints store files as references (paths only), never content.
    bool prev_files_mode = checkpoint_get_files_as_refs();
    if (kind == CHECKPOINT_KIND_QUICK) {
        checkpoint_set_files_as_refs(true);
    }

    // Pass the machine model ID and RAM size so they're stored in the checkpoint header
    const char *model_id = global_emulator->machine->model_id;
    uint32_t ram_size_kb = global_emulator->ram_size / 1024;
    checkpoint_t *checkpoint = checkpoint_open_write(filename, kind, model_id, ram_size_kb);
    if (!checkpoint) {
        printf("Error: Failed to open checkpoint file for writing: %s\n", filename);
        return GS_ERROR;
    }

    // Delegate all state serialisation to the machine profile
    global_emulator->machine->checkpoint_save(global_emulator, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        printf("Error: Failed to write checkpoint\n");
        checkpoint_close(checkpoint);
        checkpoint_set_files_as_refs(prev_files_mode);
        return GS_ERROR;
    }

    checkpoint_close(checkpoint);
    checkpoint_set_files_as_refs(prev_files_mode);

    double elapsed_ms = host_time_ms() - start_time;
    printf("Checkpoint saved to %s (%.2f ms)\n", filename, elapsed_ms);
    return GS_SUCCESS;
}

// Restore machine state from a checkpoint file.
config_t *system_restore(const char *filename) {
    checkpoint_t *checkpoint = checkpoint_open_read(filename);
    if (!checkpoint) {
        printf("Error: Failed to open checkpoint file for reading: %s\n", filename);
        return NULL;
    }

    // Save the current global emulator so we can restore it on error.
    config_t *prev = global_emulator;

    // Determine machine profile from the checkpoint header, falling back to
    // the current machine or Plus for backward compatibility.
    const hw_profile_t *profile = NULL;
    const char *saved_model_id = checkpoint_get_model_id(checkpoint);
    if (saved_model_id && saved_model_id[0])
        profile = machine_find(saved_model_id);
    if (!profile)
        profile = (prev && prev->machine) ? prev->machine : machine_find("plus");
    if (!profile)
        profile = &machine_plus;

    // Restore the RAM size from the checkpoint so system_create uses the
    // correct size instead of the machine default.
    uint32_t saved_ram_kb = checkpoint_get_ram_size_kb(checkpoint);
    if (saved_ram_kb > 0)
        system_set_pending_ram_kb(saved_ram_kb);

    config_t *config = system_create(profile, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        printf("Error: Failed to read checkpoint\n");
        checkpoint_close(checkpoint);
        global_emulator = prev;
        if (config)
            system_destroy(config);
        return NULL;
    }

    checkpoint_close(checkpoint);
    printf("Checkpoint restored from %s\n", filename);
    return config;
}

// Command handlers for checkpoint operations
uint64_t cmd_save_checkpoint(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: checkpoint --save <filename> [content|refs]\n");
        return -1;
    }
    const char *filename = argv[1];
    bool prev_mode = checkpoint_get_files_as_refs();
    if (argc >= 3) {
        const char *mode = argv[2];
        if (strcmp(mode, "refs") == 0 || strcmp(mode, "reference") == 0 || strcmp(mode, "names") == 0) {
            checkpoint_set_files_as_refs(true);
        } else if (strcmp(mode, "content") == 0 || strcmp(mode, "inline") == 0) {
            checkpoint_set_files_as_refs(false);
        } else {
            printf("checkpoint --save: unknown mode '%s' (use 'content' or 'refs')\n", mode);
            return -1;
        }
    }
    int result = system_checkpoint(filename, CHECKPOINT_KIND_CONSOLIDATED);
    checkpoint_set_files_as_refs(prev_mode); // restore previous setting
    return result;
}

// Platform hook: find the path to the latest valid background checkpoint.
// Returns a static buffer with the path, or NULL if no valid checkpoint exists.
// The weak default returns NULL; the WASM platform overrides with actual scanning.
__attribute__((weak)) const char *find_valid_checkpoint_path(void) {
    return NULL;
}

// Shell command to load a saved checkpoint from file
// Also supports "checkpoint --probe" to check for a valid background checkpoint
uint64_t cmd_load_checkpoint(int argc, char *argv[]) {
    // Handle probe subcommand: return 0 if valid checkpoint exists, 1 otherwise
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        const char *path = find_valid_checkpoint_path();
        return (path != NULL) ? 0 : 1;
    }

    if (argc < 2) {
        // No filename argument: auto-load the latest valid checkpoint
        const char *auto_path = find_valid_checkpoint_path();
        if (!auto_path) {
            printf("No valid checkpoint found\n");
            return 1;
        }
        printf("Auto-loading checkpoint: %s\n", auto_path);
        argc = 2;
        argv[1] = (char *)auto_path;
    }

    const char *filename = argv[1];
    config_t *old_config = global_emulator;
    config_t *new_config = system_restore(filename);
    if (new_config) {
        // Replace global emulator with restored state
        global_emulator = new_config;
        // Free the old emulator state after replacing it
        // This is safe because:
        // 1. Commands are registered globally, not per-config
        // 2. global_emulator now points to new_config
        // 3. No part of the call stack holds direct references to old_config
        if (old_config) {
            system_destroy(old_config);
        }
        // Force a one-shot screen redraw so the restored framebuffer appears
        extern void frontend_force_redraw(void);
        frontend_force_redraw();

        // Resume execution if checkpoint was saved while running
        if (scheduler_is_running(new_config->scheduler)) {
            printf("Checkpoint was saved while running - resuming execution\n");
        }
        return 0;
    }
    return -1;
}

// ===== Phase 5b: argv-driven entry points for the typed object-model bridge =====
// Mirror the legacy `fd` / `hd` shell commands but skip find_cmd / shell_dispatch.

static int run_subcmd_handler(cmd_fn fn, const struct cmd_reg *reg, int argc, char **argv) {
    struct cmd_io io;
    init_cmd_io(&io);
    struct cmd_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = io.out_stream;
    ctx.err = io.err_stream;
    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    res.type = RES_OK;
    if (cmd_parse_args(argc, argv, reg, &ctx, &res))
        fn(&ctx, &res);
    finalize_cmd_io(&io, &res);
    if (res.type == RES_ERR) {
        if (res.as_str)
            fprintf(stderr, "%s\n", res.as_str);
        return -1;
    }
    if (res.type == RES_INT)
        return (int)res.as_int;
    return 0;
}

int shell_fd_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "fd",
        .fn = cmd_fd_handler,
        .subcmds = fd_subcmds,
        .n_subcmds = 5,
    };
    return run_subcmd_handler(cmd_fd_handler, &reg, argc, argv);
}

int shell_hd_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "hd",
        .fn = cmd_hd_handler,
        .subcmds = hd_subcmds,
        .n_subcmds = 6,
    };
    return run_subcmd_handler(cmd_hd_handler, &reg, argc, argv);
}

int shell_image_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "image",
        .fn = cmd_image_handler,
        .subcmds = image_subcmds,
        .n_subcmds = 4,
    };
    return run_subcmd_handler(cmd_image_handler, &reg, argc, argv);
}

// ---------------- AppleTalk command handlers ----------------
// AppleTalk CLI lives in appletalk.c
