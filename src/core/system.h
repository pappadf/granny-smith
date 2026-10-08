// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.h
// Public interface for system setup and configuration.

#ifndef SYSTEM_H
#define SYSTEM_H

// === Includes ===
#include "machine_build_opts.h"
#include <stdbool.h>
#include <stddef.h>

#include "checkpoint.h"
#include "common.h"
#include "debug.h"
#include "drive_activity.h"
#include "image.h"
#include "keyboard.h"
#include "machine_profile.h" // enum media_bus, media_slot_t
#include "platform.h"
#include "scheduler.h"
#include "status.h"

struct ram;
typedef struct ram ram_t;

struct sound;
typedef struct sound sound_t;

struct rtc;
typedef struct rtc rtc_t;

struct memory;
typedef struct memory memory_map_t;

struct memory_interface;
typedef struct memory_interface memory_interface_t;

struct cpu;
typedef struct cpu cpu_t;

struct via;
typedef struct via via_t;

struct mouse;
typedef struct mouse mouse_t;

struct keyboard;
typedef struct keyboard keyboard_t;

struct scc;
typedef struct scc scc_t;

struct scsi;
typedef struct scsi scsi_t;

struct floppy;
typedef struct floppy floppy_t;

// Forward declaration for hw_profile_t (defined in machine.h)
struct hw_profile;
typedef struct hw_profile hw_profile_t;

// Opaque emulator configuration handle
struct config;
typedef struct config config_t;

// Config field accessors (opaque handle access)
image_t *config_get_image(config_t *cfg, int index);
int config_get_n_images(config_t *cfg);
void config_add_image(config_t *cfg, image_t *image);
// Standard substrate implementation of media attach (cfg->floppy +
// cfg->scsi); Mac substrates bind it into their vtables, the Lisa provides
// its own.  See machine_profile.h media_slot_t.
struct media_slot;
int system_media_attach_std(config_t *cfg, const struct media_slot *slot);
// One SCSI bus's worth of the same, for a machine with more than one.
int system_media_attach_scsi_bus(config_t *cfg, struct scsi *bus, const struct media_slot *slot);

// === Generic Machine Lifecycle ===

// One-time global initialisation: logging categories, image system, the
// AppleTalk network.
void system_init(void);

// Publish the directory the default "Shared" AppleShare volume serves (NULL
// or "": none) on the AppleTalk network.  Called once, at startup, after
// system_init; the share stays for every machine that plugs in.
void system_set_default_share(const char *path);

// Create an emulator instance for the given machine profile.
// If checkpoint is non-NULL, device state is restored from that checkpoint.
// Returns the new config handle, NOT yet the active machine: construction
// touches nothing outside it (global_emulator, the object root, the machine
// label), so a failed build has changed nothing.  system_swap_in makes it
// the active machine.  `opts` carries the construction arguments (see
// machine_build_opts.h); machine_boot_apply and the checkpoint restore both
// fill it.
extern config_t *system_create(const hw_profile_t *profile, const machine_build_opts_t *opts, checkpoint_t *checkpoint);

// Make a constructed machine the active one -- global_emulator, the object
// root, the machine label, the machine_booted event -- and destroy the one it
// replaces.  `restored`: the machine came from a checkpoint.  `pacing`: the
// host's pacing setting, which the machine runs under from its first frame.
struct host_pacing;
void system_swap_in(config_t *cfg, bool restored, const struct host_pacing *pacing);

// Destroy an emulator instance: call machine teardown and free all resources.
extern void system_destroy(config_t *config);

// The active machine (NULL before the first boot).  Ownership contract:
// system.c is the only writer -- system_swap_in publishes a fully built
// config and system_destroy clears the pointer when it destroys the config it
// names -- and every reader is expected to run on the emulator thread, so
// no barrier is needed today (nothing enforces this; a reader on another
// thread needs a real publication protocol first).  A constructor never reads it
// (it uses the cfg it was given; the system_*() accessors assert this).
// Prefer system_config() / system_running() over reading it directly.
extern config_t *global_emulator;

// Pulse the machine's vertical-blanking line: the scheduler's per-frame tick,
// dispatched to the substrate's trigger_vbl (defined in machines/machine.c).
void trigger_vbl(config_t *restrict config);

// Save current machine state to a checkpoint file.
// STATUS_OK on success; STATUS_E_NOENT with no machine, STATUS_E_IO when the
// file cannot be opened, written or finished.
status_t system_checkpoint(const char *filename, checkpoint_kind_t kind);

// Restore machine state from a checkpoint file.
// Returns a new config on success, NULL on failure.
config_t *system_restore(const char *filename);

