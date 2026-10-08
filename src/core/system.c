// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.c
// Generic emulator lifecycle: creation, destruction, checkpointing, and
// shared device coordination. Machine-specific init/teardown logic lives in
// the machine's own source file (e.g., src/machines/plus.c) and is invoked
// through the hw_profile_t callback interface.

#include "system_config.h" // full config_t definition (includes system.h transitively)

#include "adb.h"
#include "appletalk.h"
#include "build_id.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "display.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "gs_out.h"
#include "host_input.h"
#include "image.h"
#include "image_wrap.h"
#include "keyboard.h"
#include "log.h"
#include "machine_parts.h"
#include "machine_profile.h"
#include "memory.h"
#include "mouse.h"
#include "nubus.h"
#include "pci.h"
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
#include "event/gs_event.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

LOG_USE_CATEGORY_NAME("setup");

// Global emulator pointer (definition)
config_t *global_emulator = NULL;

// Pick the delta directory for a fresh writable mount.  Default is the
// active machine directory (so deltas live alongside state.checkpoint and
// the manifest).  For volatile bases under /tmp/ — typically test
// artifacts uploaded to memfs — fall back to NULL so image_create places
// deltas in its scratch root (also under /tmp), preserving memfs-only I/O
// performance.
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

// Set while system_create builds a machine.  The machine under construction
// is not the active machine -- it may be built while another one runs -- so a
// constructor reaches its devices through the cfg it was given, never through
// the accessors below, which name the ACTIVE machine.  They assert it.
static bool s_constructing;
#define NOT_DURING_CONSTRUCTION()                                                                                      \
    do {                                                                                                               \
        if (s_constructing)                                                                                            \
            construction_misuse(__func__);                                                                             \
    } while (0)

// The assert handler itself reads the active machine (its backtrace), so the
// flag is cleared before it runs.
static void construction_misuse(const char *accessor) {
    (void)accessor; // unused when asserts compile out
    s_constructing = false;
    gs_event_hold(0);
    GS_ASSERTF(false, "%s() names the active machine; a constructor uses its own cfg", accessor);
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
// The CPU to its reset vector: the 68k cores fetch SSP and PC from the ROM,
// the PowerPC goes to its reset state.  The CPU half of every reset level,
// and the one CPU reset a new machine gets at construction.
static void system_cpu_reset(config_t *cfg) {
    if (cfg->cpu) {
        if (cfg->machine && cfg->machine->cpu_model == CPU_MODEL_68040)
            cpu_reset_to_vector_68040(cfg->cpu);
        else
            cpu_reset_to_vector_68030(cfg->cpu);
    } else if (cfg->ppc) {
        ppc_reset(cfg->ppc);
    }
}

void system_machine_reset(void) {
    config_t *cfg = global_emulator;
    if (!cfg)
        return;

    system_reset_devices(); // level 1: the board's /RESET net
    system_cpu_reset(cfg);
}

// LEVEL 3 -- a power cycle (machine.restart).  Switching a machine off and on
// does not replace its chips: the CPU goes to its vector, the chipset to
// power-on and DRAM loses its contents, while the battery-backed store, the
// disks in the drives and the latched switches stay exactly where they were.
// So this is level 2 with the RAM image cleared to the state a freshly
// constructed machine has (memory_map_init calloc's it) -- deterministic,
// where real DRAM would come up indeterminate.  The clear is what makes the
// ROM's warm-start check take the cold path.  It runs BEFORE the reset so
// the vector fetch reads a cold machine.
void system_machine_power_cycle(void) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->mem_map)
        return;
    uint8_t *ram = ram_native_pointer(cfg->mem_map, 0);
    if (ram && cfg->ram_size)
        memset(ram, 0, cfg->ram_size); // DRAM loses its contents
    // The ADB bus is powered by the machine: its devices lose power too.
    adb_power_on(cfg->adb);
    // State a power-up initialises and /RESET does not (machine_profile.h).
    if (cfg->machine && cfg->machine->substrate->power_on)
        cfg->machine->substrate->power_on(cfg);
    // NuBus cards lose their VRAM as main RAM does (a board's built-in video
    // is its substrate's power_on).
    if (cfg->nubus)
        nubus_power_on(cfg->nubus);
    system_machine_reset();
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
    // ...and with VIA1 no longer driving PB2, the RTC's /CE floats high: the
    // clock chip (battery-backed, not on the net itself) abandons any transfer
    // the guest left half-done and lets go of its data line.
    rtc_deselect(cfg->rtc);
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
    NOT_DURING_CONSTRUCTION();
    return global_emulator ? global_emulator->scheduler : NULL;
}

// System-level memory accessor: returns the current memory object
memory_map_t *system_memory(void) {
    NOT_DURING_CONSTRUCTION();
    return global_emulator ? global_emulator->mem_map : NULL;
}

