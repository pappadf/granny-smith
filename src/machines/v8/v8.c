// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8.c
// The V8 family substrate -- see v8.h.  One build for the LC II (V8), the
// Classic II (Eagle) and the Color Classic (Spice): the II-family core, VIA1
// with the Egret or Cuda companion on its shift register, the Combo's SCC and 5380,
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
#include "mdu_io.h" // mdu_io_ranges_tbl: the island
#include "slot_tables.h"

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
#include "rbv_board.h"
#include "rtc.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

#define MB (1024u * 1024u)

// ============================================================
// I/O island
// ============================================================

// The Ariel as the island's colour-DAC device (the VDAC row), its context cfg.
static uint8_t v8_ariel_dev_read(void *dev, uint32_t off) {
    return v8_ariel_read((config_t *)dev, off);
}
static uint8_t v8_ariel_dev_peek(void *dev, uint32_t off) {
    return v8_ariel_peek((config_t *)dev, off);
}
static void v8_ariel_dev_write(void *dev, uint32_t off, uint8_t value) {
    v8_ariel_write((config_t *)dev, off, value);
}
static const memory_interface_t v8_ariel_iface = {
    .read_uint8 = v8_ariel_dev_read,
    .write_uint8 = v8_ariel_dev_write,
    .peek_uint8 = v8_ariel_dev_peek,
};

// The $50F00000 island (LC Developer Note p. 11; Color Classic Developer Note
// Figure 2-3) is the MDU's, row for row (mdu_io.c): VIA1, the Combo's SCC
// and 5380 (handshake, registers and pseudo-DMA windows), the sound block,
// SWIM, the Ariel at the VDAC row and the V8 registers at the RBV row.

// Bind the devices the island routes to.
static void v8_io_bind(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    board_io_t *io = &st->io;
    m68k_board_io_install(io, cfg, &v8_ops(cfg)->desc);
    io->window_base = V8_ISLAND_BASE; // the island is mapped here, not at the ROM space's end
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_SCSI, cfg->scsi, scsi_get_memory_interface(cfg->scsi));
    board_io_bind_dev(io, BOARD_DEV_ASC, st->asc, asc_get_memory_interface(st->asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, cfg->floppy, floppy_get_memory_interface(cfg->floppy));
    board_io_bind_dev(io, BOARD_DEV_RBV, st->rbv, rbv_get_memory_interface(st->rbv));
    board_io_bind_dev(io, BOARD_DEV_VDAC, cfg, &v8_ariel_iface);
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

// Overlay drop: RAM at zero, as the size bits currently decode (the overlay
// turns both ROM windows direct itself; its alias is the 32-bit ROM space).
static void v8_map_ram(config_t *cfg) {
    v8_map_ram_decode(cfg, rbv_exp(v8_st(cfg)->rbv));
}

// The RAM-decode register written (the V8's expansion register, Sonora's RAM
// configuration register; both the RBV's register 1): re-decode the RAM space
// (unless the ROM overlay still owns the bottom of it; the drop decodes then).
static void v8_ram_reg_written(void *ctx, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    v8_state_t *st = v8_st(cfg);
    LOG(2, "%s RAM decode register = $%02X (pc=%08X)", v8_ops(cfg)->desc.chipset, value, cpu_get_pc(cfg->cpu));
    st->ram_exp = value;
    if (!st->overlay.armed)
        v8_ops(cfg)->map_ram_decode(cfg, value);
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
    LOG(1, "%s RAM: %u KB SIMMs + %u KB soldered", v8_ops(cfg)->desc.chipset, st->simm_size / 1024,
        st->pcb_size / 1024);
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
    // ROM space is its alias, routed through the same trigger on every arm
    // (the ROM's restart path runs on there after its RESET) until the drop.
    m68k_board_overlay_init(&st->overlay, cfg, V8_LOW_ROM_START, V8_LOW_ROM_END, v8_map_ram, v8_ops(cfg)->desc.chipset);
    m68k_board_overlay_set_alias(&st->overlay, V8_ROM_START, V8_ROM_END);
    memory_map_add(cfg->memory_map, V8_LOW_ROM_START, V8_LOW_ROM_END - V8_LOW_ROM_START, "ROM aperture (24-bit)",
                   &st->overlay.iface, &st->overlay);
    memory_map_add(cfg->memory_map, V8_ROM_START, V8_ROM_END - V8_ROM_START, "ROM aperture", &st->overlay.iface,
                   &st->overlay);
    m68k_board_overlay_arm(&st->overlay);
}

// The V8, Eagle and Spice decoders: the ROM space at $40000000 with the
// 24-bit windows below, the I/O island the MDU's, the PDS slot's space empty.
const v8_chip_ops_t v8_chip_ops = {
    .desc =
        {
               .chipset = "V8",
               .rom_base = V8_ROM_START,
               .rom_end = V8_ROM_END,
               .io_ranges = mdu_io_ranges_tbl,
               .io_mirror_mask = V8_IO_MIRROR,
               .io_unmapped_read = 0xFF,
               .bus_err_lo = 0xFE000000u,
               .bus_err_hi = 0xFEFFFFFFu,
               },
    .ram_layout = v8_ram_layout,
    .map_ram_decode = v8_map_ram_decode,
    .memory_layout_init = v8_memory_layout_init,
    .vram_base = V8_VRAM_BASE,
    // The decoder answers the whole low 16 MB (RAM, the 24-bit ROM and I/O
    // windows): nothing there times out.  A write the ROM's tables send to
    // the ROM through $A00000 completes and is dropped, as on the hardware.
    .ram_size_max = V8_LOW_IO_BASE + V8_LOW_IO_SIZE,
};

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

// MonP depth field -> the built-in video.
static void v8_rbv_mode(void *context, int depth_code) {
    v8_video_set_depth((config_t *)context, depth_code);
}

// MonP RvVIDOff -> the built-in video switched off or on.
static void v8_rbv_blank(void *context, bool video_off) {
    v8_video_set_blank((config_t *)context, video_off);
}

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
    if (v8_ops(cfg)->checkpoint)
        v8_ops(cfg)->checkpoint(cfg, cp);
    v8_video_checkpoint(cfg, cp);
}