// Command handlers for checkpoint operations
// Checkpoint load / probe.  These replace the retired
// cmd_load_checkpoint(argc, argv), which the typed checkpoint.* methods
// reached by building a fake argv[] and then string-matching their arguments
// back out of it.
int system_checkpoint_load(const char *filename); // NULL/empty = auto-load latest
bool system_checkpoint_probe(void);

// System-level input wrappers (route to appropriate device models)
// Note: system_keyboard_update requires keyboard.h to be included for key_event_t
void system_mouse_update(bool button, int dx, int dy);
bool system_mouse_move(int dx, int dy);
bool system_mouse_pending_adb(int *dx, int *dy);
void system_keyboard_update(key_event_t event, int key);

// The reset levels (machine_profile.h).  These three are declared without
// __attribute__((weak)): the attribute sits on their definitions in system.c,
// so a test that links a stub (tests/unit/support/stub_system.c) overrides
// them with an ordinary strong definition.
//
// Level 2 -- a machine reset: the board's /RESET net plus the CPU back to its
// reset vector.  The reset button, machine.reset(), Finder > Restart, the
// Cuda's CMD_RESET, a double bus fault (HALT -> GLU RESET).
void system_machine_reset(void);

// Level 3 -- a power cycle: the machine stays standing, RAM goes cold, and
// everything else is a level-2 reset.  No device is freed or rebuilt, so the
// non-volatile stores, the RTC counter, mounted media and latched switches
// survive for the hardware's own reason: nothing destroyed them.
void system_machine_power_cycle(void);

// Level 1 -- the bus /RESET line asserted by the 68k RESET instruction: reset
// the external peripheral chips (SCSI, NuBus cards) to power-on state,
// leaving the CPU core (registers / caches / MMU) untouched.  Called from
// OP_RESET; the boot ROM relies on this during a warm restart.
void system_reset_devices(void);

// The devices every Macintosh board wires to /RESET.  A family's bus_reset
// calls this, then resets its own chipset.
void system_reset_common_devices(struct config *cfg);

// Accessors for the ACTIVE machine's subsystems.  Naming convention: an
// accessor is the bare noun (system_scheduler, system_cpu, system_display),
// an action is a verb phrase (system_machine_reset, system_swap_in) -- no
// get_ prefix.  Each answers NULL when no machine is active and asserts when
// called while a machine is being constructed (a constructor uses its cfg).

// System-level scheduler accessor: returns the current scheduler object
scheduler_t *system_scheduler(void);

// System-level memory accessor: returns the current memory object
memory_map_t *system_memory(void);

// System-level debug accessor: returns the current debugger object
debug_t *system_debug(void);

// System-level CPU accessor: returns the current CPU object
cpu_t *system_cpu(void);

// Main-CPU debug interface accessor: the vtable the
// debugger routes PC/disasm/translate through, populated by system_create
// from whichever core is the machine's main CPU.  NULL before a machine is
// built.  The pointed-to struct lives inside config_t (stable until destroy).
struct cpu_debug_if;
const struct cpu_debug_if *system_cpu_debug_if(void);

// The active machine configuration (NULL before setup).  Used by the keyboard /
// mouse object methods to find a machine-specific host-input hook.
config_t *system_config(void);
// The running machine, for an observer that may run while another machine is
// being built -- a log line's PC and instruction-count decoration describes
// the machine that is running, which a build does not change.  Constructors
// use their own cfg; everything else uses system_config().
config_t *system_running(void);
// The running machine's scheduler, or NULL with no machine.
struct scheduler *system_running_scheduler(void);

// Per-kind (DRIVE_KIND_*) sums of the attached images' read / write call
// counters, for the drive-activity lights (storage/drive_activity.h).
void system_drive_io_counts(uint64_t reads[DRIVE_KIND_COUNT], uint64_t writes[DRIVE_KIND_COUNT]);

// Host-input dispatch through the machine's substrate (a Mac's ADB or M0110A,
// the Lisa's COPS).  Each returns 0 when the machine took the request and -1
// when it refused it (a key its keyboard has not got, a bad mouse mode) or no
// machine is running.
// `adb_code` is an ADB raw keycode (0x00-0x7F) -- the model's universal
// key identity, see machine_profile.h.  Resolve names with
// debug_mac_resolve_key_name before calling.
int system_input_key(int adb_code, bool down);
// This machine's own keyboard wire byte; -1 if the machine has no raw form.
int system_input_key_raw(uint8_t byte);
int system_input_mouse_move(int x, int y, const char *mode);
int system_input_mouse_button(bool down, const char *mode);

// System-level RTC accessor: returns the current RTC object (or NULL)
rtc_t *system_rtc(void);