// System-level debug accessor: returns the current debugger object
debug_t *system_debug(void) {
    NOT_DURING_CONSTRUCTION();
    return global_emulator ? global_emulator->debugger : NULL;
}

// System-level CPU accessor: returns the current CPU object
cpu_t *system_cpu(void) {
    NOT_DURING_CONSTRUCTION();
    return global_emulator ? global_emulator->cpu : NULL;
}

// Main-CPU debug interface accessor.  Returns NULL until
// a machine with a main CPU has been built (ctx doubles as the "populated"
// flag — system_create fills the vtable right after substrate init).
const struct cpu_debug_if *system_cpu_debug_if(void) {
    NOT_DURING_CONSTRUCTION();
    if (!global_emulator || !global_emulator->cpu_dbg.ctx)
        return NULL;
    return &global_emulator->cpu_dbg;
}

// The active machine configuration (what host-input/object methods act on).
config_t *system_config(void) {
    NOT_DURING_CONSTRUCTION();
    return global_emulator;
}

config_t *system_running(void) {
    return global_emulator;
}

struct scheduler *system_running_scheduler(void) {
    return global_emulator ? global_emulator->scheduler : NULL;
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
    NOT_DURING_CONSTRUCTION();
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

// System-level display accessor: the display device the monitor is plugged
// into.  A card (or a built-in video that is a slot device -- the SE/30's,
// the IIci's RBV, the TNT's Control, Gossamer's Rage Pro) marked connected
// by the configuration wins; otherwise the machine's own built-in video
// (substrate .display -- Plus, Lisa, the DAFB, CIVIC, Ariel).  Returns NULL
// when no machine is booted or the booted machine has no screen (a IIcx with
// no card seated, or a configuration that plugs no monitor in).
display_t *system_display(void) {
    NOT_DURING_CONSTRUCTION();
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return NULL;
    display_t *connected = cfg->nubus ? nubus_connected_display(cfg->nubus) : NULL;
    if (!connected && cfg->pci)
        connected = pci_connected_display(cfg->pci);
    if (connected)
        return connected;
    return cfg->machine->substrate->display ? cfg->machine->substrate->display(cfg) : NULL;
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
// The machine's floppy drives: the drives its controller was built with (a
// position the configuration left empty has none), never more than the
// controller's two.  Drive selection is bounded by this, not by
// FLOPPY_NUM_DRIVES -- a one-drive Mac has no drive 1 to pick.
static int sys_fd_count(config_t *cfg) {
    int n = cfg->floppy ? floppy_drive_count(cfg->floppy) : profile_floppy_count(cfg->machine);
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
        gs_outf("%s: no such floppy drive %d (this machine has %d).\n", who, preferred, n);
        return -1;
    }
    if (preferred != -1) {
        if (sys_fd_is_inserted(cfg, preferred)) {
            gs_outf("%s: floppy drive %d is already occupied.\n", who, preferred);
            return -1;
        }
        return preferred;
    }
    for (int d = 0; d < n; d++)
        if (!sys_fd_is_inserted(cfg, d))
            return d;
    if (n == 0)
        gs_outf("%s: this machine has no floppy drive.\n", who);
    else
        gs_outf("%s: no free floppy drive.\n", who);
    return -1;
}

static int do_insert_fd(const char *path, int preferred, int writable_flag) {
    bool writable = (writable_flag != 0); // default to writable unless explicitly 0

    config_t *config = global_emulator;
    if (!config) {
        gs_outf("fd insert: emulator config not initialized.\n");
        return -1;
    }
    int target = sys_fd_pick(config, preferred, "fd insert");
    if (target < 0)
        return -1;

    image_t *disk = writable ? image_create(path, pick_delta_dir(path)) : image_open_readonly(path);
    if (!disk) {
        gs_outf("fd insert: failed to open disk image: %s\n", path);
        return -1;
    }

    // Register the image only once the drive has taken it.  This ignored the
    // insert's result and printed "inserted" whatever the drive said, so a
    // drive that refused left an orphan on the image list and a success claim.
    if (sys_fd_insert(config, target, disk) != 0) {
        gs_outf("fd insert: floppy drive %d refused %s.\n", target, path);
        image_close(disk);
        return -1;
    }
    add_image(config, disk);
    gs_outf("fd insert: inserted %s into floppy drive %d.\n", path, target);
    return 0;
}

// Probe a file to check if it's a valid floppy image (without inserting).
// Returns 0 if valid floppy, 1 if not.
int system_probe_floppy(const char *path) {
    image_t *disk = image_open_readonly(path);
    if (!disk) {
        gs_outf("%s: NOT a supported format\n", path);
        return 1;
    }

    if (!image_is_floppy(disk->type)) {
        gs_outf("%s: Valid disk image but not a floppy (size: %zu bytes)\n", path, disk->raw_size);
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

    gs_outf("%s: Valid floppy image (%s)\n", path, type_str);
    image_close(disk);
    return 0;
}

// Create a new blank floppy image and insert it.
// Returns 0 on success, -1 on failure.
int system_create_floppy(const char *path, bool high_density, int preferred) {
    config_t *config = global_emulator;
    if (!config) {
        gs_outf("fd create: emulator config not initialized.\n");
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
            gs_outf("fd create: file already exists: %s (won't overwrite)\n", path);
        else
            gs_outf("fd create: failed to create blank floppy file: %s\n", path);
        return -1;
    }

    image_t *disk = image_create(path, pick_delta_dir(path));
    if (!disk) {
        gs_outf("fd create: failed to open newly created image: %s\n", path);
        return -1;
    }

    if (sys_fd_insert(config, target, disk) != 0) {
        gs_outf("fd create: floppy drive %d refused %s.\n", target, path);
        image_close(disk);
        return -1;
    }
    add_image(config, disk);
    gs_outf("fd create: created %s (%s) and inserted into drive %d.\n", path, high_density ? "1440K" : "800K", target);
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
        gs_outf("hd create: invalid size: %s\n", size_str);
        gs_outf("  Use a drive model (e.g. HD20SC), human size (e.g. 40mb),\n");
        gs_outf("  or exact bytes/suffix (e.g. 20M, 512K, 21411840)\n");
        gs_outf("  Run 'hd models' to see available drive sizes.\n");
        return -1;
    }
    if (size > HD_CREATE_MAX_SIZE) {
        gs_outf("hd create: size %zu exceeds maximum (2 GiB)\n", size);
        return -1;
    }
    // reject floppy-sized images
    if (size == FLOPPY_400K_BYTES || size == FLOPPY_800K_BYTES || size == FLOPPY_1440K_BYTES) {
        gs_outf("hd create: size %zu matches a floppy format, use fd create instead\n", size);
        return -1;
    }
    // refuse to overwrite existing files
    FILE *exist = fopen(path, "rb");
    if (exist) {
        fclose(exist);
        gs_outf("hd create: file already exists: %s (won't overwrite)\n", path);
        return -1;
    }
    // A .dmg is a UDIF of one zero run -- a couple of KB however large the
    // disk; any other name is a raw file of the full size (which the browser
    // charges in full).
    size_t plen = strlen(path);
    bool udif = plen >= 4 && strcasecmp(path + plen - 4, ".dmg") == 0;
    int rc = udif ? image_create_empty_udif(path, size) : image_create_empty(path, size);
    if (rc != 0) {
        gs_outf("hd create: failed to create image: %s\n", path);
        return -1;
    }
    gs_outf("hd create: created %s (%zu bytes%s)\n", path, size, udif ? ", UDIF" : "");
    return 0;
}

// Attach a SCSI hard disk image. Delegates to add_scsi_drive().
// Returns 0 on success, -1 on error.
static int do_attach_hd_on(struct scsi *bus, const char *path, int scsi_id) {
    if (scsi_id < 0 || scsi_id > 7) {
        gs_outf("hd attach: invalid SCSI ID %d (expected 0..7)\n", scsi_id);
        return -1;
    }
    config_t *config = global_emulator;
    if (!config) {
        gs_outf("hd attach: emulator not initialized.\n");
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
    gs_outf("Granny Smith build %s\n", build_id_get());

    // Built-in machine profiles are a static const array in machine.c
    // (machine_find / machine_list walk it) — no runtime registration needed.

    // Create every category the manifest declares, so `log.levels` lists the
    // complete set rather than only what has already been
    // hit or configured.  This replaces a one-off registration of
    // "appletalk" that existed for exactly this reason -- and whose presence
    // was the tell that a manifest was missing.
    log_register_manifest();

    image_init(NULL);

    // The AppleTalk network: host state, one per process.  Machines plug into
    // it as they are built (atalk_conn_new) and never tear it down.
    appletalk_network_init();
}

// The default AppleShare volume.  The platform names its path once, at
// startup, after setup_init (the browser: /opfs/shared; headless:
// --shared-dir or $GS_SHARED_DIR), and the network publishes it then: a
// share is the network's, so it is there for every machine that plugs in.
// A failure is a logged warning, never a startup error -- a user who removed
// the directory still gets a running emulator.
#define GS_DEFAULT_SHARE_NAME "Shared"

void system_set_default_share(const char *path) {
    if (!path || !*path || atalk_afp_volume_find(GS_DEFAULT_SHARE_NAME) >= 0)
        return;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        LOG(0, "warning: default share: cannot create %s: %s", path, strerror(errno));
        return;
    }
    char err[192];
    if (atalk_afp_volume_add(GS_DEFAULT_SHARE_NAME, path, err, sizeof(err)) < 0)
        LOG(0, "warning: default share: %s", err);
}

// Background-checkpoint auto state. WASM-only at the moment — the
// headless build has no auto-checkpoint loop, so the weak defaults
// just stub out; em_main.c overrides them to read/write the live
// `checkpoint_auto_enabled` flag.
// A new machine is the active one (machine.boot, checkpoint.load).  The host
// re-bases whatever it samples from the machine: nothing it observed of the
// previous machine is compared with this one (the page's MIPS sample, headless
// --max-cycles' count).  The weak default serves a host that samples nothing.
__attribute__((weak)) void platform_machine_attached(void) {}

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
static bool g_quick_verbose = false;
static char g_quick_final_path[QUICK_CHECKPOINT_PATH_MAX];

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
    // No machine configured → nothing to save, like the idle and
    // no-directory cases below (a hidden tab before any boot lands here).
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        if (verbose)
            gs_outf("[checkpoint] no machine, nothing to save\n");
        return GS_SUCCESS;
    }

    // Skip checkpointing when the emulator is idle — nothing meaningful to save
    if (!scheduler_is_running(sched) && cpu_instr_count() == 0)
        return GS_SUCCESS;

    // No machine identity yet → nothing to save under.
    if (!checkpoint_machine_dir()) {
        if (verbose)
            gs_outf("[checkpoint] no machine directory set, skipping quick checkpoint\n");
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

    // Settle the sprint counters so the checkpoint captures an exact
    // instruction count.  Not a stop: the machine's run state is saved as
    // it is, and no mode ends (the run-state event would otherwise report
    // a pause the user never asked for).
    cpu_reschedule(sched);

    // The previous save's write is still in flight on the I/O worker: the
    // buffer is its, and a save now would have nothing to save into.  Skip
    // (the rate limit already says "not yet") and count.
    if (checkpoint_quick_in_flight()) {
        checkpoint_quick_note_skipped();
        return GS_SUCCESS;
    }

    // Serialise here (the guest state is this thread's); the write and the
    // rename over final_path run on the I/O worker, which reports through
    // system_quick_checkpoint_written -- or, without a worker, inline and
    // before system_checkpoint returns.
    g_quick_verbose = verbose;
    snprintf(g_quick_final_path, sizeof g_quick_final_path, "%s", final_path);
    checkpoint_publish_next(final_path);
    int rc = system_checkpoint(tmp_path, CHECKPOINT_KIND_QUICK);
    if (rc != GS_SUCCESS) {
        checkpoint_publish_next(NULL);
        unlink(tmp_path);
        if (verbose)
            gs_outf("[checkpoint] quick checkpoint failed (%s)\n", reason ? reason : "background");
        return rc;
    }
    g_last_quick_checkpoint_ms = now;
    return GS_SUCCESS;
}

