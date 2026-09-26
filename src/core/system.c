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
#include "cpu.h"
#include "display.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "host_input.h"
#include "image.h"
#include "jmfb.h" // restored-record sense seeding on checkpoint load
#include "keyboard.h"
#include "log.h"
#include "machine_config.h"
#include "machine_profile.h"
#include "memory.h"
#include "mouse.h"
#include "nubus.h"
#include "pci.h" // staged-pick re-seeding on checkpoint restore
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
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
// the manifest).  For volatile bases under /tmp/ — typically test
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
// LEVEL 2 -- a machine reset: the reset button, Finder > Restart, the Cuda's
// CMD_RESET, a double bus fault.  Bus reset plus the CPU back to its vector,
// which is the entire difference from level 1.
//
// The two callers of the old
// system_hardware_reset() had incompatible expectations: cpu_hardware_reset()
// called it and then reset the CPU itself, while cuda_reset_event() called it
// ALONE.  That was survivable on PDM and TNT only because their substrate
// handlers happened to call ppc_reset() from inside the bus half.  On the AV
// families nothing reset the 68040 at all, so a guest Cuda CMD_RESET -- which
// is how System 7.5 restarts an AV machine -- re-armed the ROM overlay and
// left the CPU executing from wherever it was: RAM yanked out from under
// $00000000 with the machine still running.
//
// One entry point now does both halves, in the order the hardware imposes:
// the overlay must be back before the vectors at $0/$4 are read.
void system_machine_reset(void) {
    config_t *cfg = global_emulator;
    if (!cfg)
        return;

    system_reset_devices(); // level 1: the board's /RESET net

    if (cfg->cpu) {
        if (cfg->machine && cfg->machine->cpu_model == CPU_MODEL_68040)
            cpu_reset_to_vector_68040(cfg->cpu);
        else
            cpu_reset_to_vector_68030(cfg->cpu);
    } else if (cfg->ppc) {
        ppc_reset(cfg->ppc);
    }
}

// Retained under its old name for the callers that mean "level 2".
void system_hardware_reset(void) {
    system_machine_reset();
}

// The 68k RESET instruction asserts the bus /RESET line, which resets the
// external peripheral chips (SCSI, NuBus cards, …) to their power-on state.
// It does NOT reset the CPU core (registers / caches / MMU) — that is why the
// boot ROM reconfigures the MMU itself (PMOVE) right after executing RESET.
// A Mac warm restart (Finder ▸ Restart) runs exactly this: mask interrupts →
// RESET → set up the MMU → jump to the boot entry.  Without this, the SCSI
// controller and the video card would carry stale OS-session state into the
// reboot (a garbage-video hang + a write_mr phase assertion).
// The devices every Macintosh board wires to /RESET, whatever its chipset.
// A family's bus_reset calls this and then adds its own.
//
// This used to BE system_reset_devices() -- a core-owned list of two devices
// that no board could extend, which is why it never grew a PCI arm and why
// adding one there would have been dead code.
void system_reset_common_devices(config_t *cfg) {
    if (!cfg)
        return;
    // NOTE: no second SCSI bus here.  The Network Servers have one, but there
    // is no `cfg->scsi2` field -- config_t carries one `scsi`, and the ANS's
    // second bus lives in the TNT state, so its family bus_reset is where it
    // belongs.
    if (cfg->scsi)
        // A reset condition on the wire: the bus goes free and every target
        // returns to its power-on state (scsi_bus_reset, called from
        // scsi_reset); on a 5380 machine the chip's registers clear too.
        scsi_reset_pin(cfg->scsi);
    // Both VIAs: on the net per the Guide's destination list, and the reason
    // the ROM overlay comes back (VIA1's Overlay output goes high when the
    // chip resets).  via2 is NULL on the single-VIA machines.
    if (cfg->via1)
        via_reset(cfg->via1);
    if (cfg->via2)
        via_reset(cfg->via2);
    if (cfg->scc)
        scc_reset(cfg->scc); // "MC68000, VIA, SWIM, SCC, SCSI, BBU"
    if (cfg->floppy)
        floppy_reset(cfg->floppy); // the SWIM of that list; media survive
    if (cfg->nubus)
        nubus_reset(cfg->nubus); // each populated card → power-on state
    if (cfg->pci)
        pci_reset(cfg->pci); // PCI RST#, on the same net
}

// Level 1 -- the 68k RESET opcode asserts the peripheral reset line.
//
// It now delegates to the board's own /RESET destination list instead of
// resetting a fixed pair of devices.  Two consequences, both intended and
// both per the sources in machine_profile.h: a guest RESET re-arms the ROM
// overlay (it did not before, and the boot ROM executes RESET while the
// overlay is already on), and the PPC families' chipsets are reached for the
// first time from this path.
void system_reset_devices(void) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->bus_reset) {
        // No bus_reset bound yet (Plus, Lisa -- a gap, not hardware).  Fall
        // back to the common set so those two keep the behaviour they had.
        system_reset_common_devices(cfg);
        return;
    }
    cfg->machine->substrate->bus_reset(cfg);
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

// Main-CPU debug interface accessor.  Returns NULL until
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

// Per-kind sums of the tracked images' I/O counters -- the images the
// machine's drives were given; host-side browsing opens its own handles and
// never counts.  An ejected image does no I/O, so it never lights.
void system_drive_io_counts(uint64_t reads[DRIVE_KIND_COUNT], uint64_t writes[DRIVE_KIND_COUNT]) {
    for (int k = 0; k < DRIVE_KIND_COUNT; k++)
        reads[k] = writes[k] = 0;
    config_t *cfg = global_emulator;
    for (int i = 0; cfg && i < cfg->n_images; i++) {
        const image_t *img = cfg->images[i];
        if (!img)
            continue;
        int k = image_is_floppy(img->type) ? DRIVE_KIND_FD : img->type == image_cdrom ? DRIVE_KIND_CD : DRIVE_KIND_HD;
        reads[k] += img->reads;
        writes[k] += img->writes;
    }
}

