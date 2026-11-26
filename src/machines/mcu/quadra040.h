// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// quadra040.h
// The device block and board plumbing every 68040 Quadra substrate built
// around the Quadra 700's parts shares: the MCU family (Quadra 700/900/950,
// mcu.h) and the djMEMC family (Centris/Quadra 610/650/800 and the MEMCjr
// boards, djmemc/djmemc.h).  Both carry the same 53C96, SONIC and MAC
// PROM, EASC, SWIM, DAFB-compatible video and YANCC-compatible NuBus
// controller on the same 256 KiB I/O island, the same ROM-at-zero trip-wire
// and the same /SLOTIRQ aggregate on VIA2 port A; what defines each family
// is its memory controller.
//
// Each family's machine state embeds a quadra040_t as its FIRST member, so
// quadra040_st() reaches it from cfg->machine_context whichever family is
// running, and the builders, island handler rows and IRQ callbacks below
// serve both.

#ifndef GS_MACHINES_MCU_QUADRA040_H
#define GS_MACHINES_MCU_QUADRA040_H

#include "checkpoint.h"
#include "m68k_board.h"
#include "m68k_board_io.h"
#include "m68k_board_overlay.h"
#include "memory.h"
#include "system_internal.h"
#include "yancc.h"

#include <stdbool.h>
#include <stdint.h>

struct asc;
struct dafb;
struct mmu_state;
struct scsi_53c96;
struct sonic;

// The .common block of a 68040 Quadra board: the 256 MB ROM window at
// $40000000 (the 1 MB ROM mirrors through it), the 256 KiB I/O island whose
// undecoded reads float high (see m68k_board.h), and slots $1-$8 and the
// standard slot space below the built-in video bus-erroring.  Only the
// chipset name and the island's decode table differ between boards.
#define M68K040_BOARD_COMMON(chipset_, io_ranges_)                                                                     \
    {                                                                                                                  \
        .chipset = (chipset_),                                                                                         \
        .rom_base = 0x40000000u,                                                                                       \
        .rom_end = 0x50000000u,                                                                                        \
        .io_ranges = (io_ranges_),                                                                                     \
        .io_mirror_mask = 0x0003FFFFu,                                                                                 \
        .io_unmapped_read = 0xFF,                                                                                      \
        .bus_err_lo = 0xF1000000u,                                                                                     \
        .bus_err_hi = NUBUS_BERR_HI,                                                                                   \
    }

// The shared device block: the first member of mcu_state_t and
// djmemc_state_t.
typedef struct quadra040 {
    const m68k_board_desc_t *common; // the board's ROM window and island table
    struct asc *asc; // EASC (ASC-compatible core); the adb and floppy are cfg->'s
    struct dafb *dafb; // DAFB(-compatible) built-in video
    struct scsi_53c96 *scsi96; // the (first) NCR 53C96
    struct sonic *sonic; // DP83932 SONIC Ethernet (no wire); NULL on a board without one
    uint8_t sonic_byte2; // high byte latched from an in-flight SONIC register write
    const uint8_t *mac_prom; // the Ethernet MAC-address PROM's eight bytes
    // /SLOTIRQ aggregate: bit n set = the VIA2 PA n source is asserting
    // (Ethernet 0, NuBus $A-$E 1-5, built-in video 6).  CA1 follows the OR.
    uint8_t slot_pa_mask;
    yancc_t yancc; // the YANCC / Kiwi NuBus controller register file ($28000)
    m68k_board_overlay_t overlay; // ROM-at-zero until the aperture is touched
    struct mmu_state *bus_mmu; // bus-side resolver; the 040 walker's registers live on the CPU
    board_io_t io; // device handles for the shared I/O engine
    memory_interface_t io_interface; // registered at the $50000000 island
} quadra040_t;

// The shared block of the running 68040 Quadra (cfg->machine_context).
static inline quadra040_t *quadra040_st(config_t *cfg) {
    return (quadra040_t *)cfg->machine_context;
}

