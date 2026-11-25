// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8.c
// The V8 family substrate -- see v8.h.  One build for the LC II (V8), the
// Classic II (Eagle) and the Color Classic (Spice): the II-family core, VIA1
// with the Egret companion on its shift register, the Combo's SCC and 5380,
// the sound block (an ASC subset), SWIM, the RBV-style pseudo-VIA2 at
// $50F26000, the Ariel CLUT and the built-in video, and the V8's RAM decode.
//
// RAM decode.  The decoder places two banks in a 10 MB RAM space (Apple,
// *Macintosh LC Computer* Developer Note (1990) p. 11; *Macintosh Color
// Classic* Developer Note (1993) pp. 6, 22): the SIMM bank first, from 0, its
// size programmed into bits 7-6 of the expansion register ($50F26001; 00 = 0,
// 01 = 2, 10 = 4, 11 = 8 MB), then the soldered RAM, decoded as 4 MB (bit 5 =
// 0) or 2 MB (bit 5 = 1).  The top 2 MB of the space, $800000-$9FFFFF, always
// shows the top 2 MB of the soldered RAM -- which is how the sound buffer at
// $9FF000 and the Classic II's screen at $9F9A80 are in RAM on every
// configuration.  The ROM sizes RAM by programming the bits and probing, so
// every write to the register re-decodes the space.  A bank smaller than its
// decode repeats; a decoded range with nothing behind it floats.
//
// The final decode is host-contiguous by construction: host RAM is laid out
// [SIMM bank][soldered RAM], so the PMMU's plain `phys < ram_size` path
// serves it, and the $800000 image is one host region.  On a full 10 MB
// (8 MB of SIMMs over 4 MB soldered) the soldered RAM's bottom 2 MB has no
// address once sized and lives in a side buffer.

#include "v8.h"
#include "v8_internal.h"

#include "config_seed.h"
#include "m68k_board.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h"
#include "slot_tables.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "cpu.h"
#include "cuda.h"
#include "egret.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "mmu.h"
#include "rbv.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

#define MB (1024u * 1024u)

// ============================================================
// I/O island
// ============================================================

#define V8_VIA_IO_PENALTY 16
#define V8_DEV_IO_PENALTY 2

BOARD_IO_ROW_READ(v8_ariel_row_read, v8_ariel_read)
BOARD_IO_ROW_READ(v8_ariel_row_peek, v8_ariel_peek)
BOARD_IO_ROW_WRITE(v8_ariel_row_write, v8_ariel_write)

// The $50F00000 island (LC Developer Note p. 11; Color Classic Developer Note
// Figure 2-3): VIA1, the Combo's SCC and 5380 (handshake, registers and
// pseudo-DMA windows), the sound block, SWIM, the Ariel and the V8 registers.
//   base     end      device            penalty            xform               rd  wr     name
const board_io_range_t v8_io_ranges_tbl[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, V8_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x04000, 0x06000, BOARD_DEV_SCC, V8_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, BOARD_DEV_SCSI, V8_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, BOARD_DEV_SCSI, V8_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, BOARD_DEV_SCSI, V8_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, BOARD_DEV_ASC, V8_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, BOARD_DEV_FLOPPY, V8_DEV_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0x24000, 0x26000, BOARD_DEV_VDAC, V8_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, v8_ariel_row_read, v8_ariel_row_write,
     "ariel", .peek_fn = v8_ariel_row_peek},
    {0x26000, 0x28000, BOARD_DEV_RBV, V8_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "v8"},
    {0}, // sentinel
};

// Bind the devices the island routes to.
static void v8_io_bind(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    board_io_t *io = &st->io;
    m68k_board_io_install(io, cfg, &v8_board(cfg)->desc);
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_SCSI, cfg->scsi, scsi_get_memory_interface(cfg->scsi));
    board_io_bind_dev(io, BOARD_DEV_ASC, st->asc, asc_get_memory_interface(st->asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, cfg->floppy, floppy_get_memory_interface(cfg->floppy));
    board_io_bind_dev(io, BOARD_DEV_RBV, st->rbv, rbv_get_memory_interface(st->rbv));
}

// ============================================================
// RAM decode
// ============================================================

// Host pointer of soldered-RAM offset `off` (0 <= off < pcb_size).
static uint8_t *v8_pcb_at(v8_state_t *st, uint32_t off) {
    if (st->pcb_hidden) {
        // Full 10 MB: the bottom 2 MB is the side buffer, the top follows the SIMMs.
        return off < 2 * MB ? st->pcb_hidden + off : st->pcb_host + (off - 2 * MB);
    }
    return st->pcb_host + off;
}