// Host-input dispatch through the machine substrate.  Every
// substrate implements these — Macs route to the shared mac_input_* helpers
// (keyboard / Toolbox cursor), the Lisa to its COPS — so there is one uniform
// path and no caller-side fallback.  Each returns 0 on success, <0 on failure
// (unknown key/mode, uninitialised memory, no machine).
int system_input_key(int adb_code, bool down) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->input_key)
        return -1;
    if (adb_code < 0 || adb_code > 0x7F)
        return -1;
    return cfg->machine->substrate->input_key(cfg, adb_code, down);
}
int system_input_key_raw(uint8_t byte) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine || !cfg->machine->substrate->input_key_raw)
        return -1;
    return cfg->machine->substrate->input_key_raw(cfg, byte);
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

display_t *system_display_synced(void) {
    display_t *d = system_display();
    display_sync_pixels(d);
    return d;
}

// System-level display accessor.  A machine with
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
        system_destroy(global_emulator); // clears global_emulator itself
    }

    // Create the new machine
    config_t *cfg = system_create(needed, NULL, NULL);
    if (!cfg) {
        LOG(1, "system_ensure_machine: failed to create %s", model_id);
        return -1;
    }

    LOG(1, "Machine created: %s (%s)", needed->name, needed->id);
    return 0;
}

// Floppy insertion through the machine substrate: every
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
// The machine's floppy drives: its profile's floppy_slots, never more than
// the controller's two.  Drive selection is bounded by this, not by
// FLOPPY_NUM_DRIVES -- a one-drive Mac has no drive 1 to pick.
static int sys_fd_count(config_t *cfg) {
    int n = profile_floppy_count(cfg->machine);
    return n > FLOPPY_NUM_DRIVES ? FLOPPY_NUM_DRIVES : n;
}

// The drive to put a disk in: `preferred` when it exists and is free, else
// with preferred == -1 the first free one.  -1 with the reason printed when
// there is none.  An explicit drive is a request, not a hint: fail rather than
// silently load the disk into another drive (a caller feeding a guest that
// is waiting on drive 1 must never have its disk land in drive 0).
static int sys_fd_pick(config_t *cfg, int preferred, const char *who) {
    int n = sys_fd_count(cfg);
    if (preferred < -1 || preferred >= n) {
        printf("%s: no such floppy drive %d (this machine has %d).\n", who, preferred, n);
        return -1;
    }
    if (preferred != -1) {
        if (sys_fd_is_inserted(cfg, preferred)) {
            printf("%s: floppy drive %d is already occupied.\n", who, preferred);
            return -1;
        }
        return preferred;
    }
    for (int d = 0; d < n; d++)
        if (!sys_fd_is_inserted(cfg, d))
            return d;
    if (n == 0)
        printf("%s: this machine has no floppy drive.\n", who);
    else
        printf("%s: no free floppy drive.\n", who);
    return -1;
}

static int do_insert_fd(const char *path, int preferred, int writable_flag) {
    bool writable = (writable_flag != 0); // default to writable unless explicitly 0

    config_t *config = global_emulator;
    if (!config) {
        printf("fd insert: emulator config not initialized.\n");
        return -1;
    }
    int target = sys_fd_pick(config, preferred, "fd insert");
    if (target < 0)
        return -1;

    image_t *disk = writable ? image_create(path, pick_delta_dir(path)) : image_open_readonly(path);
    if (!disk) {
        printf("fd insert: failed to open disk image: %s\n", path);
        return -1;
    }

    // Register the image only once the drive has taken it.  This ignored the
    // insert's result and printed "inserted" whatever the drive said, so a
    // drive that refused left an orphan on the image list and a success claim.
    if (sys_fd_insert(config, target, disk) != 0) {
        printf("fd insert: floppy drive %d refused %s.\n", target, path);
        image_close(disk);
        return -1;
    }
    add_image(config, disk);
    printf("fd insert: inserted %s into floppy drive %d.\n", path, target);
    return 0;
}

// Probe a file to check if it's a valid floppy image (without inserting).
// Returns 0 if valid floppy, 1 if not.
int system_probe_floppy(const char *path) {
    image_t *disk = image_open_readonly(path);
    if (!disk) {
        printf("%s: NOT a supported format\n", path);
        return 1;
    }

    if (!image_is_floppy(disk->type)) {
        printf("%s: Valid disk image but not a floppy (size: %zu bytes)\n", path, disk->raw_size);
        image_close(disk);
        return 1;
    }

    const char *type_str = "unknown";
    if (disk->type == image_fd_ss)
        type_str = "single-sided 400KB";
    else if (disk->type == image_fd_ds)
        type_str = "double-sided 800KB";
    else if (disk->type == image_fd_dd_mfm)
        type_str = "double-density 720KB MFM";
    else if (disk->type == image_fd_hd)
        type_str = "high-density 1440KB";

    printf("%s: Valid floppy image (%s)\n", path, type_str);
    image_close(disk);
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

    // The preferred drive when it exists and is free, else the first free one.
    int target = (preferred >= 0 && preferred < sys_fd_count(config) && !sys_fd_is_inserted(config, preferred))
                     ? preferred
                     : sys_fd_pick(config, -1, "fd create");
    if (target < 0)
        return -1;

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

    if (sys_fd_insert(config, target, disk) != 0) {
        printf("fd create: floppy drive %d refused %s.\n", target, path);
        image_close(disk);
        return -1;
    }
    add_image(config, disk);
    printf("fd create: created %s (%s) and inserted into drive %d.\n", path, high_density ? "1440K" : "800K", target);
    return 0;
}

// Size limits for hd create
#define HD_CREATE_MAX_SIZE (2ULL * 1024 * 1024 * 1024) // 2 GiB

