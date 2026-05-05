// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system.h
// Public interface for system setup and configuration.

#ifndef SETUP_H
#define SETUP_H

// === Includes ===
#include <stdbool.h>
#include <stddef.h>

#include "checkpoint.h"
#include "common.h"
#include "debug.h"
#include "image.h"
#include "keyboard.h"
#include "platform.h"
#include "scheduler.h"

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

#define MAX_IMAGES 10

// Opaque emulator configuration handle
struct config;
typedef struct config config_t;

// Config field accessors (opaque handle access)
image_t *config_get_image(config_t *cfg, int index);
int config_get_n_images(config_t *cfg);
void config_add_image(config_t *cfg, image_t *image);

// === Generic Machine Lifecycle ===

// One-time global initialisation: logging categories, image system, shell commands.
extern void setup_init(void);

// Create an emulator instance for the given machine profile.
// If checkpoint is non-NULL, device state is restored from that checkpoint.
// Sets global_emulator and returns the new config handle.
extern config_t *system_create(const hw_profile_t *profile, checkpoint_t *checkpoint);

// Destroy an emulator instance: call machine teardown and free all resources.
extern void system_destroy(config_t *config);

extern config_t *global_emulator;

void add_scsi_drive(config_t *restrict config, const char *filename, int scsi_id);

void trigger_vbl(config_t *restrict config);

// Save current machine state to a checkpoint file.
// Returns GS_SUCCESS on success, GS_ERROR on failure.
int system_checkpoint(const char *filename, checkpoint_kind_t kind);

// Restore machine state from a checkpoint file.
// Returns a new config on success, NULL on failure.
config_t *system_restore(const char *filename);

// Command handlers for checkpoint operations
uint64_t cmd_save_checkpoint(int argc, char *argv[]);
uint64_t cmd_load_checkpoint(int argc, char *argv[]);

// Lookup an image by its full filename within the current config's image list
image_t *setup_get_image_by_filename(const char *filename);

// System-level input wrappers (route to appropriate device models)
// Note: system_keyboard_update requires keyboard.h to be included for key_event_t
void system_mouse_update(bool button, int dx, int dy);
bool system_mouse_move(int dx, int dy);
bool system_mouse_move_adb(int dx, int dy);
void system_keyboard_update(key_event_t event, int key);

// Hardware RESET line: calls the machine's reset handler to reinitialize
// peripherals (VIA overlay, MMU, etc.).  Called by the CPU on double bus error
// (HALT → GLU RESET) BEFORE the CPU reads SSP/PC from $0/$4.
// Weak: unit tests that don't link system.c get a no-op stub.
__attribute__((weak)) void system_hardware_reset(void);

// System-level scheduler accessor: returns the current scheduler object
scheduler_t *system_scheduler(void);

// System-level memory accessor: returns the current memory object
memory_map_t *system_memory(void);

// System-level debug accessor: returns the current debugger object
debug_t *system_debug(void);

// System-level CPU accessor: returns the current CPU object
cpu_t *system_cpu(void);

// System-level RTC accessor: returns the current RTC object (or NULL)
rtc_t *system_rtc(void);

// System-level framebuffer accessor: returns pointer to video RAM buffer
uint8_t *system_framebuffer(void);

// Check if emulator is initialized and running
bool system_is_initialized(void);

// Return the model_id of the current machine, or NULL if none is active
const char *system_machine_model_id(void);

// Ensure the correct machine is active for the given model_id.
// Creates a new machine if none exists, or tears down and recreates if the
// current machine's model_id doesn't match.  Returns 0 on success, -1 on error.
int system_ensure_machine(const char *model_id);

// Create a blank floppy image at `path` and auto-mount it. high_density
// chooses 1.44 MB vs 800 KB. preferred is the target drive (0 or 1; pass
// -1 to let the system pick the first free drive). Returns 0 on success
// or -1 on failure (file exists, both drives full, image system error).
int system_create_floppy(const char *path, bool high_density, int preferred);

// Attach a read-only CD-ROM image at the given SCSI id. Used by typed
// `cdrom_attach` and the legacy `cdrom attach` command alike — the
// underlying primitive opens the image, registers it as a SCSI device,
// and emits the legacy "Attaching CD-ROM" stdout message.
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id);

// Probe a floppy image at `path`. Persists volatile (/tmp/, /fd/) paths
// to OPFS first, then opens read-only and prints the detected density.
// Returns 0 if the image is a recognised floppy, non-zero otherwise.
int system_probe_floppy(const char *path);

// Export a SCSI HD image (base + delta) as a single flat file at `dest_path`.
// Returns 0 on success, non-zero on failure.
int system_download_hd(const char *src_path, const char *dest_path);

// argv-driven entry points for the legacy `fd` / `hd` shell commands.
// Used by the typed object-model bridge (fd_insert, hd_create, hd_attach
// wrappers) to bypass shell_dispatch / find_cmd registry lookup.
int shell_fd_argv(int argc, char **argv);
int shell_hd_argv(int argc, char **argv);
int shell_image_argv(int argc, char **argv);

// Pending RAM override for next system_create() call (KB, 0 = use default)
void system_set_pending_ram_kb(uint32_t kb);
uint32_t system_get_pending_ram_kb(void);

// Reset Mac hardware to initial state
extern void mac_reset(config_t *restrict sim);

// Background auto-checkpoint state. WASM platform overrides the weak
// defaults in em_main.c to read/write the live flag; the headless
// build has no auto-checkpoint loop and the defaults stub out.
bool gs_checkpoint_auto_get(void);
void gs_checkpoint_auto_set(bool enabled);

// Platform-specific entry points used by typed root methods.  Each has
// a weak default in system.c that stubs to the headless behaviour;
// em_main.c overrides them on the WASM platform with the real
// browser-driven implementations.
//
//   gs_quit()                — request emulator shutdown.  Headless
//                              sets the quit flag; WASM no-ops.
//   gs_download(path)        — trigger a browser download of a file.
//                              Headless prints a "not supported"
//                              message; WASM streams via blob+anchor.
//   gs_background_checkpoint(reason)
//                            — capture a quick checkpoint.  Headless
//                              prints "not supported"; WASM saves via
//                              save_quick_checkpoint.
//   gs_checkpoint_clear()    — delete all checkpoint files for the
//                              active machine.  Headless prints
//                              "not supported"; WASM nukes the
//                              per-machine checkpoint dir contents.
//   gs_register_machine(id, created)
//                            — register the active machine identity
//                              for checkpoint scoping.  Headless
//                              no-ops; WASM updates the OPFS layout.
// Returns 0 on success, non-zero on failure.
void gs_quit(void);
int gs_download(const char *path);
int gs_background_checkpoint(const char *reason);
int gs_checkpoint_clear(void);
int gs_register_machine(const char *machine_id, const char *created);
// gs_find_media(dir, [dest]) — find the first floppy image in `dir`,
// optionally copy to `dest`.  WASM provides the impl; headless gets
// the weak stub.  Prints the discovered path on success.
int gs_find_media(const char *dir_path, const char *dest);

// True if a valid checkpoint exists for the active machine.  Weak
// default returns NULL (headless has no auto-checkpoint loop); WASM
// overrides with actual scanning of /opfs/checkpoints/.
const char *find_valid_checkpoint_path(void);

#endif // SETUP_H
