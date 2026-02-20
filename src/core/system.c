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
#include "cpu.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "log.h"
#include "memory.h"
#include "mouse.h"
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

LOG_USE_CATEGORY_NAME("setup");

// Global emulator pointer (definition)
config_t *global_emulator = NULL;

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
    if (!global_emulator || !global_emulator->mouse)
        return;
    mouse_update(global_emulator->mouse, button, dx, dy);
}

// System-level keyboard input wrapper: routes input to appropriate keyboard device model
void system_keyboard_update(key_event_t event, int key) {
    if (!global_emulator || !global_emulator->keyboard)
        return;
    keyboard_update(global_emulator->keyboard, event, key);
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

// Forward declarations for command handlers
uint64_t cmd_attach_hd(int argc, char *argv[]);
uint64_t cmd_new_fd(int argc, char *argv[]);
uint64_t cmd_insert_fd(int argc, char *argv[]);
uint64_t cmd_insert_disk(int argc, char *argv[]);

// Trigger a vertical blanking interval event (delegates to machine callback)
void trigger_vbl(struct config *restrict config) {
    if (config && config->machine && config->machine->trigger_vbl) {
        config->machine->trigger_vbl(config);
    }
}

// new-fd [drive]
// Creates a new blank 800K double-sided floppy image in memory and inserts it
// into the preferred drive if provided (0 or 1) or the first free drive.
// Returns 0 on success, -1 on failure.
uint64_t cmd_new_fd(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: new-fd <path-to-new-disk> [drive:0|1]\n");
        return -1;
    }

    const char *path = argv[1];
    int preferred = -1;
    if (argc >= 3) {
        if (argv[2][0] == '0' || argv[2][0] == '1')
            preferred = argv[2][0] - '0';
        else {
            printf("new-fd: invalid drive '%s' (expected 0 or 1)\n", argv[2]);
            return -1;
        }
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("new-fd: emulator config not initialized.\n");
        return -1;
    }

    bool d0_free = !floppy_is_inserted(config->floppy, 0);
    bool d1_free = !floppy_is_inserted(config->floppy, 1);

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

    int rc = image_create_blank_floppy(path, false);
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

    floppy_insert(config->floppy, target, disk);
    printf("new-fd: created %s and inserted into drive %d.\n", path, target);
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

    if (!floppy_is_inserted(config->floppy, 0)) {
        floppy_insert(config->floppy, 0, disk);
        printf("Inserted disk into floppy drive 0.\n");
    } else if (!floppy_is_inserted(config->floppy, 1)) {
        floppy_insert(config->floppy, 1, disk);
        printf("Inserted disk into floppy drive 1.\n");
    } else {
        printf("Both floppy drives are already occupied.\n");
        return -1;
    }

    return 0;
}

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Ensure logging categories of interest appear in `log list` even before any messages are emitted.
    // shell_init() (called earlier) already invoked log_init(); categories default to level 0 (OFF).
    (void)log_register_category("appletalk");

    // Module-owned command registrations
    image_init(NULL); // No cross-module commands registered here

    // Cross-module commands (image+floppy, scsi+image) registered here
    register_cmd("insert-disk", "Configuration", "Insert a disk image", &cmd_insert_disk);
    register_cmd("new-fd", "Configuration", "Create blank 800K floppy file and insert: new-fd <path> [drive:0|1]",
                 &cmd_new_fd);
    register_cmd("insert-fd", "Configuration",
                 "Insert a disk image: insert-fd [--probe] <path> [drive:0|1] [writable:0|1]", &cmd_insert_fd);
    register_cmd("attach-hd", "Configuration", "Attach (SCSI) hard disk image: attach-hd <path> [scsi-id]",
                 &cmd_attach_hd);

    // Register checkpoint commands
    register_cmd("save-state", "Checkpointing", "Save machine state to checkpoint file", &cmd_save_checkpoint);
    register_cmd("load-state", "Checkpointing", "Load machine state: load-state [<file>|probe]", &cmd_load_checkpoint);
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

    // Delegate all machine-specific initialisation to the profile
    profile->init(cfg, checkpoint);

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

    // Probe mode: check if file is a valid floppy image
    if (probe_only) {
        image_t *disk = image_open(path, false);
        if (!disk) {
            printf("%s: NOT a valid floppy image\n", path);
            return 1;
        }

        // Check if it's actually a floppy (not a hard disk)
        if (disk->type != image_fd_ss && disk->type != image_fd_ds && disk->type != image_fd_hd) {
            printf("%s: Valid disk image but not a floppy (size: %zu bytes)\n", path, disk->raw_size);
            image_close(disk);
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
        return 0;
    }

    // Normal insertion mode
    int preferred = -1;
    if (argc >= path_idx + 2) {
        if (argv[path_idx + 1][0] == '0' || argv[path_idx + 1][0] == '1') {
            preferred = argv[path_idx + 1][0] - '0';
        } else {
            printf("insert-fd: invalid drive '%s' (expected 0 or 1)\n", argv[path_idx + 1]);
            return -1;
        }
    }

    // Optional writable flag (default: false/read-only)
    bool writable = false;
    if (argc >= path_idx + 3) {
        if (argv[path_idx + 2][0] == '0') {
            writable = false;
        } else if (argv[path_idx + 2][0] == '1') {
            writable = true;
        } else {
            printf("insert-fd: invalid writable flag '%s' (expected 0 or 1)\n", argv[path_idx + 2]);
            return -1;
        }
    }

    image_t *disk = image_open(path, writable);
    if (!disk) {
        printf("insert-fd: failed to open disk image: %s\n", path);
        return -1;
    }

    config_t *config = global_emulator;
    if (!config) {
        printf("insert-fd: emulator config not initialized.\n");
        return -1;
    }

    bool d0_free = !floppy_is_inserted(config->floppy, 0);
    bool d1_free = !floppy_is_inserted(config->floppy, 1);

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
        return -1;
    }

    // Track this image so checkpoints can restore by filename
    add_image(config, disk);

    floppy_insert(config->floppy, target, disk);
    printf("insert-fd: inserted %s into floppy drive %d.\n", path, target);
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

    image_t *img = image_open(filename, true);
    if (!img) {
        printf("Failed to open image: %s\n", filename);
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
    // If no larger found, use the largest
    if (best == -1)
        best = num_disks - 1;

    printf("Attaching SCSI drive: %s as %s %s (size: %zu bytes, SCSI ID: %d)\n", filename, disks[best].vendor,
           disks[best].product, sz, scsi_id);

    // Add to config's image list
    add_image(config, img);

    // Attach to SCSI bus
    scsi_add_device(config->scsi, scsi_id, disks[best].vendor, disks[best].product, img);
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

    // For now, always restore as Plus (M6 will add ROM-based machine selection).
    extern const hw_profile_t machine_plus;
    config_t *config = system_create(&machine_plus, checkpoint);

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
        printf("Usage: save-state <filename> [content|refs]\n");
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
            printf("save-state: unknown mode '%s' (use 'content' or 'refs')\n", mode);
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
// Also supports "load-state probe" to check for a valid background checkpoint
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