// Floppy sizes that should be rejected.  Named _BYTES because machine_profile.h
// (included above) declares floppy_kind_t with FLOPPY_400K / FLOPPY_800K as
// ENUM CONSTANTS: bare macros of the same name silently shadowed them for the
// rest of this file, so anything here that later meant the kind would have got
// a byte count instead.
#define FLOPPY_400K_BYTES  409600
#define FLOPPY_800K_BYTES  819200
#define FLOPPY_1440K_BYTES 1474560

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
    if (size == FLOPPY_400K_BYTES || size == FLOPPY_800K_BYTES || size == FLOPPY_1440K_BYTES) {
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
    // Report what actually happened.  This returned 0 unconditionally, so
    // `attach_hd` on an unopenable file printed "Failed to open image" and then
    // answered true -- and once insert() started reporting its attach result,
    // a test could assert on a lie.
    return add_scsi_drive_on(config, bus ? bus : config->scsi, path, scsi_id) ? 0 : -1;
}

static int do_attach_hd(const char *path, int scsi_id) {
    return do_attach_hd_on(NULL, path, scsi_id);
}

// Initialize the setup system and register commands
void setup_init() {
    printf("Granny Smith build %s\n", get_build_id());

    // Built-in machine profiles are a static const array in machine.c
    // (machine_find / machine_list walk it) — no runtime registration needed.

    // Create every category the manifest declares, so `debug.log` with no
    // arguments lists the complete set rather than only what has already been
    // hit or configured.  This replaces a one-off registration of
    // "appletalk" that existed for exactly this reason -- and whose presence
    // was the tell that a manifest was missing.
    log_register_manifest();

    image_init(NULL);
}

// The default AppleShare volume.  The platform registers its path once
// (the browser: /opfs/shared; headless: --shared-dir or $GS_SHARED_DIR); core
// publishes it after every machine build, because a machine's teardown drops
// the volume table.  Both platforms used to carry the same provisioning in a
// system_post_create override, with different mkdir modes and log styles.
#define GS_DEFAULT_SHARE_NAME "Shared"
static char g_default_share[1024];

void system_set_default_share(const char *path) {
    snprintf(g_default_share, sizeof(g_default_share), "%s", path ? path : "");
}

// Publish the default share.  Idempotent; a failure is a logged warning,
// never a boot error — a user who removed the directory still gets a
// running machine.
static void provision_default_share(void) {
    if (!g_default_share[0] || atalk_afp_volume_find(GS_DEFAULT_SHARE_NAME) >= 0)
        return;
    if (mkdir(g_default_share, 0755) != 0 && errno != EEXIST) {
        LOG(0, "warning: default share: cannot create %s: %s", g_default_share, strerror(errno));
        return;
    }
    char err[192];
    if (atalk_afp_volume_add(GS_DEFAULT_SHARE_NAME, g_default_share, err, sizeof(err)) < 0)
        LOG(0, "warning: default share: %s", err);
}

// Background-checkpoint auto state. WASM-only at the moment — the
// headless build has no auto-checkpoint loop, so the weak defaults
// just stub out; em_main.c overrides them to read/write the live
// `checkpoint_auto_enabled` flag.
__attribute__((weak)) bool gs_checkpoint_auto_get(void) {
    return false;
}

__attribute__((weak)) int gs_checkpoint_auto_set(bool enabled) {
    (void)enabled;
    return -2; // no auto-checkpoint loop on this platform
}

// Platform-specific entry points (see system.h): the weak defaults say "not
// supported on this platform" (-2), and a platform that has the thing
// overrides them -- headless quit, wasm download.
__attribute__((weak)) int gs_quit(void) {
    return -2; // the browser owns the page's lifecycle
}
__attribute__((weak)) int gs_download(const char *path) {
    (void)path;
    return -2; // no browser to hand a file to
}

// The quick-checkpoint heartbeat: the web status bar flashes on it.
__attribute__((weak)) void gs_checkpoint_saved(double elapsed_ms) {
    (void)elapsed_ms;
}

// === Checkpoints of the running machine, and finding media ==================
//
// These are file work in the machine's checkpoint directory, the same on
// every platform, so they live here.  They used to exist only in em_main.c,
// with weak stubs that told headless "only supported in the WASM build" --
// untrue of everything but the browser trigger -- so no headless test could
// reach them.  The platform keeps the triggers (the page going hidden, the
// auto-checkpoint tick) and the heartbeat hook above.

#define QUICK_CHECKPOINT_PATH_MAX        512
#define QUICK_CHECKPOINT_MIN_INTERVAL_MS 750.0

static double g_last_quick_checkpoint_ms = 0.0;

// Build "<machine_dir>/state.checkpoint" into out_path.  Returns GS_SUCCESS
// when the machine dir is set and the path fits.
static int build_state_checkpoint_path(char *out_path, size_t out_len) {
    const char *dir = checkpoint_machine_dir();
    if (!dir)
        return GS_ERROR;
    int written = snprintf(out_path, out_len, "%s/state.checkpoint", dir);
    return (written > 0 && (size_t)written < out_len) ? GS_SUCCESS : GS_ERROR;
}

// The path of the machine's current valid quick checkpoint, in a static
// buffer, or NULL when there is none (or it is from another build).
const char *find_valid_checkpoint_path(void) {
    static char path_buf[QUICK_CHECKPOINT_PATH_MAX];
    if (build_state_checkpoint_path(path_buf, sizeof(path_buf)) != GS_SUCCESS)
        return NULL;
    struct stat st;
    if (stat(path_buf, &st) != 0)
        return NULL;
    // Reject checkpoints from a different build (incompatible state layout)
    if (!checkpoint_validate_build_id(path_buf))
        return NULL;
    return path_buf;
}

