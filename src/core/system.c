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
#include "cmd_types.h"
#include "cpu.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "log.h"
#include "machine.h"
#include "memory.h"
#include "mouse.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "sound.h"
#include "via.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

LOG_USE_CATEGORY_NAME("setup");

// Global emulator pointer (definition)
config_t *global_emulator = NULL;

// Pending RAM override (KB). Set by `setup --ram` or headless `ram=` arg.
// Consumed by system_create(); 0 means use machine default.
static uint32_t g_pending_ram_kb = 0;

void system_set_pending_ram_kb(uint32_t kb) {
    g_pending_ram_kb = kb;
}
uint32_t system_get_pending_ram_kb(void) {
    return g_pending_ram_kb;
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

// Forward declarations for command handlers
uint64_t cmd_attach_hd(int argc, char *argv[]);
uint64_t cmd_new_fd(int argc, char *argv[]);
uint64_t cmd_insert_fd(int argc, char *argv[]);
uint64_t cmd_insert_disk(int argc, char *argv[]);
uint64_t cmd_setup(int argc, char *argv[]);

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

// new-fd [--hd] [drive]
// Creates a new blank floppy image (800K default, 1440K with --hd) and inserts
// it into the preferred drive if provided (0 or 1) or the first free drive.
// Returns 0 on success, -1 on failure.
uint64_t cmd_new_fd(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: new-fd [--hd] <path-to-new-disk> [drive:0|1]\n");
        return -1;
    }

    // Parse optional --hd flag for 1.4MB high-density floppy
    bool high_density = false;
    int path_arg = 1;
    if (argc >= 3 && strcmp(argv[1], "--hd") == 0) {
        high_density = true;
        path_arg = 2;
    }

    if (path_arg >= argc) {
        printf("Usage: new-fd [--hd] <path-to-new-disk> [drive:0|1]\n");
        return -1;
    }

    const char *path = argv[path_arg];
    int preferred = -1;
    if (path_arg + 1 < argc) {
        if (argv[path_arg + 1][0] == '0' || argv[path_arg + 1][0] == '1')
            preferred = argv[path_arg + 1][0] - '0';
        else {
            printf("new-fd: invalid drive '%s' (expected 0 or 1)\n", argv[path_arg + 1]);
            return -1;
        }
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("new-fd: emulator config not initialized.\n");
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
        printf("new-fd: both floppy drives are already occupied.\n");
        return -1;
    }

    int rc = image_create_blank_floppy(path, false, high_density);
    if (rc != 0) {
        if (rc == -2)
            printf("new-fd: file already exists: %s (won't overwrite)\n", path);
        else
            printf("new-fd: failed to create blank floppy file: %s\n", path);
        return -1;
    }

    image_t *disk = image_open(path, true);
    if (!disk) {
        printf("new-fd: failed to open newly created image: %s\n", path);
        return -1;
    }

    // Track this image so checkpoints can restore by filename
    add_image(config, disk);

    sys_fd_insert(config, target, disk);
    printf("new-fd: created %s (%s) and inserted into drive %d.\n", path, high_density ? "1440K" : "800K", target);
    return 0;
}

// Insert a disk image into the floppy drive
uint64_t cmd_insert_disk(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: insert-disk <path-to-disk-image>\n");
        return -1;
    }

    const char *path = argv[1];
    image_t *disk = image_open(path, true);

    if (!disk) {
        printf("Failed to open disk image: %s\n", path);
        return -1;
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("Emulator config not initialized.\n");
        return -1;
    }

    // Track this image so checkpoints can restore by filename
    add_image(config, disk);

    if (!sys_fd_is_inserted(config, 0)) {
        sys_fd_insert(config, 0, disk);
        printf("Inserted disk into floppy drive 0.\n");
    } else if (!sys_fd_is_inserted(config, 1)) {
        sys_fd_insert(config, 1, disk);
        printf("Inserted disk into floppy drive 1.\n");
    } else {
        printf("Both floppy drives are already occupied.\n");
        return -1;
    }

    return 0;
}

