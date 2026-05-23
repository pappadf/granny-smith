// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi.h
// Public interface for SCSI controller emulation.

#ifndef SCSI_H
#define SCSI_H

// === Includes ===
#include "common.h"
#include "image.h"
#include "memory.h"
#include "via.h"

// === Type Definitions ===
struct scsi;
typedef struct scsi scsi_t;

// SCSI bus phase — observable state of the chip's bus phase logic.
// External bus masters (e.g. the IIfx SCSI DMA wrapper) inspect this
// to decide whether to transfer bytes into or out of the chip's data
// register.  Values match the SCSI-1 wire encoding for the data-phase
// transitions (data_out=0..status=3 use the chip's IO/CD/MSG bits
// directly); the remaining values are emulator-internal sequencing
// states.
typedef enum scsi_phase {
    scsi_bus_free = 0,
    scsi_arbitration,
    scsi_selection,
    scsi_reselection,
    scsi_command,
    scsi_data_in,
    scsi_data_out,
    scsi_status,
    scsi_message_in,
    scsi_message_out
} scsi_phase_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Mount a pre-machine `scsi` singleton at root carrying just the static
// helpers — hd_models, identify_hd, identify_cdrom — so file-shape
// validation works before any machine has been booted. Called from
// shell_init alongside rom_init. Idempotent.
void scsi_class_register(void);

scsi_t *scsi_init(memory_map_t *map, checkpoint_t *checkpoint);

void scsi_delete(scsi_t *scsi);

void scsi_checkpoint(scsi_t *restrict scsi, checkpoint_t *checkpoint);

// === Device Types ===
enum scsi_device_type;

// === Operations ===

void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, const char *revision,
                     image_t *image, enum scsi_device_type type, uint16_t block_size, bool read_only);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scsi_get_memory_interface(scsi_t *scsi);

// Connect SCSI interrupt outputs (IRQ, DRQ) to VIA2 for SE/30-style machines.
// On machines without VIA2 (e.g. Plus), this is not called and SCSI is polled.
void scsi_set_via(scsi_t *scsi, via_t *via);

// Machine-specific SCSI IRQ/DRQ delivery callback. Used by machines that
// don't route SCSI through VIA2 (e.g. IIfx, whose SCSI interrupts feed
// the OSS interrupt controller via source 9). Called whenever the SCSI
// IRQ or DRQ output changes. Either set this OR scsi_set_via, not both —
// the SE/30 path drives VIA2 CB2/CA2 directly, while machines using the
// callback are expected to do the equivalent routing themselves.
typedef void (*scsi_irq_fn)(void *context, bool irq, bool drq);
void scsi_set_irq_callback(scsi_t *scsi, scsi_irq_fn cb, void *context);

// Push a single byte into the SCSI buffer, bypassing the pseudo-DMA
// primer-slot gate. Used by the IIfx wrapper when the hardware-
// handshake mode (iHSKEN) is active — the wrapper hardware auto-
// handshakes every byte onto the SCSI bus during whatever phase is
// currently active, so the A/UX primer heuristic (which drops a
// leading $00 byte that differs in PC from the next byte) must not
// be applied. Handles both COMMAND-phase CDB bytes and DATA-OUT-phase
// data bytes — A/UX uses the same iHSKEN-armed write loop for both.
void scsi_hsken_data_out_byte(scsi_t *scsi, uint8_t byte);

// ============================================================================
// External bus-master pseudo-DMA helpers
// ============================================================================
//
// Used by machines whose SCSI architecture includes a custom DMA wrapper
// that acts as a bus master between the host's RAM and the NCR 5380's
// data register.  The IIfx SCSI DMA wrapper is one such device: with
// sDADDR, sDCNT, and sDCTRL.iHSKEN programmed plus the chip's MR_DMA
// set, the wrapper transfers bytes between RAM[sDADDR..sDADDR+sDCNT-1]
// and the chip's data register over REQ/ACK without CPU involvement,
// then asserts EOP (which the chip latches as end_of_dma → /IRQ).
//
// The wrapper code in src/machines/<machine>.c drives the transfer.
// These helpers expose just enough of the chip's internal state for an
// external pump function to do its job, modeled after how the real
// 5380's DACK line + bus-side state interact with an external DMA
// controller.