int system_quick_checkpoint(const char *reason, bool verbose, bool rate_limit) {
    scheduler_t *sched = system_scheduler();
    if (!sched)
        return GS_ERROR;

    // Skip checkpointing when the emulator is idle — nothing meaningful to save
    if (!scheduler_is_running(sched) && cpu_instr_count() == 0)
        return GS_SUCCESS;

    // No machine identity yet → nothing to save under.
    if (!checkpoint_machine_dir()) {
        if (verbose)
            printf("[checkpoint] no machine directory set, skipping quick checkpoint\n");
        return GS_SUCCESS;
    }

    double now = host_time_ms();
    if (rate_limit && g_last_quick_checkpoint_ms > 0.0) {
        double delta = now - g_last_quick_checkpoint_ms;
        if (delta >= 0.0 && delta < QUICK_CHECKPOINT_MIN_INTERVAL_MS)
            return GS_SUCCESS;
    }

    char final_path[QUICK_CHECKPOINT_PATH_MAX];
    char tmp_path[QUICK_CHECKPOINT_PATH_MAX];
    if (build_state_checkpoint_path(final_path, sizeof(final_path)) != GS_SUCCESS)
        return GS_ERROR;
    int wn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (wn <= 0 || (size_t)wn >= sizeof(tmp_path))
        return GS_ERROR;

    // Record running state before stopping - this will be saved in the checkpoint
    bool was_running = scheduler_is_running(sched);
    if (was_running)
        scheduler_stop(sched);
    // Temporarily restore running flag so checkpoint captures the pre-stop state
    if (was_running)
        scheduler_set_running(sched, true);

    double start = host_time_ms();
    // Drop any stale tmp from a crashed prior run.
    unlink(tmp_path);
    int rc = system_checkpoint(tmp_path, CHECKPOINT_KIND_QUICK);
    if (rc == GS_SUCCESS) {
        if (rename(tmp_path, final_path) != 0) {
            printf("[checkpoint] rename %s -> %s failed: %s\n", tmp_path, final_path, strerror(errno));
            unlink(tmp_path);
            rc = GS_ERROR;
        }
    } else {
        unlink(tmp_path);
    }
    double elapsed_ms = host_time_ms() - start;

    if (rc == GS_SUCCESS) {
        g_last_quick_checkpoint_ms = now;
        gs_checkpoint_saved(elapsed_ms);
        if (verbose)
            printf("Checkpoint saved to %s (%.2f ms)\n", final_path, elapsed_ms);
    } else if (verbose) {
        printf("[checkpoint] quick checkpoint failed (%s)\n", reason ? reason : "background");
    }
    return rc;
}

int gs_background_checkpoint(const char *reason) {
    return system_quick_checkpoint(reason ? reason : "manual", true, false) == GS_SUCCESS ? 0 : -1;
}

// Clear checkpoint files inside the current machine directory: drops
// state.checkpoint, any leftover *.tmp, and (defensive) any legacy
// sequence-numbered *.checkpoint / *.pending / *.complete files.  The
// machine directory itself is left in place.
static int clear_checkpoint_files(void) {
    const char *dir_path = checkpoint_machine_dir();
    if (!dir_path)
        return 0;
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;
    struct dirent *entry;
    int removed = 0;
    char path[QUICK_CHECKPOINT_PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.')
            continue;
        size_t len = strlen(name);
        bool match = false;
        if (strcmp(name, "state.checkpoint") == 0)
            match = true;
        else if (len >= 4 && strcmp(name + len - 4, ".tmp") == 0)
            match = true;
        else if (len >= 11 && strcmp(name + len - 11, ".checkpoint") == 0)
            match = true; // legacy
        else if (strstr(name, ".complete") || strstr(name, ".pending"))
            match = true; // legacy
        if (match) {
            snprintf(path, sizeof(path), "%s/%s", dir_path, name);
            if (unlink(path) == 0)
                removed++;
        }
    }
    closedir(dir);
    return removed;
}

int gs_checkpoint_clear(void) {
    int removed = clear_checkpoint_files();
    printf("Cleared %d checkpoint file(s)\n", removed);
    return 0;
}

int gs_register_machine(const char *machine_id, const char *created) {
    if (!machine_id || !created)
        return -1;
    int rc = checkpoint_machine_set(machine_id, created);
    if (rc != 0)
        printf("register_machine: failed to set %s-%s\n", machine_id, created);
    return rc == 0 ? 0 : -1;
}

// Find a mountable media file in a directory: walks `dir_path`, picks the
// first regular file recognised as a floppy image, optionally copies it to
// `dest`, and prints its path.  Returns 0 on success, non-zero on "no media
// found" / IO error.  The web frontend runs it after an archive extraction
// (FS.readdir from the main thread is broken with WasmFS pthreads, so this
// runs on the worker).
int gs_find_media(const char *dir_path, const char *dest) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        printf("find-media: cannot open '%s': %s\n", dir_path, strerror(errno));
        return 1;
    }

    struct dirent *entry;
    char found_path[1024] = {0};
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        // Try as floppy image
        image_t *img = image_open_readonly(full);
        if (img) {
            bool is_floppy = image_is_floppy(img->type);
            image_close(img);
            if (is_floppy) {
                snprintf(found_path, sizeof(found_path), "%s", full);
                break;
            }
        }
    }
    closedir(dir);

    if (!found_path[0])
        return 1;

    // Optionally copy to dest
    if (dest) {
        FILE *fin = fopen(found_path, "rb");
        if (!fin)
            return 1;
        FILE *fout = fopen(dest, "wb");
        if (!fout) {
            fclose(fin);
            return 1;
        }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
            if (fwrite(buf, 1, n, fout) != n) {
                fclose(fin);
                fclose(fout);
                return 1;
            }
        }
        fclose(fin);
        fclose(fout);
    }

    printf("%s\n", found_path);
    return 0;
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

// Host GPU-transport seam (system.h): no GPU on a native host.
__attribute__((weak)) bool gs_v2gpu_available(void) {
    return false;
}

__attribute__((weak)) bool gs_v2gpu_attach(void *ctrl, uint32_t bytes) {
    (void)ctrl;
    (void)bytes;
    return false;
}

__attribute__((weak)) void gs_v2gpu_detach(void *ctrl) {
    (void)ctrl;
}

__attribute__((weak)) int gs_v2gpu_wait(volatile uint32_t *addr, uint32_t expected, uint32_t timeout_ms) {
    (void)addr;
    (void)expected;
    (void)timeout_ms;
    return -1;
}