// System-level framebuffer accessor: returns pointer to video RAM buffer.
// Equivalent to system_display()->bits.
uint8_t *system_framebuffer(void);
// The active display after its pixels were made current: a card whose
// frame lives elsewhere (the Voodoo2 under the WebGPU takeover) reads
// it back first.  Use this, not system_display(), before READING bits.
display_t *system_display_synced(void);

// Forward declaration; full definition in nubus/display.h.
struct display;
typedef struct display display_t;

// System-level display accessor: returns the active display descriptor for
// whichever machine is currently booted, or NULL if no display is available.
// Consumers read fresh each frame; the renderer additionally consumes
// (reads and clears) the per-resource dirty flags on display_t to decide
// what to re-upload.  Pointer identity is stable across the machine's
// lifetime; the pointed-to fields are live-mutable.
display_t *system_display(void);

// Check if emulator is initialized and running
bool system_is_initialized(void);

// Return the model_id of the current machine, or NULL if none is active
const char *system_machine_model_id(void);

// Create a blank floppy image at `path` and auto-mount it. high_density
// chooses 1.44 MB vs 800 KB. preferred is the target drive (0 or 1; pass
// -1 to let the system pick the first free drive). Returns 0 on success
// or -1 on failure (file exists, both drives full, image system error).
int system_create_floppy(const char *path, bool high_density, int preferred);

// Attach a read-only CD-ROM image at the given SCSI id. Used by typed
// `cdrom_attach` and the legacy `cdrom attach` command alike — the
// underlying primitive opens the image, registers it as a SCSI device,
// and emits the legacy "Attaching CD-ROM" stdout message.
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id);
// The same, on an explicitly named SCSI bus.  A machine with more than one
// visible bus — the Apple Network Servers' two fast/wide 53C825A channels —
// needs `machine.scsi2.attach_cdrom` to land on the second one; NULL means
// the machine's primary bus and is what every Macintosh path passes.
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id);
// Attach a writable SCSI hard disk image (base + delta) on `bus` at `scsi_id`,
// presented as the closest catalog drive.  NULL bus: refused (no SCSI).
bool add_scsi_hd_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id);

// Probe a floppy image at `path`: opens it read-only and prints the
// detected density.
// Returns 0 if the image is a recognised floppy, non-zero otherwise.
int system_probe_floppy(const char *path);

// Whether a disk is in floppy drive `drive` (routes through the machine
// substrate's fd_present hook — Mac IWM/SWIM or the Lisa FDC).  Used by the
// platform tick to push drive-present changes to the UI.
bool system_fd_present(int drive);

// Typed object-model entry points for disk operations. The typed
// methods (floppy.drives[N].insert, scsi.attach_hd, files.hd_create)
// call these directly. Each returns 0 on success, negative on error.
int system_fd_insert(const char *path, int drive, bool writable);
int system_hd_attach(const char *path, int scsi_id);
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id);
int system_hd_create(const char *path, const char *size_str);

// The quick save's publish ended (emulator thread, from checkpoint.c): the
// checkpoint_saved event goes out here.
void system_quick_checkpoint_written(bool ok, double ms, const char *error);

// Checkpoints of the running machine, and finding media -- core, the same on
// every platform (system.c).  0 on success, non-zero on failure.
//
//   system_quick_checkpoint(reason, verbose, rate_limit)
//                            — save state.checkpoint in the machine's
//                              directory (tmp + rename), at most once per
//                              750 ms when rate-limited.  A status_t.
//   gs_background_checkpoint(reason)
//                            — the same, unthrottled and verbose.
//   gs_checkpoint_clear()    — delete the machine's checkpoint files.
//   gs_register_machine(id, created)
//                            — set the machine identity that scopes its
//                              checkpoint directory.
//   gs_find_media(dir, [dest])
//                            — find the first floppy image in `dir`,
//                              optionally copy it to `dest`; prints the
//                              path on success.
status_t system_quick_checkpoint(const char *reason, bool verbose, bool rate_limit);
int gs_background_checkpoint(const char *reason);
int gs_checkpoint_clear(void);
int gs_register_machine(const char *machine_id, const char *created);
int gs_find_media(const char *dir_path, const char *dest);

// The path of the active machine's current valid quick checkpoint
// (<machine_dir>/state.checkpoint from this build), in a static buffer, or
// NULL when there is none.
const char *find_valid_checkpoint_path(void);

// The platform seams (gs_* hooks with weak defaults) -- see platform_hooks.h.
// Included here until their callers include it themselves.
#include "platform_hooks.h"

#endif // SYSTEM_H