// Query or configure the active machine.
// setup                              → print current machine info
// setup --model <model> [--ram <kb>] → teardown current machine, create new one
uint64_t cmd_setup(int argc, char *argv[]) {
    if (argc < 2) {
        // No args: print current machine info
        if (!global_emulator || !global_emulator->machine) {
            printf("No machine instantiated\n");
            return 0;
        }
        const hw_profile_t *m = global_emulator->machine;
        printf("Machine: %s (%s)\n", m->model_name, m->model_id);
        printf("  CPU: 680%02d @ %.4f MHz\n", m->cpu_model / 1000, m->cpu_clock_hz / 1000000.0);
        printf("  Address: %d-bit, RAM: %u KB, ROM: %u KB\n", m->address_bits, global_emulator->ram_size / 1024,
               m->rom_size / 1024);
        printf("  VIAs: %d, ADB: %s\n", m->via_count, m->has_adb ? "yes" : "no");
        return 0;
    }

    // Parse --model and --ram options
    const char *model_id = NULL;
    uint32_t ram_kb = 0;
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--model") == 0) {
            if (++i >= argc) {
                printf("setup: --model requires an argument\n");
                return -1;
            }
            model_id = argv[i];
        } else if (strcmp(argv[i], "--ram") == 0) {
            if (++i >= argc) {
                printf("setup: --ram requires an argument (size in KB)\n");
                return -1;
            }
            ram_kb = (uint32_t)strtoul(argv[i], NULL, 10);
            if (ram_kb == 0) {
                printf("setup: invalid RAM size '%s'\n", argv[i]);
                return -1;
            }
        } else {
            printf("setup: unknown option '%s'\n", argv[i]);
            printf("Usage: setup [--model <model>] [--ram <kb>]\n");
            return -1;
        }
        i++;
    }

    if (!model_id) {
        printf("Usage: setup [--model <model>] [--ram <kb>]\n");
        return -1;
    }

    // Look up the requested machine profile
    const hw_profile_t *profile = machine_find(model_id);
    if (!profile) {
        printf("setup: unknown model '%s'\n", model_id);
        return -1;
    }

    // Validate RAM against machine limits
    if (ram_kb > 0) {
        uint32_t max_kb = profile->ram_size_max / 1024;
        if (ram_kb > max_kb) {
            printf("setup: RAM %u KB exceeds maximum %u KB for %s\n", ram_kb, max_kb, profile->model_name);
            return -1;
        }
        g_pending_ram_kb = ram_kb;
    }

    // Teardown existing machine if one is active
    if (global_emulator) {
        system_destroy(global_emulator);
        global_emulator = NULL;
    }

    // Create the new machine (cold boot)
    config_t *cfg = system_create(profile, NULL);
    if (!cfg) {
        printf("setup: failed to create %s\n", model_id);
        return -1;
    }

    printf("Machine created: %s (%s), RAM: %u KB\n", profile->model_name, profile->model_id, cfg->ram_size / 1024);
    return 0;
}

