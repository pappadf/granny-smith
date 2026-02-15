// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.c
// Main emulator orchestration: initialization, shutdown, and device coordination.

#include "system.h"

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
    return global_emulator ? global_emulator->new_cpu : NULL;
}

// System-level framebuffer accessor: returns pointer to video RAM buffer
uint8_t *system_framebuffer(void) {
    return global_emulator ? global_emulator->ram_vbuf : NULL;
}

// Check if emulator is initialized and running
bool system_is_initialized(void) {
    return global_emulator != NULL;
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
// Forward declarations for command handlers
uint64_t cmd_attach_hd(int argc, char *argv[]);
uint64_t cmd_new_fd(int argc, char *argv[]);
uint64_t cmd_insert_fd(int argc, char *argv[]);
uint64_t cmd_insert_disk(int argc, char *argv[]);

// Trigger a vertical blanking interval event
void trigger_vbl(struct config *restrict config) {
    struct scheduler *s = config->scheduler;

    // Trigger the vertical blanking interrupt
    via_input_c(config->new_via, 0, 0, 0);
    via_input_c(config->new_via, 0, 0, 1);

    // If we have a sound system, trigger the sound VBL
    if (config->sound2) {
        sound_vbl(config->sound2);
    }

    image_tick_all(config);
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

// Switch between main and alternate video buffer addresses
void video_use_buffer(bool main) {
    uint32_t addr;

    if (main) {
        addr = 0x400000 - 0x5900;
    } else {

        addr = 0x400000 - 0x5900 - 0x8000;
    }

    global_emulator->ram_vbuf = ram_native_pointer(global_emulator->mem_map, addr);
}

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Ensure logging categories of interest appear in `log list` even before any messages are emitted.
    // shell_init() (called earlier) already invoked log_init(); categories default to level 0 (OFF).
    (void)log_register_category("appletalk");

    // Module-owned command registrations
    // Note: appletalk_init moved to setup_plus after SCC creation
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

// Forward declarations for Plus-specific callbacks
static void plus_via_output(void *context, uint8_t port, uint8_t output);
static void plus_via_shift_out(void *context, uint8_t byte);
static void plus_via_irq(void *context, bool active);
static void plus_scc_irq(void *context, bool active);

// Set up a Mac Plus emulator configuration
config_t *setup_plus(checkpoint_t *checkpoint) {

    config_t *sim;

    sim = malloc(sizeof(config_t));
    if (sim == NULL) {
        return (NULL);
    }

    memset(sim, 0, sizeof(config_t));

    global_emulator = sim;

    sim->mem_map = memory_map_init(checkpoint);

    sim->mem_iface = memory_map_interface(sim->mem_map);

    sim->new_cpu = cpu_init(checkpoint);

    sim->scheduler = scheduler_init(sim->new_cpu, checkpoint);

    // Restore global interrupt state in the same order they were saved
    // (right after scheduler, before RTC and the rest of devices).
    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &sim->irq, sizeof(sim->irq));
    }

    sim->new_rtc = rtc_init(sim->scheduler, checkpoint);

    sim->new_scc = scc_init(sim->mem_map, sim->scheduler, plus_scc_irq, sim, checkpoint);

    // Initialize AppleTalk with scheduler and SCC dependencies (registers shell commands)
    appletalk_init(sim->scheduler, sim->new_scc, NULL);

    // sim->video = mac_video_new(512, 342);

    sim->sound2 = sound_init(sim->mem_map, checkpoint);

    sim->new_via =
        via_init(sim->mem_map, sim->scheduler, plus_via_output, plus_via_shift_out, plus_via_irq, sim, checkpoint);

    rtc_set_via(sim->new_rtc, sim->new_via);

    sim->mouse = mouse_init(sim->scheduler, sim->new_scc, sim->new_via, checkpoint);

    // Restore image list from checkpoint before devices that may reference them
    if (checkpoint) {
        uint32_t count = 0;
        system_read_checkpoint_data(checkpoint, &count, sizeof(count));
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            char *name = NULL;
            if (len > 0) {
                name = (char *)malloc(len);
                if (!name) {
                    char tmp;
                    for (uint32_t k = 0; k < len; ++k)
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                } else {
                    system_read_checkpoint_data(checkpoint, name, len);
                }
            }
            char writable = 0;
            system_read_checkpoint_data(checkpoint, &writable, sizeof(writable));
            // Read raw image size (written by image_checkpoint)
            uint64_t raw_size = 0;
            system_read_checkpoint_data(checkpoint, &raw_size, sizeof(raw_size));
            image_t *img = NULL;
            if (name) {
                // For consolidated checkpoints the embedded data is authoritative;
                // always recreate the backing file so stale/missing/wrong-size
                // files on disk are replaced with a clean image of the correct size.
                if (raw_size > 0 && checkpoint_get_kind(checkpoint) == CHECKPOINT_KIND_CONSOLIDATED) {
                    image_create_empty(name, (size_t)raw_size);
                }
                img = image_open(name, writable != 0);
                if (!img) {
                    printf("Error: image_open failed for %s while restoring checkpoint\n", name);
                    checkpoint_set_error(checkpoint);
                }
            }
            if (storage_restore_from_checkpoint(img ? img->storage : NULL, checkpoint) != GS_SUCCESS) {
                printf("Error: storage_restore_from_checkpoint failed for %s\n", name ? name : "<unnamed>");
                checkpoint_set_error(checkpoint);
            }
            if (img) {
                add_image(sim, img);
            }
            if (name) {
                free(name);
            }
        }
    }

    sim->new_scsi = scsi_init(sim->mem_map, checkpoint);

    setup_images(sim);

    sim->keyboard = keyboard_init(sim->scheduler, sim->new_scc, sim->new_via, checkpoint);

    // Initialize floppy last to match checkpoint save order
    sim->floppy = floppy_init(sim->mem_map, sim->scheduler, checkpoint);

    // After floppy exists, if restoring from a checkpoint, re-drive VIA outputs
    // so SEL and other external signals propagate to the floppy.
    if (checkpoint) {
        via_redrive_outputs(sim->new_via);
    }

    // sim->new_cpu->video = sim->video;

    // On cold boot, default to main video buffer. When restoring from a checkpoint,
    // VIA outputs will re-drive the selection, so don't override it here.
    if (!checkpoint) {
        video_use_buffer(1);
    }

    sim->debugger = debug_init();

    scheduler_start(global_emulator->scheduler);

    // Initialize IRQ/IPL only for cold boot. On checkpoint restore, devices
    // (VIA, SCC) already re-assert their interrupts; don't clobber them.
    if (!checkpoint) {
        sim->irq = 0;
        cpu_set_ipl(sim->new_cpu, 0);
    }

    return (sim);
}

