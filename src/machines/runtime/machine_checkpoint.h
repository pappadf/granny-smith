// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_checkpoint.h
// The checkpoint parts every machine family has (machine_parts.h).
//
// A family registers each part right after the constructor that reads its
// block on a restore, so a checkpoint's order is the family's construction
// order by construction.  The save functions below write the devices every
// family shares; a family's own blocks use family-local save functions.

#ifndef GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H
#define GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H

#include "checkpoint.h"
#include "common.h"
#include "machine_parts.h"

struct config;

// Part-save functions: `obj` is the device the part names.
void part_save_memory(void *obj, checkpoint_t *cp); // memory_map_t
void part_save_cpu(void *obj, checkpoint_t *cp); // cpu_t
void part_save_ppc(void *obj, checkpoint_t *cp); // ppc_t
void part_save_scheduler(void *obj, checkpoint_t *cp); // struct scheduler
void part_save_rtc(void *obj, checkpoint_t *cp); // rtc_t
void part_save_scc(void *obj, checkpoint_t *cp); // scc_t
void part_save_atalk(void *obj, checkpoint_t *cp); // atalk_conn_t
void part_save_via(void *obj, checkpoint_t *cp); // via_t
void part_save_adb(void *obj, checkpoint_t *cp); // adb_t
void part_save_scsi(void *obj, checkpoint_t *cp); // scsi_t
void part_save_asc(void *obj, checkpoint_t *cp); // asc_t
void part_save_floppy(void *obj, checkpoint_t *cp); // floppy_t
void part_save_keyboard(void *obj, checkpoint_t *cp); // keyboard_t
void part_save_mouse(void *obj, checkpoint_t *cp); // mouse_t
void part_save_sound(void *obj, checkpoint_t *cp); // sound_t
void part_save_mmu(void *obj, checkpoint_t *cp); // mmu_state_t (the 68030 PMMU)
void part_save_scsi96(void *obj, checkpoint_t *cp); // scsi_53c96_t
void part_save_sonic(void *obj, checkpoint_t *cp); // sonic_t
void part_save_egret(void *obj, checkpoint_t *cp); // egret_t (the IIsi's Egret, the towers' Caboose)
void part_save_iop(void *obj, checkpoint_t *cp); // iop_t
void part_save_cuda(void *obj, checkpoint_t *cp); // av_cuda_t

// The machine's ImageWriter (iw_printer.h): build it, reading its block on a
// restore, and register it.  After the SCC and the AppleTalk connection.
// `lisa`: the Lisa's Serial A ready line is wired by the machine itself and
// it has no AppleTalk; the Macs wire the printer's DTR to HSKi -> /CTS on
// both ports when a printer is plugged in.
void machine_part_imagewriter(struct config *cfg, checkpoint_t *cp, bool lisa);

// cfg->irq, the 68k families' aggregated interrupt-source bitmap: read it
// on a restore, and register it.
void machine_part_irq(struct config *cfg, checkpoint_t *cp);

// The machine's image list (checkpoint_images.h): read it on a restore,
// before the devices that reference it, and register it.
void machine_part_images(struct config *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H
