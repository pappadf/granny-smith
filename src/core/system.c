// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.c
// Generic emulator lifecycle: creation, destruction, checkpointing, and
// shared device coordination. Machine-specific init/teardown logic lives in
// the machine's own source file (e.g., src/machines/plus.c) and is invoked
// through the hw_profile_t callback interface.

#include "system_config.h" // full config_t definition (includes system.h transitively)

#include "build_id.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "display.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "image.h"
#include "image_vfs.h"
#include "jmfb.h" // restored-record sense seeding on checkpoint load
#include "keyboard.h"
#include "log.h"
#include "machine_config.h"
#include "machine_profile.h"
#include "memory.h"
#include "mouse.h"
#include "nubus.h"
#include "ppc.h" // ppc_debug_if (the PPC main-CPU debug seam)
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
    if (!cfg || index < 0 || index >= cfg->n_images)
        return NULL;
    return cfg->images[index];
}
int config_get_n_images(config_t *cfg) {
    return cfg ? cfg->n_images : 0;
}

// Add an image to the config's tracked image list.  Runtime-checked
// rather than asserted because asserts compile out under release builds
// and silent overflow into the next struct field would be a memory-
// corruption bug.
void config_add_image(config_t *cfg, image_t *image) {
    if (!cfg || !image)
        return;
    if (cfg->n_images >= MAX_IMAGES) {
        LOG(1, "config_add_image: image table full (max %d), dropping image", MAX_IMAGES);
        return;
    }
    cfg->images[cfg->n_images] = image;
    cfg->n_images++;
}

// Remove an image from the config's tracked image list WITHOUT closing it
// (machine.restart handle transfer: the caller takes ownership so the handle
// survives system_destroy's close loop).  Order is not preserved-sensitive;
// the tail is compacted down.
void config_remove_image(config_t *cfg, image_t *image) {
    if (!cfg || !image)
        return;
    for (int i = 0; i < cfg->n_images; ++i) {
        if (cfg->images[i] != image)
            continue;
        for (int j = i + 1; j < cfg->n_images; ++j)
            cfg->images[j - 1] = cfg->images[j];
        cfg->n_images--;
        cfg->images[cfg->n_images] = NULL;
        return;
    }
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

// Deltas already queued at the ADB device but not yet consumed — see
// adb_mouse_pending().  Returns false (zeros) on non-ADB machines.
bool system_mouse_pending_adb(int *dx, int *dy) {
    if (dx)
        *dx = 0;
    if (dy)
        *dy = 0;
    if (!global_emulator || !global_emulator->adb)
        return false;
    adb_mouse_pending(global_emulator->adb, dx, dy);
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
    if (global_emulator && global_emulator->machine && global_emulator->machine->substrate->reset)
        global_emulator->machine->substrate->reset(global_emulator);
}

// The 68k RESET instruction asserts the bus /RESET line, which resets the
// external peripheral chips (SCSI, NuBus cards, …) to their power-on state.
// It does NOT reset the CPU core (registers / caches / MMU) — that is why the
// boot ROM reconfigures the MMU itself (PMOVE) right after executing RESET.
// A Mac warm restart (Finder ▸ Restart) runs exactly this: mask interrupts →
// RESET → set up the MMU → jump to the boot entry.  Without this, the SCSI
// controller and the video card would carry stale OS-session state into the
// reboot (a garbage-video hang + a write_mr phase assertion).
void system_reset_devices(void) {
    config_t *cfg = global_emulator;
    if (!cfg)
        return;
    if (cfg->scsi)
        scsi_reset_pin(cfg->scsi); // NCR 5380 → bus-free, registers cleared
    if (cfg->nubus)
        nubus_reset(cfg->nubus); // each populated card → power-on state
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

// Main-CPU debug interface accessor (PPC proposal §3.9b).  Returns NULL until
// a machine with a main CPU has been built (ctx doubles as the "populated"
// flag — system_create fills the vtable right after substrate init).
const struct cpu_debug_if *system_cpu_debug_if(void) {
    if (!global_emulator || !global_emulator->cpu_dbg.ctx)
        return NULL;
    return &global_emulator->cpu_dbg;
}

// The active machine configuration (what host-input/object methods act on).
config_t *system_config(void) {
    return global_emulator;
}

// Host-input dispatch through the machine substrate (proposal §4.4).  Every
// substrate implements these — Macs route to the shared mac_input_* helpers
// (keyboard / Toolbox cursor), the Lisa to its COPS — so there is one uniform
// path and no caller-side fallback.  Each returns 0 on success, <0 on failure
// (unknown key/mode, uninitialised memory, no machine).
int system_input_key(const char *key, bool down) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->input_key)
        return -1;
    return cfg->machine->substrate->input_key(cfg, key, down);
}
int system_input_mouse_move(int x, int y, const char *mode) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->input_mouse_move)
        return -1;
    return cfg->machine->substrate->input_mouse_move(cfg, x, y, mode);
}
int system_input_mouse_button(bool down, const char *mode) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->input_mouse_button)
        return -1;
    return cfg->machine->substrate->input_mouse_button(cfg, down, mode);
}

