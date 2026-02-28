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
void system_keyboard_update(key_event_t event, int key);

// System-level scheduler accessor: returns the current scheduler object
scheduler_t *system_scheduler(void);

// System-level memory accessor: returns the current memory object
memory_map_t *system_memory(void);

// System-level debug accessor: returns the current debugger object
debug_t *system_debug(void);

// System-level CPU accessor: returns the current CPU object
cpu_t *system_cpu(void);

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

// Reset Mac hardware to initial state
extern void mac_reset(config_t *restrict sim);

#endif // SETUP_H