// Map the 10 MB RAM space as the size bits in `exp` decode it (MMU-off view).
static void v8_map_ram_decode(config_t *cfg, uint8_t exp) {
    v8_state_t *st = v8_st(cfg);
    static const uint32_t bank_a_sizes[4] = {0, 2 * MB, 4 * MB, 8 * MB};
    uint32_t bank_a = bank_a_sizes[(exp >> 6) & 3];
    uint32_t pcb_decode = (exp & 0x20) ? 2 * MB : 4 * MB;
    for (uint32_t a = 0; a < V8_RAM_SPACE; a += MEM_PAGE_SIZE) {
        uint32_t p = a >> MEM_PAGE_SHIFT;
        uint8_t *host = NULL;
        if (a >= V8_RAM_IMAGE) // the top-2 MB image of the soldered RAM
            host = v8_pcb_at(st, st->pcb_size - (V8_RAM_SPACE - V8_RAM_IMAGE) + (a - V8_RAM_IMAGE));
        else if (a < bank_a)
            host = st->simm_size ? st->simm_host + (a % st->simm_size) : NULL;
        else if (a - bank_a < pcb_decode)
            host = v8_pcb_at(st, (a - bank_a) % st->pcb_size);
        if (host)
            page_fill_68k(p, host, true);
        else
            page_clear(p);
    }
    st->ram_exp = exp;
}

// Overlay drop: RAM at zero, as the size bits currently decode, and the
// 32-bit ROM space direct (the trigger aperture is the 24-bit window).
static void v8_map_ram(config_t *cfg) {
    v8_map_ram_decode(cfg, rbv_exp(v8_st(cfg)->rbv));
    uint8_t *rom = ram_native_pointer(cfg->memory_map, cfg->ram_size);
    uint32_t rom_pages = cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    for (uint32_t p = V8_ROM_START >> MEM_PAGE_SHIFT; p < (V8_ROM_END >> MEM_PAGE_SHIFT) && p < g_page_count; p++)
        page_fill_68k(p, rom + (((p - (V8_ROM_START >> MEM_PAGE_SHIFT)) % rom_pages) << MEM_PAGE_SHIFT), false);
}

// V8 expansion-register write: re-decode the RAM space (unless the ROM
// overlay still owns the bottom of it; the drop decodes then).
static void v8_exp_written(void *ctx, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    v8_state_t *st = v8_st(cfg);
    LOG(2, "V8 expansion register = $%02X (pc=%08X)", value, cpu_get_pc(cfg->cpu));
    st->ram_exp = value;
    if (!st->overlay.armed)
        v8_map_ram_decode(cfg, value);
}

// Split the configured total into the SIMM bank and the soldered RAM and lay
// out the host buffer (see the file header).
static int v8_ram_layout(config_t *cfg) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = v8_st(cfg);
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    uint32_t total = cfg->ram_size;
    st->pcb_size = b->pcb_ram;
    if (total >= V8_RAM_SPACE && b->pcb_ram + 8 * MB > V8_RAM_SPACE) {
        // 8 MB of SIMMs over more soldered RAM than the space has room for.
        st->simm_size = 8 * MB;
        st->pcb_hidden = calloc(1, b->pcb_ram - (V8_RAM_SPACE - 8 * MB));
        if (!st->pcb_hidden)
            return -1;
    } else {
        st->simm_size = total > b->pcb_ram ? total - b->pcb_ram : 0;
    }
    st->simm_host = ram;
    st->pcb_host = ram + st->simm_size;
    LOG(1, "%s RAM: %u KB SIMMs + %u KB soldered", b->chip, st->simm_size / 1024, st->pcb_size / 1024);
    return 0;
}

// ============================================================
// Memory layout
// ============================================================