// Pop one byte from the chip's data-in buffer.  Returns true if a byte
// was available and writes it through `out`; false if the chip has no
// byte to deliver (wrong phase, or buffer empty).  Phase stays at
// data_in even when the buffer drains — the chip transitions to STATUS
// lazily, via the existing CSR-read deferred-phase mechanism, when the
// kernel handler reads CSR.  This models real hardware ordering: the
// target keeps the phase asserted until *after* the chip has latched
// EOP and the initiator has acked the IRQ.
bool scsi_pop_data_in_byte(scsi_t *scsi, uint8_t *out);

// Push one byte into the chip's data-out / command buffer.  Mirrors the
// chip's auto-handshake ODR alias semantics (apply_primer_gate=false —
// the bus master does not generate the primer-then-data pattern that
// the gate exists to filter).  When the buffer fills, the chip
// dispatches: run_cmd() if currently in COMMAND phase, command_complete()
// if currently in DATA_OUT phase.
void scsi_push_data_out_byte(scsi_t *scsi, uint8_t byte);

// Signal "end of DMA" from an external bus master.  Equivalent to the
// EOP pin being asserted in real hardware: the chip latches end_of_dma
// and re-evaluates /IRQ.  Idempotent — calling when end_of_dma is
// already set just re-evaluates the IRQ.
void scsi_signal_eop(scsi_t *scsi);

// Query whether MR_DMA is currently set in the chip's mode register.
// Used by bus-master pumps to gate transfers.
bool scsi_get_mr_dma(const scsi_t *scsi);

// Query whether the chip's /IRQ output is currently asserted.  Used by
// machine wrappers (IIfx) that re-evaluate their IRQ-enable gate on
// writes to their own control registers: if /IRQ is level-sensitive
// and the gate transitions from disable→enable, the wrapper should
// latch the still-asserted IRQ now rather than waiting for the next
// chip-side state change.
bool scsi_get_irq_active(const scsi_t *scsi);

// Enable/disable SCSI loopback test card (passive bus terminator).
// When enabled, initiator-driven signals are reflected back through
// status registers, emulating a connected SCSI diagnostic card.
void scsi_set_loopback(scsi_t *scsi, bool enable);

// Query whether SCSI loopback mode is active
bool scsi_get_loopback(scsi_t *scsi);

// Eject the medium at the given SCSI id (0..6). Returns 1 on successful
// eject, 0 if the slot was already empty, -1 on bad arguments.
int scsi_eject_device(scsi_t *scsi, int id);

// === M7d — object-model accessors ==========================================
//
// Read-only views over the SCSI controller and its 8 device slots used
// by the `scsi` / `scsi.bus` / `scsi.devices` object classes. Phase is
// exposed as an integer with the canonical name table living in the
// object class so the proposal's V_ENUM display works without leaking
// the internal phase enum across the public header.
//
// Slot index is 0..7 (the SCSI ID). Reads on an unpopulated slot
// return type=0 (none) / 0 / NULL — callers test `type` first.

// Bus phase as a small integer matching the order:
//   0=bus_free, 1=arbitration, 2=selection, 3=reselection, 4=command,
//   5=data_in, 6=data_out, 7=status, 8=message_in, 9=message_out
int scsi_get_bus_phase(const scsi_t *scsi);
int scsi_get_bus_target(const scsi_t *scsi);
int scsi_get_bus_initiator(const scsi_t *scsi);

// Per-device queries. `which` is the SCSI ID (0..7).
//   type:           0=none, 1=hd, 2=cdrom (matches `enum scsi_device_type`)
//   read_only:      block-write inhibit
//   medium_present: relevant for CD-ROM eject/insert state
//   block_size:     512 for HD, usually 2048 for CD-ROM
//   vendor/product: NULL when slot is empty
int scsi_device_type(const scsi_t *scsi, unsigned which);
bool scsi_device_present(const scsi_t *scsi, unsigned which);
bool scsi_device_read_only(const scsi_t *scsi, unsigned which);
bool scsi_device_medium_present(const scsi_t *scsi, unsigned which);
uint16_t scsi_device_block_size(const scsi_t *scsi, unsigned which);
const char *scsi_device_vendor(const scsi_t *scsi, unsigned which);
const char *scsi_device_product(const scsi_t *scsi, unsigned which);
const char *scsi_device_revision(const scsi_t *scsi, unsigned which);

// Get the image_t* mounted at the given SCSI id, or NULL if the slot
// is empty or the device has no medium loaded.
struct image *scsi_device_image(const scsi_t *scsi, unsigned which);

#endif // SCSI_H