// Enable/disable/query SCC external loopback (cable between port A and port B)
static uint64_t cmd_scc_loopback(int argc, char *argv[]) {
    if (!global_emulator || !global_emulator->scc) {
        printf("No SCC available\n");
        return (uint64_t)-1;
    }
    if (argc < 2) {
        // query — print current state with hint (IMP-207)
        const char *state = scc_get_external_loopback(global_emulator->scc) ? "on" : "off";
        printf("scc-loopback: %s\n", state);
        printf("  (Use \"scc-loopback on\" or \"scc-loopback off\" to change.)\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        scc_set_external_loopback(global_emulator->scc, true);
        printf("SCC external loopback enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        scc_set_external_loopback(global_emulator->scc, false);
        printf("SCC external loopback disabled\n");
    } else {
        printf("Usage: scc-loopback [on|off]\n");
        return (uint64_t)-1;
    }
    return 0;
}

// Enable/disable/query SCSI loopback test card (passive bus terminator)
static uint64_t cmd_scsi_loopback(int argc, char *argv[]) {
    if (!global_emulator || !global_emulator->scsi) {
        printf("No SCSI controller available\n");
        return (uint64_t)-1;
    }
    if (argc < 2) {
        // query — print current state with hint (IMP-207)
        const char *state = scsi_get_loopback(global_emulator->scsi) ? "on" : "off";
        printf("scsi-loopback: %s\n", state);
        printf("  (Use \"scsi-loopback on\" or \"scsi-loopback off\" to change.)\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        scsi_set_loopback(global_emulator->scsi, true);
        printf("SCSI loopback test card enabled\n");
    } else if (strcmp(argv[1], "off") == 0) {
        scsi_set_loopback(global_emulator->scsi, false);
        printf("SCSI loopback test card disabled\n");
    } else {
        printf("Usage: scsi-loopback [on|off]\n");
        return (uint64_t)-1;
    }
    return 0;
}

// ============================================================================
// Command handlers for disk, scsi, scc, setup, rom, vrom
// ============================================================================

// --- disk (unified: insert/create/eject/probe) ---
static void cmd_disk_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: disk <insert|create|probe> <path>");
        return;
    }

    if (strcmp(subcmd, "insert") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: disk insert <path>");
            return;
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "insert-disk %s", ctx->args[0].as_str);
        uint64_t r = shell_dispatch(buf);
        cmd_int(res, (int64_t)r);
        return;
    }
    if (strcmp(subcmd, "create") == 0) {
        // Check for --hd flag
        int has_hd = 0;
        for (int i = 0; i < ctx->raw_argc; i++) {
            if (strcmp(ctx->raw_argv[i], "--hd") == 0) {
                has_hd = 1;
                break;
            }
        }
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: disk create [--hd] <path>");
            return;
        }
        char buf[512];
        if (has_hd)
            snprintf(buf, sizeof(buf), "new-fd --hd %s", ctx->args[0].as_str);
        else
            snprintf(buf, sizeof(buf), "new-fd %s", ctx->args[0].as_str);
        uint64_t r = shell_dispatch(buf);
        cmd_int(res, (int64_t)r);
        return;
    }
    if (strcmp(subcmd, "probe") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: disk probe <path>");
            return;
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "insert-fd --probe %s", ctx->args[0].as_str);
        uint64_t r = shell_dispatch(buf);
        cmd_int(res, (int64_t)r);
        return;
    }
    if (strcmp(subcmd, "eject") == 0) {
        cmd_printf(ctx, "disk eject: not yet implemented\n");
        cmd_ok(res);
        return;
    }
    cmd_err(res, "unknown disk subcommand: %s", subcmd);
}

// --- scsi (unified: attach/loopback) ---
static void cmd_scsi_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: scsi <attach|loopback> [args...]");
        return;
    }

    if (strcmp(subcmd, "attach") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: scsi attach <path> [id]");
            return;
        }
        char buf[512];
        if (ctx->args[1].present)
            snprintf(buf, sizeof(buf), "attach-hd %s %lld", ctx->args[0].as_str, (long long)ctx->args[1].as_int);
        else
            snprintf(buf, sizeof(buf), "attach-hd %s", ctx->args[0].as_str);
        uint64_t r = shell_dispatch(buf);
        cmd_int(res, (int64_t)r);
        return;
    }
    if (strcmp(subcmd, "loopback") == 0) {
        if (ctx->args[0].present) {
            char buf[64];
            snprintf(buf, sizeof(buf), "scsi-loopback %s", ctx->args[0].as_str);
            uint64_t r = shell_dispatch(buf);
            cmd_int(res, (int64_t)r);
        } else {
            uint64_t r = shell_dispatch("scsi-loopback");
            cmd_int(res, (int64_t)r);
        }
        return;
    }
    cmd_err(res, "unknown scsi subcommand: %s", subcmd);
}