static void v8_memory_layout_init(config_t *cfg) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = v8_st(cfg);

    // The I/O island, once.  It does NOT repeat at $50F40000: the VRAM window
    // is there (empty on the Eagle), and the ROM tells a VISA-lineage decoder
    // from the MDU by exactly that -- VIA1's IER must not reappear $40000 up.
    board_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, V8_ISLAND_BASE, V8_ISLAND_SIZE, "I/O", &st->io_interface, &st->io);

    // VRAM pages direct over the island's window, repeating through the
    // 512 KB decode (the ROM sizes the VRAM by that wrap).
    if (b->fb_source == V8_FB_VRAM && st->video.vram) {
        for (uint32_t off = 0; off < V8_VRAM_WINDOW; off += MEM_PAGE_SIZE)
            page_fill_68k((V8_VRAM_BASE + off) >> MEM_PAGE_SHIFT, st->video.vram + (off % b->vram_size), true);
        memory_map_host_region(cfg->memory_map, "v8_vram", st->video.vram, V8_VRAM_BASE, b->vram_size, true);
        if (b->vram_size < V8_VRAM_WINDOW)
            memory_map_host_region_alias(cfg->memory_map, V8_VRAM_BASE + b->vram_size, V8_VRAM_BASE);
    }

    // The final RAM decode's top-2 MB image, for the PMMU's physical resolver.
    if (cfg->ram_size <= V8_RAM_IMAGE)
        memory_map_host_region(cfg->memory_map, "v8_ram_image",
                               v8_pcb_at(st, st->pcb_size - (V8_RAM_SPACE - V8_RAM_IMAGE)), V8_RAM_IMAGE,
                               V8_RAM_SPACE - V8_RAM_IMAGE, true);

    // The low 16 MB repeats the 24-bit map in either addressing mode (Classic
    // II Developer Note p. 7: "the same address can be used in either 24-bit
    // or 32-bit modes"): ROM at $A00000-$DFFFFF, the I/O space at $F00000.
    memory_map_add(cfg->memory_map, V8_LOW_IO_BASE, V8_ISLAND_SIZE, "I/O (24-bit)", &st->io_interface, &st->io);
    if (b->fb_source == V8_FB_VRAM && st->video.vram)
        for (uint32_t off = 0; off < V8_VRAM_WINDOW; off += MEM_PAGE_SIZE)
            page_fill_68k((V8_LOW_IO_BASE + (V8_VRAM_BASE & 0xFFFFFu) + off) >> MEM_PAGE_SHIFT,
                          st->video.vram + (off % b->vram_size), true);

    // ...for the PMMU too: the ROM's own tables map ROMBase onto the 24-bit
    // physical window, and the VRAM is reached at $F40000 as well.
    uint8_t *rom = ram_native_pointer(cfg->memory_map, cfg->ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    memory_map_host_region(cfg->memory_map, "v8_rom_24", rom, V8_LOW_ROM_START, rom_size, false);
    for (uint32_t a = V8_LOW_ROM_START + rom_size; a < V8_LOW_ROM_END; a += rom_size)
        memory_map_host_region_alias(cfg->memory_map, a, V8_LOW_ROM_START);
    if (b->fb_source == V8_FB_VRAM && st->video.vram) {
        uint32_t low_vram = V8_LOW_IO_BASE + (V8_VRAM_BASE & 0xFFFFFu);
        memory_map_host_region(cfg->memory_map, "v8_vram_24", st->video.vram, low_vram, b->vram_size, true);
        if (b->vram_size < V8_VRAM_WINDOW)
            memory_map_host_region_alias(cfg->memory_map, low_vram + b->vram_size, low_vram);
    }

    // The overlay: ROM at zero until the first access to the ROM space, in
    // either map.  Its trigger aperture is the 24-bit ROM window; the 32-bit
    // ROM space routes through the same trigger until the drop.
    m68k_board_overlay_init(&st->overlay, cfg, V8_LOW_ROM_START, V8_LOW_ROM_END, v8_map_ram, b->chip);
    memory_map_add(cfg->memory_map, V8_LOW_ROM_START, V8_LOW_ROM_END - V8_LOW_ROM_START, "ROM aperture (24-bit)",
                   &st->overlay.iface, &st->overlay);
    memory_map_add(cfg->memory_map, b->desc.rom_base, b->desc.rom_end - b->desc.rom_base, "ROM aperture",
                   &st->overlay.iface, &st->overlay);
    m68k_board_overlay_arm(&st->overlay);
}

// ============================================================
// VIA1 / RBV / Egret wiring
// ============================================================

// VIA1 outputs: port A PA5 is the floppy head select; port B carries the
// Egret handshake (PB3 xcvrSes in, PB4 viaFull and PB5 sysSes out).
static void v8_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    if (port == 0) {
        if (cfg->floppy)
            floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
        return;
    }
    v8_state_t *st = v8_st(cfg);
    if (st->cuda)
        cuda_via1_port_output(st->cuda, port, output);
    else
        egret_via1_port_output(st->egret, port, output);
}