// What a board's built-in video is (quadra040_build_dafb).
typedef struct quadra040_video {
    uint32_t vram_size; // VRAM fitted into the fixed 2 MiB aperture
    uint8_t version; // DAFB_Test bits 11:9 (3 on the Quadra 950's DAFB 3)
    bool ac842a; // AC842a-compatible RAMDAC (PCBR1, x555 16-bit mode)
    bool antelope; // the Antelope CLUT/DAC: PCBR1's mfg/rev nibble reads non-zero
    bool turbo_scsi; // TurboSCSI control readback carries the 53C96 DRQ (the MCU boards)
} quadra040_video_t;

// --- Construction ------------------------------------------------------------

// The construction prefix: the shared 68k core (memory map, CPU, scheduler),
// the IRQ part, the low-speed spine (RTC, SCC, AppleTalk; `scc_irq` NULL for
// the family default) and the two VIAs on exact-rational clocks, with the
// board's VIA hooks.  Records `common` in the shared block.
void quadra040_build_core(config_t *cfg, checkpoint_t *cp, const m68k_board_desc_t *common,
                          void (*scc_irq)(void *, bool), void (*via1_output)(void *, uint8_t, uint8_t),
                          void (*via1_shift_out)(void *, uint8_t), void (*via2_output)(void *, uint8_t, uint8_t));

// The construction suffix: NuBus (the declared slot cards), their host
// regions projected into the page table, and m68k_board_finish.
void quadra040_build_finish(config_t *cfg, checkpoint_t *cp);

// Idle levels of the lines every board parks the same way: VIA1 port A to
// `via1_pa` (the identity straps), VIA1 CA1/CB1 high, VIA2 port A (the
// active-low slot requests) and its CA1 (/SLOTIRQ) and CB1 (sound) high.
void quadra040_idle_lines(config_t *cfg, uint8_t via1_pa);

// The internal SCSI bus (part "scsi", with the configured disks and CD) and
// the 53C96 fronting it (part "scsi96"), its interrupt on `irq`.
void quadra040_build_scsi0(config_t *cfg, checkpoint_t *cp, void (*irq)(void *ctx, bool active));

// The SONIC Ethernet (part "sonic"; no wire, bus-master DMA through the
// guest-physical port) -- none when `sonic_irq` is NULL -- then the EASC
// (part "asc", left channel to the speaker) and the SWIM (part "floppy").
void quadra040_build_sonic_asc_floppy(config_t *cfg, checkpoint_t *cp, void (*sonic_irq)(void *ctx, bool active),
                                      void (*asc_irq)(void *ctx, bool active));

// The built-in video and its monitor strap (parts "dafb.monitor", "dafb"),
// its interrupt on VIA2 PA6.  `scsi96_ext` is the towers' second 53C96 for
// TurboSCSI channel 1 (NULL elsewhere).  -1 when out of memory.
int quadra040_build_dafb(config_t *cfg, checkpoint_t *cp, const quadra040_video_t *video,
                         struct scsi_53c96 *scsi96_ext);

// The bus-side physical resolver for the 040 walker: RAM at 0 through the
// full 1 GiB aperture, the ROM mirroring through its window.  -1 when out
// of memory.
int quadra040_build_bus_mmu(config_t *cfg);

// Install the board's island table and bind the device set every board
// shares: VIA1, VIA2, SCC, EASC, SWIM and the 53C96.
void quadra040_io_bind(config_t *cfg);

// Register the I/O island at $50000000 (repeating through $53FFFFFF).
void quadra040_map_island(config_t *cfg);

// Register the video: the controller's registers at $F9800000 as `regs_name`
// through `regs_iface`/`regs_ctx` (NULL: the DAFB's own face), and the VRAM
// at $F9000000 repeating through the 2 MB aperture, for the CPU's pages and
// the bus resolver alike.
void quadra040_map_video(config_t *cfg, const char *regs_name, const memory_interface_t *regs_iface, void *regs_ctx);