__attribute__((weak)) void gs_v2gpu_notify(volatile uint32_t *addr) {
    (void)addr;
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
config_t *system_create(const hw_profile_t *profile, const machine_build_opts_t *opts, checkpoint_t *checkpoint) {

    assert(profile != NULL);
    assert(profile->substrate != NULL && profile->substrate->init != NULL);

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg)
        return NULL;
    memset(cfg, 0, sizeof(config_t));
    cfg->build_opts = opts ? *opts : machine_build_opts_default();

    cfg->machine = profile;
    // Main-CPU architecture tag: derived from the
    // profile's cpu_model; the substrate init below builds the matching core.
    cfg->cpu_arch = cpu_arch_for_model(profile->cpu_model);
    global_emulator = cfg;

    // Label the machine container node with the active model name so the
    // SYSTEM tab shows "Macintosh IIcx" rather than the bare "machine"
    // segment. Covers cold boot and checkpoint restore — both land here.
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

    // Delegate all machine-specific initialisation to the profile.  A
    // non-zero return means the machine could not be built (the only cause
    // today is an allocation failure); tear down whatever it managed and
    // report the failure rather than handing back a half-built config.  Every
    // teardown tolerates a partially-constructed machine -- each guards its
    // machine_context -- which is what makes this safe to call here.
    if (profile->substrate->init(cfg, checkpoint) != 0) {
        LOG(0, "Error: failed to construct %s", profile->name);
        if (profile->substrate->teardown)
            profile->substrate->teardown(cfg);
        free(cfg);
        return NULL;
    }

    // The floppy controller learns how many drives this machine cables.
    if (cfg->floppy)
        floppy_set_drive_count(cfg->floppy, sys_fd_count(cfg));

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

    // A machine with a CD bay has the DRIVE on the bus from power-on, disc or
    // no disc.  SCSI is not hot-plug: the guest's CD driver claims its targets
    // during the boot-time bus scan and polls only those, so a drive that
    // materialises later — when the user picks Insert — is one nothing ever
    // looks at, and the disc never mounts.  Registering it empty here makes a
    // later insert an ordinary medium change (UNIT ATTENTION), which is what
    // the driver notices and the Finder mounts on.
    //
    // Skip a slot that is already occupied: restoring a checkpoint rebuilds
    // the bus from the saved state, and that device outranks a blank bay.
    if (profile->has_cdrom && cfg->scsi && !scsi_device_present(cfg->scsi, (unsigned)profile->cdrom_id))
        scsi_add_device(cfg->scsi, profile->cdrom_id, "SONY", "CD-ROM CDU-8002", "1.8g", NULL, scsi_dev_cdrom, 2048,
                        true);

    // The `machine.adb.keyboard` object, per machine.  After the substrate
    // because it wants the scheduler, before scheduler_start because its
    // event type has to exist when the scheduler re-binds restored events.
    cfg->host_input = host_input_init(cfg, cfg->scheduler);

    // Stand up the object-model root: attaches stub classes for
    // cpu/memory/scheduler/machine/shell/storage so `eval` can read
    // runtime state. The legacy shell remains primary.
    root_install(cfg);

    // The volume table went with the previous machine: publish the share.
    provision_default_share();

    // Cold boot: stamp out a manifest documenting what was set up.  Skipped
    // on checkpoint restore — the manifest is fixed at original creation
    // time and is purely informational.  Failure is non-fatal.
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

    // The keyboard object goes with the root teardown: it is per machine, and
    // any typing still paced out on the scheduler is aimed at a machine that
    // is about to stop existing.
    host_input_delete(config->host_input);
    config->host_input = NULL;

    // Tear down the expansion buses before the peripherals, so cards --
    // which hold pointers to devices the substrate owns -- free cleanly
    // first.  NuBus cards hold cfg->via2 and friends; the Network
    // Servers' two 53C825As borrow cfg->scsi and machine.scsi2, so a card
    // outliving its bus is a use-after-free either way.  Both are no-ops
    // when the machine has no such bus.
    //
    // The order BETWEEN the two is not load-bearing: no profile declares
    // both nubus_slots and pci_slots, so a machine has at most one of
    // these.  Do not read a dependency into it.
    if (config->pci) {
        pci_root_delete(config->pci);
        config->pci = NULL;
    }
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

    // The process-global pointer dies with the config it names.  This used to
    // be every caller's job: five sites remembered and one -- system_restore's
    // failure path, which deliberately restores a DIFFERENT config -- did not,
    // which is what made the pattern look load-bearing rather than fragile.
    // `global_emulator` is read from roughly sixty places (system_scheduler,
    // system_cpu, system_memory, system_is_initialized, machine.c's attribute
    // getters, checkpoint_machine.c, iop_swim.c), so a caller that forgot left
    // all of them pointing at freed memory -- and system_is_initialized()
    // answering true for it.
    //
    // The guard is what keeps system_restore working: it installs the new
    // config first and destroys the old one after, so by the time this runs
    // the global already names someone else and must not be cleared.
    if (global_emulator == config)
        global_emulator = NULL;

    free(config);
}

// Reset Mac hardware to initial state
void mac_reset(config_t *restrict sim) {
    scc_reset(sim->scc);
}