void system_quick_checkpoint_written(bool ok, double ms, const char *error) {
    if (ok) {
        gs_event_emitf(GS_EVENT_NOTIFY, "{\"event\":\"checkpoint_saved\",\"elapsed_ms\":%.2f}", ms);
        if (g_quick_verbose)
            gs_outf("Checkpoint saved to %s (%.2f ms)\n", g_quick_final_path, ms);
    } else {
        gs_outf("[checkpoint] quick checkpoint write failed: %s\n", error ? error : "?");
    }
}

int gs_background_checkpoint(const char *reason) {
    // A snapshot promises a complete file when it returns: let a publish in
    // flight land first, save, and wait for this one's publish too.
    checkpoint_quick_wait();
    int rc = system_quick_checkpoint(reason ? reason : "manual", true, false);
    checkpoint_quick_wait();
    return rc == GS_SUCCESS ? 0 : -1;
}

// Clear checkpoint files inside the current machine directory: drops
// True when `path` is the delta or journal of an image the machine holds
// open: clearing must not pull a live file from under it.
static bool image_file_in_use(const char *path) {
    config_t *cfg = global_emulator;
    for (int i = 0; cfg && i < cfg->n_images; i++) {
        const image_t *img = cfg->images[i];
        if (!img)
            continue;
        if ((img->delta_path && strcmp(img->delta_path, path) == 0) ||
            (img->journal_path && strcmp(img->journal_path, path) == 0))
            return true;
    }
    return false;
}

