// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// plus.c
// Macintosh Plus machine implementation.
//
// This file owns the full lifecycle of a Plus emulator instance:
// init, teardown, checkpoint save/restore, VIA/SCC interrupt callbacks,
// VBL trigger, and the static hw_profile_t descriptor.

#include "machine.h"
#include "system_config.h" // full config_t definition

#include "appletalk.h"
#include "cpu.h"
#include "debug.h"
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

LOG_USE_CATEGORY_NAME("plus");

// Plus-specific peripheral state not shared with other machines.
// Accessed through config_t.machine_context.
typedef struct plus_state {
    sound_t *sound; // PWM sound (VIA-driven)
} plus_state_t;

// Helper: return the Plus-specific state from a config handle
static inline plus_state_t *plus_state(config_t *cfg) {
    return (plus_state_t *)cfg->machine_context;
}

// ============================================================
// Forward declarations for Plus callbacks
// ============================================================

static void plus_via_output(void *context, uint8_t port, uint8_t output);
static void plus_via_shift_out(void *context, uint8_t byte);
static void plus_via_irq(void *context, bool active);
static void plus_scc_irq(void *context, bool active);
static void plus_update_ipl(config_t *sim, int level, bool value);

// ============================================================
// Video buffer helper (Plus-specific address constants)
// ============================================================

// Switch between main and alternate video buffer addresses for the Plus.
// Main buffer is at top of RAM minus 0x5900; alternate is 0x8000 bytes lower.
static void plus_use_video_buffer(config_t *cfg, bool main) {
    uint32_t addr = main ? (0x400000 - 0x5900) : (0x400000 - 0x5900 - 0x8000);
    cfg->ram_vbuf = ram_native_pointer(cfg->mem_map, addr);
}

// ============================================================
// Init / Teardown
// ============================================================

// Initialise all Plus subsystems.
// If checkpoint is non-NULL, each device restores state from it (same order as checkpoint_save).
static void plus_init(config_t *cfg, checkpoint_t *checkpoint) {
    // Allocate Plus-specific peripheral state
    plus_state_t *ps = malloc(sizeof(plus_state_t));
    assert(ps != NULL);
    memset(ps, 0, sizeof(plus_state_t));
    cfg->machine_context = ps;

    cfg->mem_map = memory_map_init(checkpoint);
    cfg->mem_iface = memory_map_interface(cfg->mem_map);

    cfg->cpu = cpu_init(checkpoint);

    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);

    // Restore global interrupt state after scheduler (same order as checkpoint_save)
    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));
    }

    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);

    cfg->scc = scc_init(cfg->mem_map, cfg->scheduler, plus_scc_irq, cfg, checkpoint);

    // Initialise AppleTalk with scheduler and SCC dependencies (registers shell commands)
    appletalk_init(cfg->scheduler, cfg->scc, NULL);

    ps->sound = sound_init(cfg->mem_map, checkpoint);

    cfg->via1 =
        via_init(cfg->mem_map, cfg->scheduler, plus_via_output, plus_via_shift_out, plus_via_irq, cfg, checkpoint);

    rtc_set_via(cfg->rtc, cfg->via1);

    cfg->mouse = mouse_init(cfg->scheduler, cfg->scc, cfg->via1, checkpoint);

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
                // For consolidated checkpoints the embedded data is authoritative
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
                add_image(cfg, img);
            }
            if (name) {
                free(name);
            }
        }
    }

    cfg->scsi = scsi_init(cfg->mem_map, checkpoint);

    setup_images(cfg);

    cfg->keyboard = keyboard_init(cfg->scheduler, cfg->scc, cfg->via1, checkpoint);

    // Initialise floppy last to match checkpoint save order
    cfg->floppy = floppy_init(cfg->mem_map, cfg->scheduler, checkpoint);

    // After floppy exists, if restoring from a checkpoint, re-drive VIA outputs
    // so SEL and other external signals propagate to the floppy.
    if (checkpoint) {
        via_redrive_outputs(cfg->via1);
    }

    // On cold boot, default to main video buffer.
    // When restoring from a checkpoint, VIA outputs will re-drive the selection.
    if (!checkpoint) {
        plus_use_video_buffer(cfg, true);
    }

    cfg->debugger = debug_init();

    scheduler_start(cfg->scheduler);

    // Initialise IRQ/IPL only for cold boot; on restore, devices already re-assert.
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