// Tear down and free all emulator resources
void setup_teardown(config_t *config) {
    if (!config)
        return;

    // stop scheduler if running (best effort)
    if (config->scheduler) {
        scheduler_stop(config->scheduler);
    }

    if (config->keyboard) {
        keyboard_delete(config->keyboard);
        config->keyboard = NULL;
    }
    if (config->new_scsi) {
        scsi_delete(config->new_scsi);
        config->new_scsi = NULL;
    }
    if (config->mouse) {
        mouse_delete(config->mouse);
        config->mouse = NULL;
    }
    if (config->new_via) {
        via_delete(config->new_via);
        config->new_via = NULL;
    }
    if (config->sound2) {
        sound_delete(config->sound2);
        config->sound2 = NULL;
    }
    if (config->new_scc) {
        scc_delete(config->new_scc);
        config->new_scc = NULL;
    }
    if (config->new_rtc) {
        rtc_delete(config->new_rtc);
        config->new_rtc = NULL;
    }
    if (config->scheduler) {
        scheduler_delete(config->scheduler);
        config->scheduler = NULL;
    }
    if (config->floppy) {
        floppy_delete(config->floppy);
        config->floppy = NULL;
    }
    if (config->new_cpu) {
        cpu_delete(config->new_cpu);
        config->new_cpu = NULL;
    }
    if (config->mem_map) {
        memory_map_delete(config->mem_map);
        config->mem_map = NULL;
    }
    if (config->debugger) {
        debug_cleanup(config->debugger);
        config->debugger = NULL;
    }

    // Free all tracked images
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
    scc_reset(sim->new_scc);
}