// VIA1 shift-out: a byte for Egret.
static void v8_via1_shift_out(void *context, uint8_t byte) {
    v8_state_t *st = v8_st((config_t *)context);
    if (st->cuda)
        cuda_via1_shift_input(st->cuda, byte);
    else
        egret_via1_shift_input(st->egret, byte);
}

// The pseudo-VIA2's combined interrupt -> IPL 2.
static void v8_rbv_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, V8_IRQ_RBV, active);
}

// MonP depth field -> the built-in video.
static void v8_rbv_mode(void *context, int depth_code) {
    v8_video_set_depth((config_t *)context, depth_code);
}

// The 5380's IRQ/DRQ through the pseudo-VIA2 flags.
static void v8_scsi_irq(void *context, bool irq, bool drq) {
    v8_state_t *st = v8_st((config_t *)context);
    if (!st->rbv)
        return;
    rbv_set_scsi_irq(st->rbv, irq);
    rbv_set_scsi_drq(st->rbv, drq);
}

// The sound block's interrupt -> the pseudo-VIA2 sound flag.
static void v8_asc_irq(void *context, bool active) {
    rbv_set_snd_irq((rbv_t *)context, active);
}

// Soft power-off (Egret's PwrDown): stop the scheduler.
static void v8_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "%s soft power-off", cfg->machine->name);
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

// The RBV register file's part save is the MDU family's (mdu.c).
void part_save_rbv(void *obj, checkpoint_t *cp);

// The family's own checkpoint block: overlay state, the decoded size bits
// and the video.
static void part_save_v8(void *obj, checkpoint_t *cp) {
    config_t *cfg = (config_t *)obj;
    v8_state_t *st = v8_st(cfg);
    uint8_t armed = st->overlay.armed ? 1 : 0;
    system_write_checkpoint_data(cp, &armed, 1);
    system_write_checkpoint_data(cp, &st->ram_exp, 1);
    if (st->pcb_hidden)
        system_write_checkpoint_data(cp, st->pcb_hidden, st->pcb_size - (V8_RAM_SPACE - 8 * MB));
    v8_video_checkpoint(cfg, cp);
}

// ============================================================
// Init / teardown
// ============================================================

static int v8_init(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    m68k_board_build_core(cfg, &b->desc, cp);
    machine_part_irq(cfg, cp);
    m68k_board_build_lowspeed(cfg, cp, NULL);

    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_freq_factor_for_clock(cfg->machine->freq), "via1", v8_via1_output,
                         v8_via1_shift_out, m68k_board_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // Machine-ID strap on PA6/PA4/PA2/PA1; CA1/CB1 idle high.
    static const uint8_t id_pins[] = {6, 4, 2, 1};
    for (size_t i = 0; i < sizeof id_pins; i++)
        via_input(cfg->via1, 0, id_pins[i], (b->id_strap >> id_pins[i]) & 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    if (v8_ram_layout(cfg) != 0)
        return -1;

    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    machine_part_images(cfg, cp);

    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    scsi_5380_attach(cfg->scsi, cp); // the Combo's 53C80
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    scsi_set_irq_callback(cfg->scsi, v8_scsi_irq, cfg);

    machine_part_begin(cfg, cp, "asc");
    st->asc = asc_init(NULL, cfg->scheduler, cp);
    if (!st->asc)
        return -1;
    asc_set_version(st->asc, b->asc_version);
    asc_set_fifo_only(st->asc); // no wavetable, always draining
    machine_part(cfg, cp, "asc", part_save_asc, st->asc);
    asc_set_mix(st->asc, ASC_MIX_CH_A); // mono: the left channel only

    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);

    if (b->cuda) {
        machine_part_begin(cfg, cp, "cuda");
        st->cuda = cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/false);
        if (!st->cuda)
            return -1;
        machine_part(cfg, cp, "cuda", part_save_cuda, st->cuda);
    } else {
        machine_part_begin(cfg, cp, "egret");
        st->egret = egret_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp);
        if (!st->egret)
            return -1;
        machine_part(cfg, cp, "egret", part_save_egret, st->egret);
        egret_set_power_off_callback(st->egret, v8_power_off, cfg);
    }

    // The pseudo-VIA2, its MonP sense lines strapped with the monitor.
    if (v8_video_init(cfg, cfg->build_opts.builtin_sense) != 0) {
        LOG(0, "Error: out of memory constructing the %s video", b->chip);
        return -1;
    }
    machine_part_begin(cfg, cp, "rbv");
    st->rbv = rbv_init(RBV_VARIANT_V8, v8_video_sense(cfg), cp);
    if (!st->rbv)
        return -1;
    machine_part(cfg, cp, "rbv", part_save_rbv, st->rbv);
    rbv_set_irq_callback(st->rbv, v8_rbv_irq, cfg);
    rbv_set_mode_callback(st->rbv, v8_rbv_mode, cfg);
    rbv_set_exp_callback(st->rbv, v8_exp_written, cfg);
    asc_set_irq_handler(st->asc, v8_asc_irq, st->rbv);

    st->mmu = m68k_board_build_mmu(cfg, b->desc.rom_base, b->desc.rom_end);
    if (!st->mmu)
        return -1;
    // The decoder answers the whole low 16 MB (RAM, the 24-bit ROM and I/O
    // windows): nothing there times out.  A write the ROM's tables send to the
    // ROM through $A00000 completes and is dropped, as on the hardware.
    st->mmu->ram_size_max = V8_LOW_IO_BASE + V8_LOW_IO_SIZE;

    v8_io_bind(cfg);
    v8_memory_layout_init(cfg);

    machine_part_begin(cfg, cp, "v8");
    if (cp) {
        uint8_t armed = 1;
        system_read_checkpoint_data(cp, &armed, 1);
        system_read_checkpoint_data(cp, &st->ram_exp, 1);
        if (st->pcb_hidden)
            system_read_checkpoint_data(cp, st->pcb_hidden, st->pcb_size - (V8_RAM_SPACE - 8 * MB));
        v8_video_restore(cfg, cp);
        if (!armed)
            m68k_board_overlay_drop(&st->overlay);
    }
    machine_part(cfg, cp, "v8", part_save_v8, cfg);

    machine_part_pmmu(cfg, cp, st->mmu, cfg->via1, NULL);
    m68k_board_finish(cfg, cp, &st->io);
    return 0;
}