// --- scc (unified) ---
static void cmd_scc_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: scc loopback [on|off]");
        return;
    }

    if (strcmp(subcmd, "loopback") == 0) {
        if (ctx->args[0].present) {
            char buf[64];
            snprintf(buf, sizeof(buf), "scc-loopback %s", ctx->args[0].as_str);
            uint64_t r = shell_dispatch(buf);
            cmd_int(res, (int64_t)r);
        } else {
            uint64_t r = shell_dispatch("scc-loopback");
            cmd_int(res, (int64_t)r);
        }
        return;
    }
    cmd_err(res, "unknown scc subcommand: %s", subcmd);
}

// --- setup ---
static void cmd_set_handlerup(struct cmd_context *ctx, struct cmd_result *res) {
    uint64_t r = cmd_setup(ctx->raw_argc, ctx->raw_argv);
    cmd_int(res, (int64_t)r);
}

// --- rom ---
static void cmd_rom_handler(struct cmd_context *ctx, struct cmd_result *res) {
    uint64_t r = cmd_rom(ctx->raw_argc, ctx->raw_argv);
    cmd_int(res, (int64_t)r);
}

// --- vrom ---
static void cmd_vrom_handler(struct cmd_context *ctx, struct cmd_result *res) {
    uint64_t r = cmd_vrom(ctx->raw_argc, ctx->raw_argv);
    cmd_int(res, (int64_t)r);
}

// Registration tables

// disk subcommands
static const struct arg_spec disk_path_args[] = {
    {"path",  ARG_PATH,               "disk image path"},
    {"drive", ARG_INT | ARG_OPTIONAL, "drive number"   },
};
static const struct subcmd_spec disk_subcmds[] = {
    {"insert", NULL, disk_path_args, 1, "auto-detect and insert"    },
    {"create", NULL, disk_path_args, 1, "create blank floppy"       },
    {"probe",  NULL, disk_path_args, 1, "validate without inserting"},
    {"eject",  NULL, NULL,           0, "eject disk (future)"       },
};

// scsi subcommands
static const struct arg_spec scsi_attach_args[] = {
    {"path", ARG_PATH,               "hard disk image path"},
    {"id",   ARG_INT | ARG_OPTIONAL, "SCSI ID"             },
};
static const char *loopback_values[] = {"on", "off", NULL};
static const struct arg_spec loopback_args[] = {
    {"state", ARG_ENUM | ARG_OPTIONAL, "on or off", loopback_values},
};
static const struct subcmd_spec scsi_subcmds[] = {
    {"attach",   NULL, scsi_attach_args, 2, "attach hard disk image"},
    {"loopback", NULL, loopback_args,    1, "passive terminator"    },
};

// scc subcommands
static const struct subcmd_spec scc_subcmds[] = {
    {"loopback", NULL, loopback_args, 1, "external loopback on/off"},
};

// setup args
static const struct arg_spec setup_args[] = {
    {"options", ARG_REST | ARG_OPTIONAL, "setup options (--model, --ram)"},
};