// Open `path` as the medium a bay on `bus` takes and fill in `slot` -- the
// image handle plus, on SCSI, the identity the device presents -- ready for
// a substrate's media_attach.  One open for every attach path: a hard disk
// as the closest catalog drive over a base+delta image, a CD-ROM read-only
// at 2048-byte blocks, a ProFile at its 532-byte block.  slot->unit is left
// 0 for the caller.  Returns false (and says why) when the file cannot be
// opened.
static bool media_open(media_bus_t bus, bool cdrom, const char *path, media_slot_t *slot) {
    *slot = (media_slot_t){.bus = bus};
    if (!path || !*path) {
        printf("Cannot attach: no image path\n");
        return false;
    }
    if (bus == MEDIA_BUS_PROFILE) {
        const image_geometry_t geom = {.block_size = PROFILE_BLOCK_SIZE};
        slot->img = image_create_with_geometry(path, pick_delta_dir(path), geom);
        if (!slot->img)
            printf("Failed to open ProFile image: %s\n", path);
        return slot->img != NULL;
    }
    if (cdrom) {
        // CD-ROM images are always opened read-only
        slot->img = image_open_readonly(path);
        if (!slot->img) {
            printf("Failed to open CD-ROM image: %s\n", path);
            return false;
        }
        slot->img->type = image_cdrom;
        // A CD-ROM drive presents 2048-byte logical blocks — that is the Mode 1
        // sector, not a property of the disc — so serve every disc at 2048 and let
        // the guest ask for anything else.  A host that wants 512-byte addressing
        // issues MODE SELECT with a block descriptor, which scsi_cdrom_mode_select
        // already honours; A/UX does exactly that when it mounts its install CD.
        //
        // Do NOT adopt the sbBlkSize the disc's Driver Descriptor Map records
        // (block 0, 'ER' signature, bytes 2-3).  That field is the unit the
        // PARTITION MAP is addressed in — 512 on an HFS disc, including a raw hard
        // disk image burned to CD — and not the drive's block length.  Conflating
        // the two hands the Apple CD-ROM driver a 512-byte device when it is
        // addressing 2048-byte sectors, so every sector number it computes lands a
        // quarter of the way into the disc: it reads byte 8192 looking for the
        // ISO 9660 descriptor at sector 16, never finds the partition map, and the
        // Finder offers to initialize the disc.  Mapping the map's 512-byte units
        // onto 2048-byte sectors is the driver's job, and it does it in software.
        slot->scsi_type = scsi_dev_cdrom;
        slot->block_size = 2048;
        slot->read_only = true;
        snprintf(slot->vendor, sizeof(slot->vendor), "SONY");
        snprintf(slot->product, sizeof(slot->product), "CD-ROM CDU-8002");
        snprintf(slot->revision, sizeof(slot->revision), "1.8g");
        printf("Attaching SCSI CD-ROM: %s as SONY CD-ROM CDU-8002 (size: %zu bytes, %u-byte blocks)\n", path,
               disk_size(slot->img), slot->block_size);
        return true;
    }
    slot->img = image_create(path, pick_delta_dir(path));
    if (!slot->img) {
        printf("Failed to open image: %s\n", path);
        return false;
    }
    size_t sz = disk_size(slot->img);
    // Find the closest drive model from the catalog
    const struct drive_model *best = drive_catalog_find_closest(sz);
    if (!best) {
        LOG(1, "add_scsi_drive: drive catalog is empty; cannot attach %s", path);
        image_close(slot->img);
        slot->img = NULL;
        return false;
    }
    LOG(1, "Attaching SCSI drive: %s as %s %s (size: %zu bytes)", path, best->vendor, best->product, sz);
    slot->scsi_type = scsi_dev_hd;
    slot->block_size = 512;
    slot->read_only = false;
    snprintf(slot->vendor, sizeof(slot->vendor), "%s", best->vendor);
    snprintf(slot->product, sizeof(slot->product), "%s", best->product);
    snprintf(slot->revision, sizeof(slot->revision), "%s", best->revision);
    return true;
}

// Add a SCSI hard disk to the configuration.
bool add_scsi_drive(struct config *restrict config, const char *filename, int scsi_id) {
    return add_scsi_drive_on(config, config ? config->scsi : NULL, filename, scsi_id);
}

// ...on a NAMED bus.  Every Macintosh has exactly one SCSI bus a guest can
// see, so the call above — and every consumer of it — means `config->scsi`.
// The Apple Network Servers are the first machines with more than one:
// two fast/wide 53C825A channels carrying the backplane's bays between
// them, reachable as `machine.scsi` and `machine.scsi2`.  Passing the bus
// explicitly is what lets `machine.scsi2.attach_hd` mean what it says.
//
// A NULL bus is refused: the Lisa has no SCSI at all, and `hd=` on a Lisa
// used to hand NULL to scsi_add_device and crash the harness.
bool add_scsi_drive_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    if (!bus) {
        printf("Cannot attach %s: this machine has no SCSI bus\n", filename);
        return false;
    }
    media_slot_t slot;
    if (!media_open(MEDIA_BUS_SCSI, false, filename, &slot))
        return false;
    slot.unit = scsi_id;
    return system_media_attach_scsi_bus(config, bus, &slot) == 0;
}

// Add a SCSI CD-ROM to the configuration (AppleCD SC Plus / Sony CDU-8002)
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    return add_scsi_cdrom_on(config, config ? config->scsi : NULL, filename, scsi_id);
}

// ...on a NAMED bus; see add_scsi_drive_on.
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    if (!bus) {
        printf("Cannot attach CD-ROM %s: this machine has no SCSI bus\n", filename);
        return false;
    }
    media_slot_t slot;
    if (!media_open(MEDIA_BUS_SCSI, true, filename, &slot))
        return false;
    slot.unit = scsi_id;
    return system_media_attach_scsi_bus(config, bus, &slot) == 0;
}

// === machine.restart media transfer ========================================
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
    n += system_media_detach_scsi_bus(cfg, cfg->scsi, MEDIA_BUS_SCSI, out + n, max - n);
    return n;
}

// Capture one SCSI bus's mounted media, tagged with the bus they came off.
// Split out because a machine may have more than one visible bus and a SCSI
// id does not identify a device on its own there (the Network Servers' two
// fast/wide channels); a substrate with a second bus calls this again for
// it.  Returns the count appended.
int system_media_detach_scsi_bus(config_t *cfg, struct scsi *bus, media_bus_t which, media_slot_t *out, int max) {
    int n = 0;
    for (unsigned id = 0; id < 8 && n < max; ++id) {
        image_t *img = bus ? scsi_device_image(bus, id) : NULL;
        if (!img)
            continue;
        media_slot_t *s = &out[n];
        *s = (media_slot_t){.bus = which, .unit = (int)id, .img = img};
        s->scsi_type = scsi_device_type(bus, id);
        s->block_size = scsi_device_block_size(bus, id);
        s->read_only = scsi_device_read_only(bus, id);
        snprintf(s->vendor, sizeof(s->vendor), "%s", scsi_device_vendor(bus, id));
        snprintf(s->product, sizeof(s->product), "%s", scsi_device_product(bus, id));
        snprintf(s->revision, sizeof(s->revision), "%s", scsi_device_revision(bus, id));
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
        return system_media_attach_scsi_bus(cfg, cfg->scsi, slot);
    default:
        return -1;
    }
}

