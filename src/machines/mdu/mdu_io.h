// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.h
// The MDU+RBV family's I/O dispatch table, shared by the IIci and IIsi.  These
// two decode the canonical $50Fxxxxx island identically (VIA1 / SCC / SCSI
// {DRQ,REG,BLIND} / ASC / SWIM / VDAC / RBV, 18-bit $40000 mirror) — the IIsi
// dispatcher was a 0-substantive-diff prefix-rename of the IIci's.
//
// The decode runs on the shared board_io engine (m68k_board_io.h): this file
// just supplies the MDU window table + mirror + device set via mdu_io_bind.
// Unlike GLUE there is no VIA2 window — the RBV ($26000) replaces it — and a
// Bt450 VDAC ($24000) fronts the built-in RBV video card.

#ifndef GS_MACHINES_MDU_IO_H
#define GS_MACHINES_MDU_IO_H

#include "m68k_board_io.h" // the shared engine (board_io_t + board_io_*)
#include "m68k_board_overlay.h"
#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct nubus_card;
struct adb;
struct asc;
struct egret;
struct floppy;
struct rbv;
struct mmu_state;

// The MDU dispatch context is the shared engine's context; this file installs
// the MDU table + mirror + devices into it.  (The IIci/IIsi state struct names
// its field's type mdu_io_t.)
typedef board_io_t mdu_io_t;

struct m68k_board_desc;

// Cache the MDU device interfaces and install the board's window table + mirror
// + unmapped-read value.
void mdu_io_bind(mdu_io_t *io, config_t *cfg, const struct m68k_board_desc *desc, void *asc, void *floppy, void *rbv,
                 struct nubus_card *video_card);

// The MDU family's ordered window table (sentinel-terminated).  Exposed for the
// address-map unit test and for MDU machines' board descriptors.
extern const board_io_range_t mdu_io_ranges_tbl[];
const board_io_range_t *mdu_io_ranges(void);

// The table's rows, for the islands that extend it (the V8 family's Sonora,
// sonora.c, which inserts its own registers between them).  Every window but
// VIA1's charges the same bus turnaround.
#define MDU_VIA_IO_PENALTY 16
#define MDU_DEV_IO_PENALTY 2
//   base     end      device            penalty             xform               rd  wr     name
// VIA1, the SCC, the 5380's pseudo-DMA handshake, registers and blind-transfer
// windows, and the sound block.
#define MDU_IO_ROW_VIA1                                                                                                \
    {0x00000, 0x02000, BOARD_DEV_VIA1, MDU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1}
#define MDU_IO_ROW_SCC {0x04000, 0x06000, BOARD_DEV_SCC, MDU_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"}
#define MDU_IO_ROW_SCSI_DRQ                                                                                            \
    {0x06000, 0x08000, BOARD_DEV_SCSI, MDU_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"}
#define MDU_IO_ROW_SCSI_REG                                                                                            \
    {0x10000, 0x12000, BOARD_DEV_SCSI, MDU_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"}
#define MDU_IO_ROW_SCSI_BLIND                                                                                          \
    {0x12000, 0x14000, BOARD_DEV_SCSI, MDU_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"}
#define MDU_IO_ROW_ASC {0x14000, 0x16000, BOARD_DEV_ASC, MDU_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "asc"}
// SWIM (SWIM2 on Sonora), the colour DAC (Bt450 / Ariel) and the RBV register file.
#define MDU_IO_ROW_SWIM(name)                                                                                          \
    {0x16000, 0x18000, BOARD_DEV_FLOPPY, MDU_DEV_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, (name)}
#define MDU_IO_ROW_VDAC(name)                                                                                          \
    {0x24000, 0x26000, BOARD_DEV_VDAC, MDU_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, (name)}
#define MDU_IO_ROW_RBV(name)                                                                                           \
    {0x26000, 0x28000, BOARD_DEV_RBV, MDU_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, (name)}

// Unified MDU+RBV machine state — the single struct shared by the IIci and
// IIsi (mirrors the GLUE-family unification).  Superset: the IIsi uses egret;
// the IIci leaves egret NULL.
typedef struct mdu_state {
    struct asc *asc; // (the adb and floppy are cfg->adb / cfg->floppy)
    struct egret *egret; // IIsi companion (ADB/RTC/PRAM/power); NULL on IIci
    struct rbv *rbv; // RBV chip (VIA2 replacement + video control)
    struct nubus_card *video_card; // built-in RBV video pseudo-card

    m68k_board_overlay_t overlay; // ROM at $00000000, following VIA1 PA4 (line policy)
    struct mmu_state *mmu;

    mdu_io_t mdu_io; // device handles for the shared MDU dispatcher

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} mdu_state_t;

#endif // GS_MACHINES_MDU_IO_H