// Plus-specific VIA output callback: routes port changes to floppy, video, sound, RTC
static void plus_via_output(void *context, uint8_t port, uint8_t output) {
    config_t *sim = (config_t *)context;

    if (port == 0) {
        floppy_set_sel_signal(sim->floppy, (output & 0x20) != 0);

        video_use_buffer(output >> 6 & 1);

        sound_use_buffer(sim->sound2, output >> 3 & 1);

        sound_volume(sim->sound2, output & 7);
    } else {
        rtc_input(sim->new_rtc, output >> 2 & 1, output >> 1 & 1, output & 1);

        sound_enable(sim->sound2, (output & 0x80) == 0);
    }
}

// Plus-specific VIA shift-out callback: routes keyboard data
static void plus_via_shift_out(void *context, uint8_t byte) {
    config_t *sim = (config_t *)context;
    keyboard_input(sim->keyboard, byte);
}

// Plus-specific interrupt routing: update CPU IPL from VIA or SCC IRQ changes
static void plus_update_ipl(config_t *sim, int level, bool value) {
    int old_irq = sim->irq;
    int old_ipl = cpu_get_ipl(sim->new_cpu);
    if (value)
        sim->irq |= level;
    else
        sim->irq &= ~level;

    // Guide to the Macintosh Family Hardware, chapter 3:
    // The interrupt request line from the VIA goes to the PALs,
    // which can assert an interrupt to the processor on line /IPL0.
    // The PALs also monitor interrupt line /IPL1,
    // and deassert /IPL0 whenever /IPL1 is asserted.

    uint32_t new_ipl;
    if (sim->irq > 1)
        new_ipl = 2;
    else if (sim->irq == 1)
        new_ipl = 1;
    else
        new_ipl = 0;
    cpu_set_ipl(sim->new_cpu, new_ipl);

    LOG(1, "plus_update_ipl: level=%d value=%d irq:%d->%d ipl:%d->%d", level, value ? 1 : 0, old_irq, sim->irq, old_ipl,
        new_ipl);

    cpu_reschedule();
}

// Plus-specific VIA IRQ callback: VIA uses IPL level 1
static void plus_via_irq(void *context, bool active) {
    plus_update_ipl((config_t *)context, 1, active);
}

// Plus-specific SCC IRQ callback: SCC uses IPL level 2
static void plus_scc_irq(void *context, bool active) {
    plus_update_ipl((config_t *)context, 2, active);
}

// new-fd [drive]
// Creates a new blank 800K double-sided floppy image in memory and inserts it
// into the preferred drive if provided (0 or 1) or the first free drive.
// Returns 0 on success, -1 on failure.
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
    scsi_add_device(config->new_scsi, scsi_id, disks[best].vendor, disks[best].product, img);
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

// Device checkpoint functions for saving state