// state.checkpoint, any leftover *.tmp, and (defensive) any legacy
// sequence-numbered *.checkpoint / *.pending / *.complete files, plus the
// image deltas and journals of the discarded state: a delta the checkpoint
// no longer refers to can never be reached again, and each session left
// one behind, a disk-sized file the browser's site data hid (#149).  The
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
        else if ((len >= 6 && strcmp(name + len - 6, ".delta") == 0) ||
                 (len >= 8 && strcmp(name + len - 8, ".journal") == 0))
            match = true; // an image delta or journal, unless still open below
        if (match) {
            snprintf(path, sizeof(path), "%s/%s", dir_path, name);
            if (image_file_in_use(path))
                continue;
            if (unlink(path) == 0)
                removed++;
        }
    }
    closedir(dir);
    return removed;
}

int gs_checkpoint_clear(void) {
    int removed = clear_checkpoint_files();
    gs_outf("Cleared %d checkpoint file(s)\n", removed);
    return 0;
}

int gs_register_machine(const char *machine_id, const char *created) {
    if (!machine_id || !created)
        return -1;
    int rc = checkpoint_machine_set(machine_id, created);
    if (rc != 0)
        gs_outf("register_machine: failed to set %s-%s\n", machine_id, created);
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
        gs_outf("find-media: cannot open '%s': %s\n", dir_path, strerror(errno));
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

    gs_outf("%s\n", found_path);
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
// The board's block: the model and the RAM size, which the rest of the
// machine is built for.  It is the checkpoint's first part, so a restore
// reads it before it builds anything (system_restore).
typedef struct {
    char model[40];
    uint32_t ram_kb;
} board_block_t;

static void board_part_save(void *obj, checkpoint_t *cp) {
    const config_t *cfg = obj;
    board_block_t b;
    memset(&b, 0, sizeof b);
    snprintf(b.model, sizeof b.model, "%s", cfg->machine->id);
    b.ram_kb = cfg->ram_size / 1024u;
    system_write_checkpoint_data(cp, &b, sizeof b, "machine");
}

// The storage devices part: their count, then the devices.
static void storage_part_save(void *obj, checkpoint_t *cp) {
    const config_t *cfg = obj;
    int32_t n = cfg->n_storage;
    system_write_checkpoint_data(cp, &n, sizeof n, "storage");
    system_write_checkpoint_data(cp, cfg->storage, (size_t)n * sizeof cfg->storage[0], "storage");
}

// The storage devices a machine is built with: a restore's from its
// checkpoint, a boot's from the document, else the model's default
// configuration.
static void storage_part(config_t *cfg, checkpoint_t *cp) {
    machine_part_begin(cfg, cp, "storage");
    if (cp) {
        int32_t n = 0;
        system_read_checkpoint_data(cp, &n, sizeof n, "storage");
        if (n < 0 || n > MACHINE_STORAGE_MAX) {
            checkpoint_set_error(cp);
            n = 0;
        }
        system_read_checkpoint_data(cp, cfg->storage, (size_t)n * sizeof cfg->storage[0], "storage");
        cfg->n_storage = n;
        for (int i = 0; i < n; i++)
            cfg->storage[i].bus[sizeof cfg->storage[i].bus - 1] = '\0';
    } else if (cfg->build_opts.storage_given) {
        cfg->n_storage = cfg->build_opts.n_storage;
        memcpy(cfg->storage, cfg->build_opts.storage, sizeof cfg->storage);
    } else {
        for (const storage_device_decl_t *s = cfg->machine->default_storage;
             s && s->bus && cfg->n_storage < MACHINE_STORAGE_MAX; s++) {
            machine_storage_dev_t *d = &cfg->storage[cfg->n_storage++];
            snprintf(d->bus, sizeof d->bus, "%s", s->bus);
            d->unit = s->unit;
            d->type = s->type;
        }
    }
    machine_part(cfg, cp, "storage", storage_part_save, cfg);
}

static void events_part_save(void *obj, checkpoint_t *cp) {
    scheduler_checkpoint_events(obj, cp);
}

// Put the running machine's memory map back after a build selected its own:
// the aliases memory_map_select derives, plus the access pair the CPU had
// chosen, which selection resets to supervisor (a 68000 re-picks it only on a
// mode change).
static void reselect_active_map(uintptr_t *active_read, uintptr_t *active_write) {
    memory_map_select(global_emulator ? global_emulator->mem_map : NULL);
    if (global_emulator) {
        g_active_read = active_read;
        g_active_write = active_write;
    }
}

config_t *system_create(const hw_profile_t *profile, const machine_build_opts_t *opts, checkpoint_t *checkpoint) {

    assert(profile != NULL);
    assert(profile->substrate != NULL && profile->substrate->init != NULL);
    assert(opts != NULL && opts->ram_kb != 0);
    assert(checkpoint != NULL || (opts->rom.data != NULL && opts->rom.size != 0)); // a machine has its ROM

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg)
        return NULL;
    memset(cfg, 0, sizeof(config_t));
    cfg->build_opts = *opts;

    // The build selects its own memory map (memory_map_init); afterwards the
    // running machine's goes back exactly as it was, down to the access mode
    // its CPU had the fast path in.
    uintptr_t *const active_read = g_active_read, *const active_write = g_active_write;

    cfg->machine = profile;
    // Main-CPU architecture tag: derived from the
    // profile's cpu_model; the substrate init below builds the matching core.
    cfg->cpu_arch = cpu_arch_for_model(profile->cpu_model);

    // The RAM size is a build option: the caller validated and defaulted it.
    cfg->ram_size = cfg->build_opts.ram_kb * 1024u;

    // The board is the checkpoint's first part: what the machine is.  On a
    // restore, system_restore has read it -- name and block -- to choose the
    // model and the RAM size it is built with, so nothing is read here.
    machine_part_begin(cfg, NULL, "machine");
    machine_part(cfg, checkpoint, "machine", board_part_save, cfg);
    storage_part(cfg, checkpoint);

    // Delegate all machine-specific initialisation to the profile.  A
    // non-zero return means the machine could not be built (the only cause
    // today is an allocation failure); tear down whatever it managed and
    // report the failure rather than handing back a half-built config.  Every
    // teardown tolerates a partially-constructed machine -- each guards its
    // machine_context -- which is what makes this safe to call here.
    s_constructing = true;
    gs_event_hold(1);
    if (profile->substrate->init(cfg, checkpoint) != 0) {
        s_constructing = false;
        gs_event_hold(0);
        LOG(0, "Error: failed to construct %s", profile->name);
        if (profile->substrate->teardown)
            profile->substrate->teardown(cfg);
        machine_parts_free(cfg);
        free(cfg);
        reselect_active_map(active_read, active_write);
        return NULL;
    }

    // The seeding step: a NEW machine's parameter memory gets the records that
    // follow from its configuration -- the startup device, AppleTalk -- after
    // every device and its factory content exist and before the first
    // instruction.  Never on a restore: the store came back from the
    // checkpoint, and whatever the guest chose since is state.
    if (!checkpoint && profile->substrate->seed)
        profile->substrate->seed(cfg);

    // The build options are construction's arguments, and construction is
    // over: every device took what it needed (the memory map copied the ROM,
    // the buses their slot entries, the video its monitor strap).  Nothing
    // reads them again, so the machine keeps none of them.
    cfg->build_opts = machine_build_opts_default();

    // A new machine powers on: the CPU starts from its reset vector, with the
    // ROM already in place, by the same path every reset takes.  Every device
    // was constructed in its power-on state, so the board's /RESET net has
    // nothing to do.  A restored CPU carries its own state.
    if (!checkpoint)
        system_cpu_reset(cfg);

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

    // The `machine.adb.keyboard` object, per machine.  After the substrate
    // because it wants the scheduler.
    cfg->host_input = host_input_init(cfg, cfg->scheduler);

    // The event queue is the checkpoint's last part, restored once every
    // event source has been constructed and registered its types.
    machine_part_begin(cfg, checkpoint, "events");
    if (checkpoint)
        scheduler_restore_events(cfg->scheduler, checkpoint);
    machine_part(cfg, checkpoint, "events", events_part_save, cfg->scheduler);
    s_constructing = false;
    gs_event_hold(0);

    // The fast-path aliases go back to the active machine's until the swap
    // step.
    reselect_active_map(active_read, active_write);

    return cfg;
}