// Register the ROM aperture's overlay trip-wire and arm it; `map_ram` puts
// RAM at zero when it drops, `name` is the log prefix.
void quadra040_map_overlay(config_t *cfg, void (*map_ram)(config_t *cfg), const char *name);

// --- Lifecycle ---------------------------------------------------------------

// The shared half of the board's /RESET: the overlay re-arms, the video
// resets when `reset_video` (the MCU's DAFB is on the net, the djMEMC's is
// not), the bus resolver goes back to off, the 53C96 resets.  The caller
// adds its own parts, then system_reset_common_devices.
void quadra040_bus_reset(config_t *cfg, bool reset_video);

// Delete the shared devices (SONIC, 53C96, video, bus resolver, EASC).
void quadra040_teardown(quadra040_t *q);

// Re-drive what a restored checkpoint derives: the /SLOTIRQ lines, a pending
// NuBus-controller error request, the bus resolver's TLB and VIA1's outputs.
void quadra040_restore_lines(config_t *cfg);

// substrate.trigger_vbl: VIA1 CA1 (the 60.15 Hz chain), the NuBus cards'
// VBL, the image tick.  The video interrupt is the DAFB's own timing.
void quadra040_trigger_vbl(config_t *cfg);

// substrate.display: the built-in video's scanout.
struct display *quadra040_display(config_t *cfg);

// --- Interrupt wiring -----------------------------------------------------------

// Drive one /SLOTIRQ source (VIA2 PA bit 0-6, `active` in source polarity):
// sets the active-low PA input and re-resolves the CA1 aggregate.
void quadra040_slot_irq_source(config_t *cfg, int pa_bit, bool active);

// substrate.nubus_slot_irq: a card's /NMRQ lands on VIA2 PA(slot - 9)
// (slot $A -> PA1 .. $E -> PA5).
void quadra040_nubus_slot_irq(config_t *cfg, int slot, bool active);

// VIA1 outputs of the boards with the classic RTC and the VIA ADB
// transceiver: PA5 floppy head select; PB the RTC serial (bits 0-2) and
// the ADB state lines (bits 4-5).
void quadra040_via1_output(void *context, uint8_t port, uint8_t output);

// VIA2 outputs: nothing the model consumes (DFAC selects, soft power).
void quadra040_via2_output(void *context, uint8_t port, uint8_t output);

// EASC interrupt -> VIA2 CB1 (active low).
void quadra040_asc_irq(void *context, bool active);

// 53C96 INT -> VIA2 CB2 (active low).
void quadra040_scsi96_irq(void *context, bool active);

// SONIC INT -> VIA2 PA0 through the /SLOTIRQ aggregate.
void quadra040_sonic_irq(void *context, bool active);

// The NuBus controller's error request -> the level-7 (NMI) source.
void quadra040_yancc_irq(void *context, bool active);

// --- I/O island handler rows -------------------------------------------------

// The Ethernet MAC-address PROM ($08000): the eight bytes repeating.
uint8_t quadra040_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void quadra040_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// The SONIC ($0A000): 16-bit registers on 4-byte spacing.
uint8_t quadra040_sonic_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void quadra040_sonic_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// The YANCC / Kiwi register file ($28000).
uint8_t quadra040_yancc_read(config_t *cfg, uint32_t win_off, uint32_t addr);
uint8_t quadra040_yancc_peek(config_t *cfg, uint32_t win_off, uint32_t addr);
void quadra040_yancc_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// --- Built-in video as a display device ------------------------------------------

// The DAFB's monitors, by strap (dafb.h's indexed numbering): every one a
// ROM was measured with (the passive codes plus the 16" on the extended
// probe), or the passive codes only.
bool quadra040_monitor_at(size_t i, const char **id, const char **monitor);
bool quadra040_monitor_passive_at(size_t i, const char **id, const char **monitor);
// The strap a monitor id names.
bool quadra040_monitor_sense(const char *id, uint8_t *out);

#endif // GS_MACHINES_MCU_QUADRA040_H