// The board's /RESET net: the overlay re-arms, the shared devices, the
// pseudo-VIA2 (whose expansion register clears, so the RAM decode follows).
static void v8_bus_reset(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    m68k_board_bus_reset(cfg, &st->overlay);
    // The re-armed overlay drops on the next ROM access in EITHER map: the
    // ROM's restart path runs on in the 32-bit ROM space after its RESET.
    for (uint32_t p = V8_ROM_START >> MEM_PAGE_SHIFT; p < (V8_ROM_END >> MEM_PAGE_SHIFT) && p < g_page_count; p++)
        page_route_device(p, &st->overlay.iface, &st->overlay, V8_ROM_START);
    rbv_reset(st->rbv);
    st->ram_exp = 0;
}

// A power cycle's power-on-only half: the sound block's power-on clear.
static void v8_power_on(config_t *cfg) {
    asc_power_on(v8_st(cfg)->asc);
}

static void v8_teardown(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    if (st) {
        if (st->egret)
            egret_delete(st->egret);
        if (st->cuda)
            cuda_delete(st->cuda);
        st->cuda = NULL;
        if (st->rbv)
            rbv_delete(st->rbv);
        if (st->mmu)
            mmu_delete(st->mmu);
        if (st->asc)
            asc_delete(st->asc);
        st->egret = NULL;
        st->rbv = NULL;
        st->mmu = NULL;
        st->asc = NULL;
        v8_video_teardown(cfg);
        free(st->pcb_hidden);
        st->pcb_hidden = NULL;
    }
    machine_teardown_config_devices(cfg);
}

// VBL: the 60.15 Hz tick on VIA1 CA1 and the built-in video's frame
// interrupt (slot 0, bit 6 of the slot-interrupt register).
static void v8_trigger_vbl(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    via_pulse_c(cfg->via1, 0, 0);
    if (st->rbv && v8_video_display(cfg))
        rbv_assert_slot_irq(st->rbv, 0);
    v8_video_vbl(cfg);
    image_tick_all(cfg);
}

// substrate.display: the built-in video, which is no slot device.
static display_t *v8_display(config_t *cfg) {
    return v8_video_display(cfg);
}

const machine_substrate_t v8_substrate = {
    .init = v8_init,
    .bus_reset = v8_bus_reset,
    .power_on = v8_power_on,
    .teardown = v8_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = v8_trigger_vbl,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = v8_display,
};