// Make a constructed machine the active one, then destroy the machine it
// replaces: the swap step of build, swap, destroy.  Everything that names "the
// active machine" changes here and nowhere else, so a build that fails --
// system_create returning NULL, or a restore whose checkpoint flagged an
// error -- has changed nothing and leaves nothing to put back.
void system_swap_in(config_t *cfg, bool restored, const struct host_pacing *pacing) {
    config_t *old = global_emulator;
    global_emulator = cfg;
    memory_map_select(cfg->mem_map);

    // Label the machine container node with the active model name so the
    // SYSTEM tab shows "Macintosh IIcx" rather than the bare "machine"
    // segment.
    machine_set_active_label(cfg->machine->name);

    // The expansion buses' slot trees (machine.nubus.slot[N], machine.pci.
    // slot[N]) -- and with them machine.screen.source -- project this
    // machine's cards.  They are process state, so they change here, never
    // while a machine is being built.  Before root_install, which attaches the
    // bus nodes they register.
    if (cfg->nubus)
        nubus_objects_build(cfg->nubus);
    if (cfg->pci)
        pci_objects_build(cfg->pci);

    // Stand up the object-model root: attaches stub classes for
    // cpu/memory/scheduler/machine/shell/storage so `eval` can read
    // runtime state.
    root_install(cfg);

    // The debugger's node and the memory-logpoint hook it owns.
    debug_activate(cfg->debugger);

    // The new machine takes the AppleTalk cable.  The one it replaces comes
    // off it now, its sessions closing as a server sees a Mac vanish; the
    // network itself is untouched.
    atalk_conn_plug(cfg->atalk);

    // Cold boot: stamp out a manifest documenting what was set up.  Skipped
    // on checkpoint restore — the manifest is fixed at original creation
    // time and is purely informational.  Failure is non-fatal.
    if (!restored && checkpoint_machine_dir())
        checkpoint_machine_write_manifest();

    // The machine it replaces goes last; its teardown leaves the new
    // machine's object tree alone (root_uninstall_if).
    if (old)
        system_destroy(old);

    // A new machine is active: machine.boot and checkpoint.load both end here
    // (machine.restart builds nothing).  The page reloads its object trees on this.
    // The machine runs as the host says from its first frame: a machine is
    // built at the default pacing and given the host's here.
    scheduler_apply_pacing(cfg->scheduler, pacing);
    scheduler_announce_speed(cfg->scheduler);
    gs_event_emitf(GS_EVENT_STATE, "{\"event\":\"machine_booted\",\"model\":\"%s\",\"restored\":%s}",
                   cfg->machine->id ? cfg->machine->id : "", restored ? "true" : "false");
    platform_machine_attached();
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
    // The guard is what keeps the swap step working: it installs the new
    // config first and destroys the old one after, so by the time this runs
    // the global already names someone else and must not be cleared; a build
    // that failed was never installed at all.
    if (global_emulator == config)
        global_emulator = NULL;

    machine_parts_free(config);
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
        gs_outf("Cannot attach: no image path\n");
        return false;
    }
    if (bus == MEDIA_BUS_PROFILE) {
        const image_geometry_t geom = {.block_size = PROFILE_BLOCK_SIZE};
        slot->img = image_create_with_geometry(path, pick_delta_dir(path), geom);
        if (!slot->img)
            gs_outf("Failed to open ProFile image: %s\n", path);
        return slot->img != NULL;
    }
    if (cdrom) {
        // The drive is the one the machine's profile declares for its CD bay.
        const struct scsi_cd_drive *drive = global_emulator ? global_emulator->machine->cdrom_drive : NULL;
        if (!drive) {
            gs_outf("Cannot attach a CD-ROM: this machine takes no CD-ROM drive\n");
            return false;
        }
        // CD-ROM images are always opened read-only
        slot->img = image_open_readonly(path);
        if (!slot->img) {
            gs_outf("Failed to open CD-ROM image: %s\n", path);
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
        slot->block_size = drive->block_size;
        slot->read_only = true;
        snprintf(slot->vendor, sizeof(slot->vendor), "%s", drive->vendor);
        snprintf(slot->product, sizeof(slot->product), "%s", drive->product);
        snprintf(slot->revision, sizeof(slot->revision), "%s", drive->revision);
        gs_outf("Attaching SCSI CD-ROM: %s as %s %s (size: %zu bytes, %u-byte blocks)\n", path, drive->vendor,
                drive->product, disk_size(slot->img), slot->block_size);
        return true;
    }
    slot->img = image_create(path, pick_delta_dir(path));
    if (!slot->img) {
        gs_outf("Failed to open image: %s\n", path);
        return false;
    }
    // An HFS volume with no driver in front of it (a bare volume, or a
    // partition map without a driver partition) is invisible to the ROM's
    // SCSI boot code; present it behind a synthesised map and the GSDisk
    // driver instead.  The file itself is untouched.
    int wrapped = image_wrap_volume(slot->img);
    if (wrapped < 0) {
        gs_outf("Failed to wrap volume: %s\n", path);
        image_close(slot->img);
        slot->img = NULL;
        return false;
    }
    if (wrapped == IMAGE_WRAP_BARE)
        gs_outf("%s: bare HFS volume — wrapped with a partition map and the GSDisk driver\n", path);
    else if (wrapped == IMAGE_WRAP_DRIVERLESS)
        gs_outf("%s: partitioned disk without a driver — its HFS partition wrapped with a partition map and the "
                "GSDisk driver\n",
                path);
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
        gs_outf("Cannot attach %s: this machine has no SCSI bus\n", filename);
        return false;
    }
    media_slot_t slot;
    if (!media_open(MEDIA_BUS_SCSI, false, filename, &slot))
        return false;
    slot.unit = scsi_id;
    if (system_media_attach_scsi_bus(config, bus, &slot) != 0) {
        image_close(slot.img);
        return false;
    }
    return true;
}