// Hand one transferred medium back to a named SCSI bus.  The counterpart of
// system_media_detach_scsi_bus, and the same reason for existing.
int system_media_attach_scsi_bus(config_t *cfg, struct scsi *bus, const media_slot_t *slot) {
    if (!bus)
        return -1;
    add_image(cfg, slot->img);
    scsi_add_device(bus, slot->unit, slot->vendor, slot->product, slot->revision, slot->img,
                    (enum scsi_device_type)slot->scsi_type, slot->block_size, slot->read_only);
    return 0;
}

// === Machine-level attach and eject ===========================
//
// One verb for "put this disk in that bay", whatever bus the bay is on: the
// same substrate dispatch machine.restart hands media back through
// (media_attach), fed by media_open instead of a transferred handle.  Before,
// every caller branched on hd_bus itself and chose between scsi.attach_hd,
// scsi2.attach_hd and hd.attach -- and several chose wrong.

bool system_media_present_scsi_bus(struct scsi *bus, int unit) {
    return bus && unit >= 0 && unit <= 6 && scsi_device_image(bus, (unsigned)unit) != NULL;
}

int system_media_eject_scsi_bus(struct scsi *bus, int unit) {
    if (!bus || unit < 0 || unit > 6)
        return -1;
    int rc = scsi_eject_device(bus, unit);
    return rc == 1 ? 0 : rc == -2 ? -2 : -1;
}

bool system_media_present_std(config_t *cfg, media_bus_t bus, int unit) {
    switch (bus) {
    case MEDIA_BUS_FLOPPY:
        return sys_fd_is_inserted(cfg, unit);
    case MEDIA_BUS_SCSI:
        return system_media_present_scsi_bus(cfg->scsi, unit);
    default:
        return false;
    }
}

int system_media_eject_std(config_t *cfg, media_bus_t bus, int unit) {
    switch (bus) {
    case MEDIA_BUS_FLOPPY:
        return (cfg->floppy && unit >= 0 && floppy_drive_eject(cfg->floppy, (unsigned)unit)) ? 0 : -1;
    case MEDIA_BUS_SCSI:
        return system_media_eject_scsi_bus(cfg->scsi, unit);
    default:
        return -1;
    }
}

int system_media_attach_path(config_t *cfg, const media_bay_t *bay, bool cdrom, const char *path, char *err,
                             size_t errlen) {
    const machine_substrate_t *sub = (cfg && cfg->machine) ? cfg->machine->substrate : NULL;
    if (!sub || !sub->media_attach) {
        snprintf(err, errlen, "no machine is running");
        return -1;
    }
    if (sub->media_present && sub->media_present(cfg, bay->bus, bay->unit)) {
        snprintf(err, errlen, "%s is occupied; eject it first", bay->label ? bay->label : "the bay");
        return -1;
    }
    media_slot_t slot;
    if (!media_open(bay->bus, cdrom, path, &slot)) {
        snprintf(err, errlen, "cannot open '%s'", path ? path : "");
        return -1;
    }
    slot.unit = bay->unit;
    if (sub->media_attach(cfg, &slot) != 0) {
        image_close(slot.img);
        snprintf(err, errlen, "%s cannot take '%s'", bay->label ? bay->label : "the bay", path);
        return -1;
    }
    return 0;
}

int system_media_eject(config_t *cfg, media_bus_t bus, int unit) {
    const machine_substrate_t *sub = (cfg && cfg->machine) ? cfg->machine->substrate : NULL;
    if (!sub || !sub->media_eject)
        return -1;
    return sub->media_eject(cfg, bus, unit);
}

// Save current machine state to a checkpoint file.
// Returns GS_SUCCESS on success, GS_ERROR on failure.
int system_checkpoint(const char *filename, checkpoint_kind_t kind) {
    if (!global_emulator) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: no emulator instance to checkpoint");
        return GS_ERROR;
    }
    if (!global_emulator->machine || !global_emulator->machine->substrate->checkpoint_save) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: machine has no checkpoint_save callback");
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
        LOG_WITH(log_register_category("ckpt"), 0, "Error: failed to open checkpoint file for writing: %s", filename);
        return GS_ERROR;
    }

    // Built-from record first (fixed-size POD; the stream is build-ID-gated
    // so the layout may change freely between builds). Restore reads it
    // symmetrically in system_restore before machine construction.
    system_write_checkpoint_data(checkpoint, machine_config_record(), sizeof(machine_config_record_t));

    // Delegate all state serialisation to the machine profile
    global_emulator->machine->substrate->checkpoint_save(global_emulator, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: failed to write checkpoint");
        checkpoint_close(checkpoint);
        checkpoint_set_files_as_refs(prev_files_mode);
        return GS_ERROR;
    }

    checkpoint_close(checkpoint);
    checkpoint_set_files_as_refs(prev_files_mode);

    double elapsed_ms = host_time_ms() - start_time;
    // Ambient by default — the browser's background auto-saves land here
    // every ~15 s and used to spam the terminal. `debug.log ckpt 1`
    // restores the line; the status bar gets its own push (em_main.c).
    LOG_WITH(log_register_category("ckpt"), 1, "Checkpoint saved to %s (%.2f ms)", filename, elapsed_ms);
    return GS_SUCCESS;
}

