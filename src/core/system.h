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

#define MAX_IMAGES 10

// Opaque emulator configuration handle
struct config;
typedef struct config config_t;

// Config field accessors (opaque handle access)
image_t *config_get_image(config_t *cfg, int index);
int config_get_n_images(config_t *cfg);
void config_add_image(config_t *cfg, image_t *image);

extern void setup_init(void);
extern config_t *setup_plus(checkpoint_t *checkpoint);

extern void mac_reset(config_t *restrict sim);

extern config_t *global_emulator;

void add_scsi_drive(config_t *restrict config, const char *filename, int scsi_id);

void trigger_vbl(config_t *restrict config);

// Teardown: dispose all modules in reverse init order and free config
void setup_teardown(config_t *config);

// Device checkpoint functions are declared in their respective module headers

// Function to save complete machine state checkpoint
// `kind` selects quick vs consolidated checkpointing behavior.
// Returns GS_SUCCESS on success, GS_ERROR on failure
int setup_plus_checkpoint(const char *filename, checkpoint_kind_t kind);

// Function to load machine state from checkpoint
config_t *setup_plus_restore(const char *filename);

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

#endif // SETUP_H