// rom/vrom args
static const struct arg_spec rom_args[] = {
    {"options", ARG_REST | ARG_OPTIONAL, "rom options (--load, --checksum, --probe)"},
};

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Register built-in machine profiles so machine_find() can look them up
    machine_register(&machine_plus);
    machine_register(&machine_se30);

    // Ensure logging categories of interest appear in `log list` even before any messages are emitted.
    // shell_init() (called earlier) already invoked log_init(); categories default to level 0 (OFF).
    (void)log_register_category("appletalk");

    // Module-owned command registrations
    image_init(NULL); // No cross-module commands registered here

    // Simple commands (used for internal delegation from unified commands)
    register_cmd("insert-disk", "Configuration", "insert-disk <path> — auto-detect and insert a floppy disk image",
                 &cmd_insert_disk);
    register_cmd("new-fd", "Configuration", "Create blank floppy and insert: new-fd [--hd] <path> [drive:0|1]",
                 &cmd_new_fd);
    register_cmd("insert-fd", "Configuration",
                 "insert-fd [--probe] <path> [drive:0|1] [writable:0|1] — insert floppy with options", &cmd_insert_fd);
    register_cmd("attach-hd", "Configuration", "Attach (SCSI) hard disk image: attach-hd <path> [scsi-id]",
                 &cmd_attach_hd);
    register_cmd("scc-loopback", "Configuration", "scc-loopback [on|off] – enable/disable external loopback",
                 &cmd_scc_loopback);
    register_cmd("scsi-loopback", "Configuration", "scsi-loopback [on|off] – enable/disable SCSI loopback",
                 &cmd_scsi_loopback);

    // Unified commands
    register_command(&(struct cmd_reg){
        .name = "disk",
        .category = "Media",
        .synopsis = "Manage floppy disks (insert/create/eject)",
        .fn = cmd_disk_handler,
        .subcmds = disk_subcmds,
        .n_subcmds = 4,
    });
    register_command(&(struct cmd_reg){
        .name = "scsi",
        .category = "Media",
        .synopsis = "Manage SCSI devices (attach/loopback)",
        .fn = cmd_scsi_handler,
        .subcmds = scsi_subcmds,
        .n_subcmds = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "setup",
        .category = "Configuration",
        .synopsis = "Query or configure machine model",
        .fn = cmd_set_handlerup,
        .args = setup_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "rom",
        .category = "Media",
        .synopsis = "Load or probe ROM image",
        .fn = cmd_rom_handler,
        .args = rom_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "vrom",
        .category = "Media",
        .synopsis = "Load or probe VROM image",
        .fn = cmd_vrom_handler,
        .args = rom_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "scc",
        .category = "Configuration",
        .synopsis = "SCC serial port configuration",
        .fn = cmd_scc_handler,
        .subcmds = scc_subcmds,
        .n_subcmds = 1,
    });
}

// Platform hook called after system_create completes.
// The weak default is a no-op; the WASM platform overrides to install
// the assertion callback (which requires the debug object to exist).
__attribute__((weak)) void system_post_create(config_t *cfg) {
    (void)cfg;
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

    // Notify the platform (e.g., install assertion callback)
    system_post_create(cfg);

    return cfg;
}