// Restore machine state from a checkpoint file.
config_t *system_restore(const char *filename) {
    checkpoint_t *checkpoint = checkpoint_open_read(filename);
    if (!checkpoint) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: failed to open checkpoint file for reading: %s", filename);
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
    machine_build_opts_t build_opts = machine_build_opts_default();
    if (restored_record.valid) {
        if (restored_record.video_card[0])
            nubus_staged_card_set(NUBUS_STAGED_WILDCARD, restored_record.video_card);
        if (restored_record.video_mode[0])
            nubus_staged_mode_set(NUBUS_STAGED_WILDCARD, restored_record.video_mode);
        if (restored_record.custom_mode[0])
            nubus_staged_custom_mode_set(NUBUS_STAGED_WILDCARD, restored_record.custom_mode);
        // The sense goes into the build options, which every video model
        // reads -- the JMFB cards, the DAFB and PDM's Ariel alike.  This used
        // to call jmfb_pending_sense_set() and note that "the DAFB's half is
        // NOT staged here: dafb.h is a machine header and core may not
        // include it", so the Quadras carried their sense through the
        // checkpoint as device state instead.  machine_build_opts_t lives in
        // core, so one channel now serves both and the layering test is
        // satisfied by construction rather than by a second mechanism.
        if (restored_record.video_sense >= 0)
            build_opts.video_sense = restored_record.video_sense;
        // The built-in monitor strap resolves to a sense code exactly as
        // machine.boot resolves it (machine_boot_apply), and wins over
        // video_sense there too.  The record's id was validated at boot.
        if (restored_record.monitor[0] && profile->builtin_video && profile->builtin_video->monitor_sense) {
            uint8_t mon_sense = 0;
            if (profile->builtin_video->monitor_sense(restored_record.monitor, &mon_sense))
                build_opts.video_sense = mon_sense;
        }
        // The record's explicit vrom=/prom= picks replace whatever the
        // running machine registered.
        machine_config_set_explicit_picks(restored_record.vrom, restored_record.prom);
        // The PCI half of the same rule: a checkpoint written with a
        // socketed PCI card (and its options) must re-seat that card, or
        // the slot resolves its default (usually empty) and the
        // strictly-ordered PCI device stream misaligns on the first
        // record the missing card wrote.
        if (restored_record.pci_card[0])
            pci_staged_card_set(PCI_STAGED_WILDCARD, restored_record.pci_card);
        pci_staged_option_set_spec(PCI_STAGED_WILDCARD, restored_record.pci_option);
        // ...and the explicit per-slot picks beyond the wildcard, the
        // multi-card surface machine.restart already replays.
        for (int i = 0; i < restored_record.n_slot_cards; i++) {
            const machine_config_slot_card_t *e = &restored_record.slot_cards[i];
            if (!e->explicit_pick)
                continue;
            if (e->bus_kind == MC_BUS_PCI)
                pci_staged_card_set(e->slot, e->card_id);
            else
                nubus_staged_card_set(e->slot, e->card_id);
        }
    }

    // Fresh vROM-pick list for the restore construction (the card loaders
    // re-report their picks during system_create).
    machine_config_reset_vroms();

    config_t *config = system_create(profile, &build_opts, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        LOG(0, "Error: Failed to read checkpoint");
        checkpoint_close(checkpoint);
        global_emulator = prev;
        if (config)
            system_destroy(config);
        // Put the surviving machine's object tree back.
        //
        // system_create() ran root_install(config) on the way in, which tore
        // down `prev`'s stubs and claimed g_installed_cfg; system_destroy()
        // then ran root_uninstall_if(config), which matched and uninstalled
        // again.  Nothing reinstalled `prev`.  So a truncated or mismatched
        // checkpoint left the still-running machine executing with `shell`,
        // `shell.functions`, `shell.alias`, `storage`, `storage.images`,
        // `machine.nubus` and `machine.pci` all detached and the root methods
        // gone -- the entire tooling surface evaporated, with no diagnostic
        // beyond "Failed to read checkpoint".
        if (prev)
            root_install(prev);
        // ...and its explicit picks: the installed record is still prev's.
        machine_config_set_explicit_picks(machine_config_record()->vrom, machine_config_record()->prom);
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

    LOG_WITH(log_register_category("ckpt"), 1, "Checkpoint restored from %s", filename);
    return config;
}

// Save the current state to a checkpoint file.
//
// Was cmd_save_checkpoint(argc, argv) -- the retired command shape -- reached
// by the typed checkpoint.save() building a fake argv[] and then string-
// matching the mode back out of it.  The typed method calls this directly now
// and the mode arrives as a validated V_ENUM, so the framework rejects a typo
// instead of the body re-checking it.
int system_checkpoint_save(const char *filename, bool files_as_refs) {
    if (!filename || !*filename)
        return -1;
    bool prev_mode = checkpoint_get_files_as_refs();
    checkpoint_set_files_as_refs(files_as_refs);
    int result = system_checkpoint(filename, CHECKPOINT_KIND_CONSOLIDATED);
    checkpoint_set_files_as_refs(prev_mode); // restore previous setting
    return result;
}

// Load a saved checkpoint.  `filename` NULL or empty auto-loads the latest
// valid background checkpoint.
//
// The "probe" subcommand this used to carry is now system_checkpoint_probe():
// it was reached by string-matching argv[1], and since the typed method passed
// the USER'S PATH as argv[1], `checkpoint.load("probe")` ran a probe instead
// of loading a file called probe.  That collision goes with the argv[] layer.
int system_checkpoint_load(const char *filename) {
    char auto_buf[1024];
    if (!filename || !*filename) {
        const char *auto_path = find_valid_checkpoint_path();
        if (!auto_path) {
            LOG(0, "No valid checkpoint found");
            return 1;
        }
        snprintf(auto_buf, sizeof(auto_buf), "%s", auto_path);
        LOG(1, "Auto-loading checkpoint: %s", auto_buf);
        filename = auto_buf;
    }

    config_t *old_config = global_emulator;
    config_t *new_config = system_restore(filename);
    if (!new_config)
        return -1;

    // Replace global emulator with restored state.  Safe because commands are
    // registered globally rather than per-config, global_emulator now names
    // new_config, and no part of the call stack holds old_config.
    global_emulator = new_config;
    if (old_config)
        system_destroy(old_config);

    // Force a one-shot screen redraw so the restored framebuffer appears
    extern void frontend_force_redraw(void);
    frontend_force_redraw();

    if (scheduler_is_running(new_config->scheduler))
        LOG(1, "Checkpoint was saved while running - resuming execution");
    return 0;
}

// True when a valid background checkpoint exists to load.
bool system_checkpoint_probe(void) {
    return find_valid_checkpoint_path() != NULL;
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