// ============================================================
// Init / teardown
// ============================================================

static int v8_init(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    const v8_chip_ops_t *ops = v8_ops(cfg);
    v8_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    m68k_board_build_core(cfg, &ops->desc, cp);
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

    if (ops->ram_layout(cfg) != 0)
        return -1;

    // ADB, the images, the Combo's 5380, the sound block (an ASC subset),
    // SWIM and the Egret or Cuda companion, as the IIsi builds them.
    const rbv_board_spec_t spec = {
        .companion = b->cuda ? RBV_BOARD_CUDA : RBV_BOARD_EGRET,
        .asc_version = b->asc_version,
        .asc_fifo_only = true,
    };
    const rbv_board_slots_t slots = {.rbv = &st->rbv, .asc = &st->asc, .egret = &st->egret, .cuda = &st->cuda};
    if (rbv_board_build_devices(cfg, cp, &spec, &slots) != 0)
        return -1;

    // The pseudo-VIA2, its MonP sense lines strapped with the monitor.
    if (v8_video_init(cfg, cfg->build_opts.builtin_sense) != 0) {
        LOG(0, "Error: out of memory constructing the %s video", v8_ops(cfg)->desc.chipset);
        return -1;
    }
    if (rbv_board_build_rbv(cfg, cp, RBV_VARIANT_V8, v8_video_sense(cfg), &slots) != 0)
        return -1;
    // The V8's video follows MonP; Sonora's has its own depth and mode
    // registers at $50F28000 (sonora.c), so a MonP write must not reach it.
    // RvVIDOff switches off the VRAM video (the LC II's and the Color
    // Classic's); the Classic II's screen is scanned from DRAM by other logic,
    // and its ROM leaves the bit set.
    if (!ops->video_ctl) {
        rbv_set_mode_callback(st->rbv, v8_rbv_mode, cfg);
        if (b->fb_source == V8_FB_VRAM)
            rbv_set_blank_callback(st->rbv, v8_rbv_blank, cfg);
    }
    rbv_set_exp_callback(st->rbv, v8_ram_reg_written, cfg);

    st->mmu = m68k_board_build_mmu(cfg, ops->desc.rom_base, ops->desc.rom_end);
    if (!st->mmu)
        return -1;
    // The ROMs' MMU-enable sequence runs on from the instruction cache
    // across PMOVE to TC (mmu.h, icache_hold_*)
    st->mmu->icache_hold_enabled = true;
    // How far the decoder answers the PMMU's physical space (v8_chip_ops_t).
    if (ops->ram_size_max)
        st->mmu->ram_size_max = ops->ram_size_max;

    v8_io_bind(cfg);
    ops->memory_layout_init(cfg);

    machine_part_begin(cfg, cp, "v8");
    if (cp) {
        uint8_t armed = 1;
        system_read_checkpoint_data(cp, &armed, 1);
        system_read_checkpoint_data(cp, &st->ram_exp, 1);
        if (st->pcb_hidden)
            system_read_checkpoint_data(cp, st->pcb_hidden, st->pcb_size - (V8_RAM_SPACE - 8 * MB));
        if (ops->restore)
            ops->restore(cfg, cp);
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
// decoder's own half, the pseudo-VIA2 (whose RAM-decode register clears, so
// the RAM decode follows).
static void v8_bus_reset(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    m68k_board_bus_reset(cfg, &st->overlay);
    if (v8_ops(cfg)->bus_reset)
        v8_ops(cfg)->bus_reset(cfg);
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
        v8_video_teardown(cfg);
        free(st->pcb_hidden);
        st->pcb_hidden = NULL;
    }
    rbv_board_teardown(
        cfg, st ? &(rbv_board_slots_t){.rbv = &st->rbv, .asc = &st->asc, .egret = &st->egret, .cuda = &st->cuda} : NULL,
        st ? &st->mmu : NULL);
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