// Add a SCSI CD-ROM to the configuration (AppleCD SC Plus / Sony CDU-8002)
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    return add_scsi_cdrom_on(config, config ? config->scsi : NULL, filename, scsi_id);
}

// ...on a NAMED bus; see add_scsi_drive_on.
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    if (!bus) {
        gs_outf("Cannot attach CD-ROM %s: this machine has no SCSI bus\n", filename);
        return false;
    }
    media_slot_t slot;
    if (!media_open(MEDIA_BUS_SCSI, true, filename, &slot))
        return false;
    slot.unit = scsi_id;
    if (system_media_attach_scsi_bus(config, bus, &slot) != 0) {
        image_close(slot.img);
        return false;
    }
    return true;
}

// === Media attach ===========================================================
//
// The standard substrate implementation over cfg->floppy + cfg->scsi, bound
// into every Mac substrate's vtable (the Lisa implements its own: parallel
// FDC + ProFile).  It hands an opened image to the same device paths a fresh
// mount uses, minus the open-by-path step.

// Attach one opened medium to the running machine.  Returns 0 on success
// (the machine owns the handle), <0 when the medium cannot be attached (the
// caller must close the handle).
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

// Attach one opened medium to a named SCSI bus: a machine may have more than
// one visible bus, and a SCSI id does not identify a device on its own there
// (the Network Servers' two fast/wide channels).
int system_media_attach_scsi_bus(config_t *cfg, struct scsi *bus, const media_slot_t *slot) {
    if (!bus)
        return -1;
    // A CD-ROM drive is construction: a hard disk cannot replace one, or the
    // next checkpoint would hold a bus no restore of this machine can rebuild.
    if (slot->scsi_type != scsi_dev_cdrom && slot->unit >= 0 && scsi_device_is_cd_drive(bus, (unsigned)slot->unit)) {
        gs_outf("Cannot attach %s at SCSI id %d: that is a CD-ROM drive (insert a CD there instead)\n",
                slot->img ? image_get_filename(slot->img) : "the image", slot->unit);
        return -1;
    }
    add_image(cfg, slot->img);
    scsi_add_device(bus, slot->unit, slot->vendor, slot->product, slot->revision, slot->img,
                    (enum scsi_device_type)slot->scsi_type, slot->block_size, slot->read_only);
    return 0;
}