// System-level RTC accessor: returns the current RTC object
rtc_t *system_rtc(void) {
    return global_emulator ? global_emulator->rtc : NULL;
}

// System-level framebuffer accessor: thin wrapper over system_display()
// for renderer call sites that want just the raw bits pointer.
uint8_t *system_framebuffer(void) {
    display_t *d = system_display();
    return d ? (uint8_t *)d->bits : NULL;
}

// System-level display accessor.  Per proposal §3.3.2: a machine with
// built-in video (substrate .display — Plus, Lisa, the MCU family's DAFB)
// shows that factory display; glue030-family machines source theirs from
// the NuBus bus controller (the IIci's built-in RBV is itself the first
// BUILTIN card, so "built-in wins" holds uniformly).  Returns NULL when no
// machine is booted or the booted machine has no primary display (e.g. a
// IIcx with no card seated).
display_t *system_display(void) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return NULL;
    if (cfg->machine->substrate->display) {
        display_t *d = cfg->machine->substrate->display(cfg);
        if (d)
            return d;
    }
    if (cfg->nubus)
        return nubus_primary_display(cfg->nubus);
    return NULL;
}

// Check if emulator is initialized and running
bool system_is_initialized(void) {
    return global_emulator != NULL;
}

// Return the id of the current machine, or NULL if none is active
const char *system_machine_model_id(void) {
    if (!global_emulator || !global_emulator->machine)
        return NULL;
    return global_emulator->machine->id;
}

// Ensure the correct machine is active for the given model id.
// Creates a new machine if none exists, or tears down and recreates if the
// current machine's id doesn't match.  Returns 0 on success, -1 on error.
int system_ensure_machine(const char *model_id) {
    if (!model_id)
        return -1;

    const hw_profile_t *needed = machine_find(model_id);
    if (!needed) {
        LOG(1, "system_ensure_machine: unknown model '%s'", model_id);
        return -1;
    }

    // Already have the right machine?
    const char *current = system_machine_model_id();
    if (current && strcmp(current, model_id) == 0)
        return 0;

    // Teardown existing machine if wrong type
    if (global_emulator) {
        LOG(1, "Switching machine from %s to %s", global_emulator->machine->id, model_id);
        system_destroy(global_emulator);
        global_emulator = NULL;
    }

    // Create the new machine
    config_t *cfg = system_create(needed, NULL);
    if (!cfg) {
        LOG(1, "system_ensure_machine: failed to create %s", model_id);
        return -1;
    }

    LOG(1, "Machine created: %s (%s)", needed->name, needed->id);
    return 0;
}

// Floppy insertion through the machine substrate (proposal §4.4): every
// substrate implements fd_present/fd_insert — Macs route to mac_fd_* (their
// IWM/SWIM via cfg->floppy), the Lisa to its parallel FDC — so there is one
// uniform path and no cfg->floppy special-case here.
static bool sys_fd_is_inserted(config_t *cfg, int drive) {
    if (cfg->machine && cfg->machine->substrate->fd_present)
        return cfg->machine->substrate->fd_present(cfg, drive);
    return true; // no controller → treat as occupied
}

static int sys_fd_insert(config_t *cfg, int drive, image_t *disk) {
    if (cfg->machine && cfg->machine->substrate->fd_insert)
        return cfg->machine->substrate->fd_insert(cfg, drive, disk);
    return -1;
}

// Public present-state accessor for the active machine's floppy drive `drive`.
bool system_fd_present(int drive) {
    return global_emulator ? sys_fd_is_inserted(global_emulator, drive) : false;
}