// Tear down all Plus resources in reverse init order.
static void plus_teardown(config_t *cfg) {
    // Stop scheduler if running (best effort)
    if (cfg->scheduler) {
        scheduler_stop(cfg->scheduler);
    }

    if (cfg->keyboard) {
        keyboard_delete(cfg->keyboard);
        cfg->keyboard = NULL;
    }
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (cfg->mouse) {
        mouse_delete(cfg->mouse);
        cfg->mouse = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }

    plus_state_t *ps = plus_state(cfg);
    if (ps && ps->sound) {
        sound_delete(ps->sound);
        ps->sound = NULL;
    }

    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->floppy) {
        floppy_delete(cfg->floppy);
        cfg->floppy = NULL;
    }
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }

    // Free machine-specific state; images are freed by system_destroy()
    if (ps) {
        free(ps);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

// Save complete Plus machine state to an open checkpoint stream.
// Order must match the restore path in plus_init().
static void plus_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);

    // Save global interrupt state (irq) after scheduler/cpu
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);

    plus_state_t *ps = plus_state(cfg);
    sound_checkpoint(ps ? ps->sound : NULL, cp);

    via_checkpoint(cfg->via1, cp);
    mouse_checkpoint(cfg->mouse, cp);

    // Checkpoint list of images (path + writable) before devices that reference them
    {
        uint32_t count = (uint32_t)cfg->n_images;
        system_write_checkpoint_data(cp, &count, sizeof(count));
        for (uint32_t i = 0; i < count; ++i) {
            image_checkpoint(cfg->images[i], cp);
        }
    }

    scsi_checkpoint(cfg->scsi, cp);
    keyboard_checkpoint(cfg->keyboard, cp);
    floppy_checkpoint(cfg->floppy, cp);
}

// ============================================================
// VIA / SCC callbacks
// ============================================================

// Plus-specific interrupt routing: update CPU IPL from VIA or SCC IRQ changes
static void plus_update_ipl(config_t *sim, int level, bool value) {
    int old_irq = sim->irq;
    int old_ipl = cpu_get_ipl(sim->cpu);
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
    cpu_set_ipl(sim->cpu, new_ipl);

    LOG(1, "plus_update_ipl: level=%d value=%d irq:%d->%d ipl:%d->%d", level, value ? 1 : 0, old_irq, sim->irq, old_ipl,
        new_ipl);

    cpu_reschedule();
}

// Plus-specific VIA output callback: routes port changes to floppy, video, sound, RTC
static void plus_via_output(void *context, uint8_t port, uint8_t output) {
    config_t *sim = (config_t *)context;
    plus_state_t *ps = plus_state(sim);

    if (port == 0) {
        floppy_set_sel_signal(sim->floppy, (output & 0x20) != 0);

        plus_use_video_buffer(sim, (output >> 6) & 1);

        sound_use_buffer(ps->sound, (output >> 3) & 1);

        sound_volume(ps->sound, output & 7);
    } else {
        rtc_input(sim->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);

        sound_enable(ps->sound, (output & 0x80) == 0);
    }
}

// Plus-specific VIA shift-out callback: routes keyboard data to keyboard device
static void plus_via_shift_out(void *context, uint8_t byte) {
    config_t *sim = (config_t *)context;
    keyboard_input(sim->keyboard, byte);
}

// Plus-specific VIA IRQ callback: VIA uses IPL level 1
static void plus_via_irq(void *context, bool active) {
    plus_update_ipl((config_t *)context, 1, active);
}

// Plus-specific SCC IRQ callback: SCC uses IPL level 2
static void plus_scc_irq(void *context, bool active) {
    plus_update_ipl((config_t *)context, 2, active);
}

// ============================================================
// VBL trigger
// ============================================================

// Trigger vertical blanking interval for the Plus.
static void plus_trigger_vbl(config_t *cfg) {
    plus_state_t *ps = plus_state(cfg);

    // Assert then deassert VIA CA1 to signal VBL to the CPU
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);

    // Advance the sound DMA phase
    if (ps && ps->sound) {
        sound_vbl(ps->sound);
    }

    image_tick_all(cfg);
}

// ============================================================
// Machine descriptor
// ============================================================

// Macintosh Plus hardware profile descriptor
const hw_profile_t machine_plus = {
    .model_name = "Macintosh Plus",
    .model_id = "plus",

    // 68000 at 7.8336 MHz (actual Plus clock)
    .cpu_model = 68000,
    .cpu_clock_hz = 7833600,
    .mmu_present = false,
    .fpu_present = false,

    // 24-bit address space
    .address_bits = 24,
    .ram_size_default = 0x400000, // 4 MB
    .ram_size_max = 0x400000, // 4 MB max
    .rom_size = 0x020000, // 128 KB

    // Single VIA, no ADB, no NuBus
    .via_count = 1,
    .has_adb = false,
    .has_nubus = false,
    .nubus_slot_count = 0,

    // Lifecycle callbacks wired to Plus-specific implementations
    .init = plus_init,
    .teardown = plus_teardown,
    .checkpoint_save = plus_checkpoint_save,
    .checkpoint_restore = NULL, // restore is handled by plus_init when checkpoint != NULL
    .memory_layout_init = NULL, // Plus memory layout handled inside plus_init
    .update_ipl = NULL, // internal: plus_update_ipl() called directly by VIA/SCC callbacks
    .trigger_vbl = plus_trigger_vbl,
};