// === Machine-level attach and eject ===========================
//
// One verb for "put this disk in that bay", whatever bus the bay is on: the
// one substrate dispatch (media_attach), fed by media_open.  Before,
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
    double start_time = host_time_ms();

    checkpoint_t *checkpoint = checkpoint_open_write(filename, kind);
    if (!checkpoint) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: failed to open checkpoint file for writing: %s", filename);
        return GS_ERROR;
    }

    // Every part of the machine, in the order it was built: the board first,
    // the event queue last (machine_parts.h).
    machine_parts_save(global_emulator, checkpoint);

    if (checkpoint_has_error(checkpoint)) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: failed to write checkpoint");
        checkpoint_close(checkpoint);
        return GS_ERROR;
    }

    checkpoint_close(checkpoint);

    double elapsed_ms = host_time_ms() - start_time;
    // Ambient by default — the browser's background auto-saves land here
    // every ~15 s and used to spam the terminal. `log.set ckpt 1`
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

    // The board's block comes first: the model and the RAM size to build.
    // Everything else -- the cards in the slots, their ROMs and options, the
    // monitor straps -- is read by the part it belongs to as the machine is
    // built.
    board_block_t board;
    machine_part_expect(checkpoint, "machine", 0);
    system_read_checkpoint_data(checkpoint, &board, sizeof board, "machine");
    board.model[sizeof board.model - 1] = '\0';
    const hw_profile_t *profile = checkpoint_has_error(checkpoint) ? NULL : machine_find(board.model);
    if (!profile) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: checkpoint %s names no model this build has ('%s')",
                 filename, board.model);
        checkpoint_close(checkpoint);
        return NULL;
    }

    // The RAM size is validated like a boot's: a size the model does not
    // offer rejects the restore rather than clamping.
    if (!hw_profile_ram_option_allowed(profile, board.ram_kb)) {
        LOG_WITH(log_register_category("ckpt"), 0, "Error: checkpoint RAM %u KB is not a size %s offers", board.ram_kb,
                 profile->name);
        checkpoint_close(checkpoint);
        return NULL;
    }

    machine_build_opts_t build_opts = machine_build_opts_default();
    build_opts.ram_kb = board.ram_kb;

    config_t *config = system_create(profile, &build_opts, checkpoint);

    // The build is not yet the active machine, so a failure leaves the
    // running one exactly as it was.
    if (!config || checkpoint_has_error(checkpoint)) {
        LOG(0, "Error: Failed to read checkpoint");
        checkpoint_close(checkpoint);
        system_destroy(config);
        return NULL;
    }

    checkpoint_close(checkpoint);

    LOG_WITH(log_register_category("ckpt"), 1, "Checkpoint restored from %s", filename);
    return config;
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

    config_t *new_config = system_restore(filename);
    if (!new_config)
        return -1;

    // Swap the restored machine in; the old one is destroyed.  Safe because
    // commands are registered globally rather than per-config, and no part of
    // the call stack holds the old config.
    system_swap_in(new_config, true, platform_pacing());

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
// scsi.attach_hd, files.hd_create) call directly — no line
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