// Trigger a vertical blanking interval event (delegates to machine callback)
void trigger_vbl(struct config *restrict config) {
    if (config && config->machine && config->machine->substrate->trigger_vbl) {
        config->machine->substrate->trigger_vbl(config);
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

    // An explicit drive is a request, not a hint: fail rather than silently
    // load the disk into the other drive (a caller feeding a guest that is
    // waiting on drive 1 must never have its disk land in drive 0).
    if (preferred != -1) {
        if (preferred == 0 ? !d0_free : !d1_free) {
            printf("fd insert: floppy drive %d is already occupied.\n", preferred);
            image_close(disk);
            free(persistent_path);
            return -1;
        }
    }

    // No drive requested: fall back to the first free one.
    int target = preferred;
    if (target == -1) {
        if (d0_free) {
            target = 0;
        } else if (d1_free) {
            target = 1;
        } else {
            printf("fd insert: both floppy drives are already occupied.\n");
            image_close(disk);
            free(persistent_path);
            return -1;
        }
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

// Attach a SCSI hard disk image. Delegates to add_scsi_drive().
// Returns 0 on success, -1 on error.
static int do_attach_hd_on(struct scsi *bus, const char *path, int scsi_id) {
    if (scsi_id < 0 || scsi_id > 7) {
        printf("hd attach: invalid SCSI ID %d (expected 0..7)\n", scsi_id);
        return -1;
    }
    config_t *config = global_emulator;
    if (!config) {
        printf("hd attach: emulator not initialized.\n");
        return -1;
    }
    add_scsi_drive_on(config, bus ? bus : config->scsi, path, scsi_id);
    return 0;
}

static int do_attach_hd(const char *path, int scsi_id) {
    return do_attach_hd_on(NULL, path, scsi_id);
}

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Built-in machine profiles are a static const array in machine.c
    // (machine_find / machine_list walk it) — no runtime registration needed.

    // Ensure logging categories of interest appear in `log list` even before any messages are emitted.
    // shell_init() (called earlier) already invoked log_init(); categories default to level 0 (OFF).
    (void)log_register_category("appletalk");

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

// Host video-input seam: the defaults model "no camera attached" — the
// headless build drives capture from the deterministic machine.videoin
// sources instead; em_camera.c overrides these on WASM.
__attribute__((weak)) bool gs_video_in_connected(void) {
    return false;
}

__attribute__((weak)) int gs_video_in_frame(uint8_t *rgba) {
    (void)rgba;
    return -1;
}

__attribute__((weak)) void gs_video_in_state(bool active) {
    (void)active;
}

// Host audio-input seam: the defaults model "no microphone attached" —
// the headless build drives capture from the deterministic
// machine.audioin sources instead; a WASM override can trail.
__attribute__((weak)) bool gs_audio_in_connected(void) {
    return false;
}

__attribute__((weak)) bool gs_audio_in_frames(int16_t *lr, uint32_t frames, uint32_t rate) {
    (void)lr;
    (void)frames;
    (void)rate;
    return false;
}

__attribute__((weak)) void gs_audio_in_state(bool active) {
    (void)active;
}

__attribute__((weak)) void gs_audio_in_injected(const char *path) {
    (void)path;
}

__attribute__((weak)) bool gs_audio_in_debug(char *buf, size_t buflen) {
    (void)buf;
    (void)buflen;
    return false;
}

// Create an emulator instance for the given machine profile.
// Allocates config_t, wires the machine descriptor, and calls profile->substrate->init().
config_t *system_create(const hw_profile_t *profile, checkpoint_t *checkpoint) {
    assert(profile != NULL);
    assert(profile->substrate != NULL && profile->substrate->init != NULL);

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg)
        return NULL;
    memset(cfg, 0, sizeof(config_t));

    cfg->machine = profile;
    // Main-CPU architecture tag (PPC proposal §3.9a): derived from the
    // profile's cpu_model; the substrate init below builds the matching core.
    cfg->cpu_arch = cpu_arch_for_model(profile->cpu_model);
    global_emulator = cfg;

    // Label the machine container node with the active model name so the
    // SYSTEM tab shows "Macintosh IIcx" rather than the bare "machine"
    // segment (proposal-system-object-model.md §7.1). Covers cold boot and
    // checkpoint restore — both land here.
    machine_set_active_label(profile->name);

    // Compute RAM size: use pending override if set, otherwise machine default
    if (g_pending_ram_kb > 0) {
        cfg->ram_size = g_pending_ram_kb * 1024;
        if (cfg->ram_size > profile->ram_max)
            cfg->ram_size = profile->ram_max;
        g_pending_ram_kb = 0; // consume the override
    } else {
        cfg->ram_size = profile->ram_default;
    }

    // Delegate all machine-specific initialisation to the profile
    profile->substrate->init(cfg, checkpoint);

    // Bind the main-CPU debug seam to whichever core the substrate built.
    switch (cfg->cpu_arch) {
    case CPU_ARCH_M68K:
        if (cfg->cpu)
            cfg->cpu_dbg = cpu_debug_if(cfg->cpu);
        break;
    case CPU_ARCH_PPC:
        if (cfg->ppc)
            cfg->cpu_dbg = ppc_debug_if(cfg->ppc);
        break;
    }

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

    // Tear down NuBus before peripherals so cards (which hold device
    // pointers via cfg->via2 etc.) free cleanly first.  No-op when nubus
    // is NULL (Plus today; future 68000-family machines).
    if (config->nubus) {
        nubus_delete(config->nubus);
        config->nubus = NULL;
    }

    // Delegate machine-specific teardown to the profile
    if (config->machine && config->machine->substrate->teardown) {
        config->machine->substrate->teardown(config);
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

// Add a SCSI hard disk to the configuration.
void add_scsi_drive(struct config *restrict config, const char *filename, int scsi_id) {
    add_scsi_drive_on(config, config ? config->scsi : NULL, filename, scsi_id);
}

// ...on a NAMED bus.  Every Macintosh has exactly one SCSI bus a guest can
// see, so the call above — and every consumer of it — means `config->scsi`.
// The Apple Network Servers are the first machines with more than one:
// two fast/wide 53C825A channels carrying the backplane's bays between
// them, reachable as `machine.scsi` and `machine.scsi2`.  Passing the bus
// explicitly is what lets `machine.scsi2.attach_hd` mean what it says.
void add_scsi_drive_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
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
    if (!best) {
        LOG(1, "add_scsi_drive: drive catalog is empty; cannot attach %s", filename);
        image_close(img);
        free(persistent_path);
        return;
    }

    LOG(1, "Attaching SCSI drive: %s as %s %s (size: %zu bytes, SCSI ID: %d)", filename, best->vendor, best->product,
        sz, scsi_id);

    add_image(config, img);
    scsi_add_device(bus, scsi_id, best->vendor, best->product, best->revision, img, scsi_dev_hd, 512, false);
    // Block the VFS auto-mount cache from serving reads on the same file
    // while the emulator holds writable handles against it (§2.9).
    image_vfs_notify_attached(filename);
    free(persistent_path);
}

// Add a SCSI CD-ROM to the configuration (AppleCD SC Plus / Sony CDU-8002)
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    add_scsi_cdrom_on(config, config ? config->scsi : NULL, filename, scsi_id);
}

// ...on a NAMED bus; see add_scsi_drive_on.
void add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
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

    // Present the disc at the block size it was MASTERED for, which its own
    // Driver Descriptor Map records in sbBlkSize (block 0, 'ER' signature,
    // bytes 2-3).  A pressed Apple system CD says 2048; a disk image laid out
    // like a hard disk — including every image in tests/data/systems — says
    // 512, and there are real 512-byte-block CDs too.  Serving a 512-mastered
    // image at 2048 multiplies every LBA by four: the Start Manager reads the
    // wrong sectors, finds no driver partition and the machine does not boot.
    //
    // This is what real hardware does as well.  An AppleCD drive powers up in
    // 2048-byte mode and Apple's driver issues MODE SELECT to switch it to
    // 512 for HFS discs (scsi_cdrom_mode_select already models that path);
    // adopting sbBlkSize up front gets the boot blocks readable before any
    // driver is loaded, which is the ordering the ROM needs.
    uint16_t cd_block_size = 2048;
    uint8_t ddm[512];
    if (disk_read_data(img, 0, ddm, sizeof(ddm)) == sizeof(ddm) && ddm[0] == 'E' && ddm[1] == 'R') {
        uint16_t sb = (uint16_t)((ddm[2] << 8) | ddm[3]);
        if (sb == 512 || sb == 2048)
            cd_block_size = sb;
        else
            printf("CD-ROM %s: Driver Descriptor Map declares an unsupported block size %u; using 2048\n", filename,
                   sb);
    }

    printf("Attaching SCSI CD-ROM: %s as SONY CD-ROM CDU-8002 (size: %zu bytes, %u-byte blocks, SCSI ID: %d)\n",
           filename, disk_size(img), cd_block_size, scsi_id);

    add_image(config, img);
    scsi_add_device(bus, scsi_id, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, cd_block_size, true);
    image_vfs_notify_attached(filename);
    free(persistent_path);
}

// === machine.restart media transfer (proposal-boot-vs-reset §3.3) ==========
//
// The standard substrate implementation over cfg->floppy + cfg->scsi, bound
// into every Mac substrate's vtable (the Lisa implements its own: parallel
// FDC + ProFile).  Detach removes the open handles from cfg->images — the
// list system_destroy would otherwise close — so they survive the teardown;
// attach hands each handle back through the same device paths a fresh mount
// uses, minus the open-by-path step.

// Capture every mounted medium's handle + attachment coordinates into `out`
// and disown them from the tracked-image list.  Returns the count.
int system_media_detach_std(config_t *cfg, media_slot_t *out, int max) {
    int n = 0;
    for (int d = 0; d < 2 && n < max; ++d) {
        image_t *img = cfg->floppy ? floppy_drive_image(cfg->floppy, (unsigned)d) : NULL;
        if (!img)
            continue;
        out[n] = (media_slot_t){.bus = MEDIA_BUS_FLOPPY, .unit = d, .img = img};
        config_remove_image(cfg, img);
        n++;
    }
    for (unsigned id = 0; id < 8 && n < max; ++id) {
        image_t *img = cfg->scsi ? scsi_device_image(cfg->scsi, id) : NULL;
        if (!img)
            continue;
        media_slot_t *s = &out[n];
        *s = (media_slot_t){.bus = MEDIA_BUS_SCSI, .unit = (int)id, .img = img};
        s->scsi_type = scsi_device_type(cfg->scsi, id);
        s->block_size = scsi_device_block_size(cfg->scsi, id);
        s->read_only = scsi_device_read_only(cfg->scsi, id);
        snprintf(s->vendor, sizeof(s->vendor), "%s", scsi_device_vendor(cfg->scsi, id));
        snprintf(s->product, sizeof(s->product), "%s", scsi_device_product(cfg->scsi, id));
        snprintf(s->revision, sizeof(s->revision), "%s", scsi_device_revision(cfg->scsi, id));
        config_remove_image(cfg, img);
        n++;
    }
    return n;
}

// Re-attach one transferred medium to the freshly built machine.  Returns 0
// on success (the machine owns the handle again), <0 when the medium cannot
// be attached (the caller must close the handle).
int system_media_attach_std(config_t *cfg, const media_slot_t *slot) {
    switch (slot->bus) {
    case MEDIA_BUS_FLOPPY:
        if (sys_fd_insert(cfg, slot->unit, slot->img) != 0)
            return -1;
        add_image(cfg, slot->img);
        return 0;
    case MEDIA_BUS_SCSI:
        if (!cfg->scsi)
            return -1;
        add_image(cfg, slot->img);
        scsi_add_device(cfg->scsi, slot->unit, slot->vendor, slot->product, slot->revision, slot->img,
                        (enum scsi_device_type)slot->scsi_type, slot->block_size, slot->read_only);
        image_vfs_notify_attached(image_get_filename(slot->img));
        return 0;
    default:
        return -1;
    }
}

// Save current machine state to a checkpoint file.
// Returns GS_SUCCESS on success, GS_ERROR on failure.
int system_checkpoint(const char *filename, checkpoint_kind_t kind) {
    if (!global_emulator) {
        printf("Error: No emulator instance to checkpoint\n");
        return GS_ERROR;
    }
    if (!global_emulator->machine || !global_emulator->machine->substrate->checkpoint_save) {
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
    const char *model_id = global_emulator->machine->id;
    uint32_t ram_size_kb = global_emulator->ram_size / 1024;
    checkpoint_t *checkpoint = checkpoint_open_write(filename, kind, model_id, ram_size_kb);
    if (!checkpoint) {
        printf("Error: Failed to open checkpoint file for writing: %s\n", filename);
        return GS_ERROR;
    }

    // Built-from record first (fixed-size POD; the stream is build-ID-gated
    // so the layout may change freely between builds). Restore reads it
    // symmetrically in system_restore before machine construction.
    system_write_checkpoint_data(checkpoint, machine_config_record(), sizeof(machine_config_record_t));

    // Delegate all state serialisation to the machine profile
    global_emulator->machine->substrate->checkpoint_save(global_emulator, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        printf("Error: Failed to write checkpoint\n");
        checkpoint_close(checkpoint);
        checkpoint_set_files_as_refs(prev_files_mode);
        return GS_ERROR;
    }

    checkpoint_close(checkpoint);
    checkpoint_set_files_as_refs(prev_files_mode);

    double elapsed_ms = host_time_ms() - start_time;
    // Ambient by default — the browser's background auto-saves land here
    // every ~15 s and used to spam the terminal. `debug.log checkpoint 1`
    // restores the line; the status bar gets its own push (em_main.c).
    LOG_WITH(log_register_category("checkpoint"), 1, "Checkpoint saved to %s (%.2f ms)", filename, elapsed_ms);
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

    // Read the built-from record (mirrors the write in system_checkpoint).
    // It is installed only after the restore succeeds, so a failed restore
    // leaves the previous machine's record intact.
    machine_config_record_t restored_record;
    memset(&restored_record, 0, sizeof(restored_record));
    system_read_checkpoint_data(checkpoint, &restored_record, sizeof(restored_record));

    // Determine machine profile from the checkpoint header, falling back to
    // the current machine or Plus for backward compatibility.
    const hw_profile_t *profile = NULL;
    const char *saved_model_id = checkpoint_get_model_id(checkpoint);
    if (saved_model_id && saved_model_id[0])
        profile = machine_find(saved_model_id);
    if (!profile)
        profile = (prev && prev->machine) ? prev->machine : machine_find("plus");

    // Restore the RAM size from the checkpoint so system_create uses the
    // correct size instead of the machine default.
    uint32_t saved_ram_kb = checkpoint_get_ram_size_kb(checkpoint);
    if (saved_ram_kb > 0)
        system_set_pending_ram_kb(saved_ram_kb);

    // Seed the construction channels from the restored record so socket
    // resolution recreates the SAVED card configuration — the staged table
    // was consumed by the previous boot, and a checkpoint written with a
    // non-default card must not restore against the slot default (the
    // strictly-ordered stream would misalign).
    if (restored_record.valid) {
        if (restored_record.video_card[0])
            nubus_staged_card_set(NUBUS_STAGED_WILDCARD, restored_record.video_card);
        if (restored_record.video_mode[0])
            nubus_staged_mode_set(NUBUS_STAGED_WILDCARD, restored_record.video_mode);
        if (restored_record.custom_mode[0])
            nubus_staged_custom_mode_set(NUBUS_STAGED_WILDCARD, restored_record.custom_mode);
        if (restored_record.video_sense >= 0)
            jmfb_pending_sense_set((uint8_t)restored_record.video_sense);
        // The DAFB's half of this is NOT staged here: dafb.h is a machine
        // header (src/machines/mcu/) and core may not include it — see the
        // core-layering test. The Quadras' built-in video carries its sense
        // through the checkpoint as device state instead; see dafb_checkpoint().
        if (restored_record.vrom[0])
            vrom_set_path(restored_record.vrom);
    }

    // Fresh vROM-pick list for the restore construction (the card loaders
    // re-report their picks during system_create).
    machine_config_reset_vroms();

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

    // Install the restored built-from record so machine.config answers for
    // the restored machine and machine.restart can replay it. The vROM
    // pick list reflects THIS construction (the loaders re-reported during
    // system_create), so keep the fresh entries over the serialized ones.
    machine_config_record_t *rec = machine_config_record_mut();
    machine_config_vrom_t fresh_vroms[MC_MAX_VROMS];
    memcpy(fresh_vroms, rec->vroms, sizeof(fresh_vroms));
    int32_t fresh_n = rec->n_vroms;
    *rec = restored_record;
    memcpy(rec->vroms, fresh_vroms, sizeof(rec->vroms));
    rec->n_vroms = fresh_n;

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

// ===== Typed object-model entry points =====================================
// Thin wrappers the typed methods (floppy.drives[N].insert,
// scsi.attach_hd, storage.hd_create) call directly — no line
// re-tokenisation, no legacy command framework.

// Insert a floppy image into a drive (or the first free drive when
// drive < 0). Returns 0 on success, negative on error.
int system_fd_insert(const char *path, int drive, bool writable) {
    return do_insert_fd(path, drive, writable ? 1 : 0);
}

// Attach a SCSI hard-disk image at `scsi_id`. Returns 0 / negative.
int system_hd_attach(const char *path, int scsi_id) {
    return do_attach_hd(path, scsi_id);
}

// ...on a NAMED bus (NULL = the machine's primary one).
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    return do_attach_hd_on(bus, path, scsi_id);
}

// Create a blank SCSI hard-disk image sized per `size_str` (a drive
// model, human size, or byte count). Returns 0 / negative.
int system_hd_create(const char *path, const char *size_str) {
    return do_create_hd(path, size_str);
}