// Function to save complete machine state checkpoint
// Returns GS_SUCCESS on success, GS_ERROR on failure
int setup_plus_checkpoint(const char *filename, checkpoint_kind_t kind) {
    if (!global_emulator) {
        printf("Error: No emulator instance to checkpoint\n");
        return GS_ERROR;
    }

    // Start timing the checkpoint save operation
    double start_time = host_time_ms();

    // Quick checkpoints must store files as references (paths only), never content.
    // Content embedding was a bug that added ~128 KB of ROM I/O per quick checkpoint.
    bool prev_files_mode = checkpoint_get_files_as_refs();
    if (kind == CHECKPOINT_KIND_QUICK) {
        checkpoint_set_files_as_refs(true);
    }

    checkpoint_t *checkpoint = checkpoint_open_write(filename, kind);
    if (!checkpoint) {
        printf("Error: Failed to open checkpoint file for writing: %s\n", filename);
        return GS_ERROR;
    }

    // Save all device states
    memory_map_checkpoint(global_emulator->mem_map, checkpoint);
    cpu_checkpoint(global_emulator->new_cpu, checkpoint);
    scheduler_checkpoint(global_emulator->scheduler, checkpoint);

    // Save global interrupt state (irq) after scheduler/cpu
    system_write_checkpoint_data(checkpoint, &global_emulator->irq, sizeof(global_emulator->irq));

    rtc_checkpoint(global_emulator->new_rtc, checkpoint);
    scc_checkpoint(global_emulator->new_scc, checkpoint);
    sound_checkpoint(global_emulator->sound2, checkpoint);
    via_checkpoint(global_emulator->new_via, checkpoint);
    mouse_checkpoint(global_emulator->mouse, checkpoint);
    // Checkpoint list of images (path + writable) before devices that reference them
    {
        uint32_t count = (uint32_t)global_emulator->n_images;
        system_write_checkpoint_data(checkpoint, &count, sizeof(count));
        for (uint32_t i = 0; i < count; ++i) {
            image_checkpoint(global_emulator->images[i], checkpoint);
        }
    }
    scsi_checkpoint(global_emulator->new_scsi, checkpoint);
    keyboard_checkpoint(global_emulator->keyboard, checkpoint);
    floppy_checkpoint(global_emulator->floppy, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        printf("Error: Failed to write checkpoint\n");
        checkpoint_close(checkpoint);
        checkpoint_set_files_as_refs(prev_files_mode);
        return GS_ERROR;
    }

    checkpoint_close(checkpoint);

    // Restore previous files-as-refs mode
    checkpoint_set_files_as_refs(prev_files_mode);

    // Calculate and print elapsed time
    double elapsed_ms = host_time_ms() - start_time;
    printf("Checkpoint saved to %s (%.2f ms)\n", filename, elapsed_ms);
    return GS_SUCCESS;
}

// Function to load machine state from checkpoint
config_t *setup_plus_restore(const char *filename) {
    checkpoint_t *checkpoint = checkpoint_open_read(filename);
    if (!checkpoint) {
        printf("Error: Failed to open checkpoint file for reading: %s\n", filename);
        return NULL;
    }

    // Save the current global emulator so we can restore it on error.
    // setup_plus() sets global_emulator internally (needed by subsystem lookups),
    // but if the overall restore fails we must not leave a broken pointer.
    config_t *prev = global_emulator;

    // Create new emulator instance from checkpoint
    config_t *config = setup_plus(checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        printf("Error: Failed to read checkpoint\n");
        checkpoint_close(checkpoint);
        // Restore the previous global_emulator so callers see the old, valid config
        global_emulator = prev;
        if (config)
            setup_teardown(config);
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
    int result = setup_plus_checkpoint(filename, CHECKPOINT_KIND_CONSOLIDATED);
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
    config_t *new_config = setup_plus_restore(filename);
    if (new_config) {
        // Replace global emulator with restored state
        global_emulator = new_config;
        // Free the old emulator state after replacing it
        // This is safe because:
        // 1. Commands are registered globally, not per-config
        // 2. global_emulator now points to new_config
        // 3. No part of the call stack holds direct references to old_config
        if (old_config) {
            setup_teardown(old_config);
        }
        // Force a one-shot screen redraw so the restored framebuffer appears
        extern void frontend_force_redraw(void);
        frontend_force_redraw();

        // Resume execution if checkpoint was saved while running
        // (scheduler's running flag is restored from checkpoint)
        if (scheduler_is_running(new_config->scheduler)) {
            printf("Checkpoint was saved while running - resuming execution\n");
        }
        return 0;
    }
    return -1;
}

// ---------------- AppleTalk command handlers ----------------
// AppleTalk CLI lives in appletalk.c