// Destroy an emulator instance: call machine teardown and free all resources.
void system_destroy(config_t *config) {
    if (!config)
        return;

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

// new-fd [drive] — insert-fd variant
uint64_t cmd_insert_fd(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: insert-fd [--probe] <path-to-disk-image> [drive:0|1] [writable:0|1]\n");
        printf("  --probe    Test if file is a valid floppy image without inserting\n");
        return -1;
    }

    // Check for --probe option
    bool probe_only = false;
    int path_idx = 1;
    if (argc >= 2 && (strcmp(argv[1], "--probe") == 0 || strcmp(argv[1], "-p") == 0)) {
        probe_only = true;
        path_idx = 2;
        if (argc < 3) {
            printf("Usage: insert-fd --probe <path-to-disk-image>\n");
            return -1;
        }
    }

    const char *path = argv[path_idx];

    // Persist volatile images (/tmp/, /fd/) to OPFS so they survive page reload
    char *persistent_path = image_persist_volatile(path);
    if (persistent_path)
        path = persistent_path;

    // Probe mode: check if file is a valid floppy image
    if (probe_only) {
        image_t *disk = image_open(path, false);
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

    // Normal insertion mode
    int preferred = -1;
    if (argc >= path_idx + 2) {
        if (argv[path_idx + 1][0] == '0' || argv[path_idx + 1][0] == '1') {
            preferred = argv[path_idx + 1][0] - '0';
        } else {
            printf("insert-fd: invalid drive '%s' (expected 0 or 1)\n", argv[path_idx + 1]);
            free(persistent_path);
            return -1;
        }
    }

    bool writable = false;
    if (argc >= path_idx + 3) {
        if (argv[path_idx + 2][0] == '0') {
            writable = false;
        } else if (argv[path_idx + 2][0] == '1') {
            writable = true;
        } else {
            printf("insert-fd: invalid writable flag '%s' (expected 0 or 1)\n", argv[path_idx + 2]);
            free(persistent_path);
            return -1;
        }
    }

    image_t *disk = image_open(path, writable);
    if (!disk) {
        printf("insert-fd: failed to open disk image: %s\n", path);
        free(persistent_path);
        return -1;
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("insert-fd: emulator config not initialized.\n");
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
        printf("insert-fd: both floppy drives are already occupied.\n");
        free(persistent_path);
        return -1;
    }

    add_image(config, disk);
    sys_fd_insert(config, target, disk);
    printf("insert-fd: inserted %s into floppy drive %d.\n", path, target);
    free(persistent_path);
    return 0;
}

// Add a SCSI drive to the configuration
void add_scsi_drive(struct config *restrict config, const char *filename, int scsi_id) {
    // Disk table: Model, Vendor, Product, Size
    struct disk_info {
        const char *vendor;
        const char *product;
        size_t size;
    } disks[] = {
        {"SEAGATE",  "ST225N",   21411840 }, // HD20SC
        {"MINISCRB", "8425S",    21307392 }, // Miniscribe 8425S
        {"CONNER",   "CP3040",   42881664 }, // HD40SC
        {"QUANTUM",  "PRODRIVE", 81222144 }, // HD80SC
        {"QUANTUM",  "LPS170S",  177270240}  // HD160SC
    };
    const int num_disks = sizeof(disks) / sizeof(disks[0]);

    // Persist volatile images to OPFS
    char *persistent_path = image_persist_volatile(filename);
    if (persistent_path)
        filename = persistent_path;

    image_t *img = image_open(filename, true);
    if (!img) {
        printf("Failed to open image: %s\n", filename);
        free(persistent_path);
        return;
    }

    size_t sz = disk_size(img);

    // Find the closest exact or next-larger match
    int best = -1;
    for (int i = 0; i < num_disks; ++i) {
        if (sz <= disks[i].size) {
            best = i;
            break;
        }
    }
    if (best == -1)
        best = num_disks - 1;

    printf("Attaching SCSI drive: %s as %s %s (size: %zu bytes, SCSI ID: %d)\n", filename, disks[best].vendor,
           disks[best].product, sz, scsi_id);

    add_image(config, img);
    scsi_add_device(config->scsi, scsi_id, disks[best].vendor, disks[best].product, img);
    free(persistent_path);
}

// attach-hd <path-to-image> [scsi-id]
// Opens the image and attaches it as a SCSI device using add_scsi_drive().
// Optional scsi-id defaults to 0. Returns 0 on success, -1 on error.
uint64_t cmd_attach_hd(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: attach-hd <path-to-image> [scsi-id]\n");
        return -1;
    }
    const char *path = argv[1];
    int scsi_id = 0;
    if (argc >= 3) {
        scsi_id = argv[2][0] - '0';
        if (scsi_id < 0 || scsi_id > 7) {
            printf("attach-hd: invalid scsi id '%s' (expected 0..7)\n", argv[2]);
            return -1;
        }
    }
    config_t *config = global_emulator;
    if (!config) {
        printf("attach-hd: emulator config not initialized.\n");
        return -1;
    }
    // add_scsi_drive handles image_open + scsi_add_device logic.
    add_scsi_drive(config, path, scsi_id);
    return 0;
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

    checkpoint_t *checkpoint = checkpoint_open_write(filename, kind);
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

    // Determine machine profile: use the current machine if one is active,
    // otherwise default to Plus for backward compatibility with old checkpoints.
    const hw_profile_t *profile = (prev && prev->machine) ? prev->machine : machine_find("plus");
    if (!profile)
        profile = &machine_plus;
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

// ---------------- AppleTalk command handlers ----------------
// AppleTalk CLI lives in appletalk.c
