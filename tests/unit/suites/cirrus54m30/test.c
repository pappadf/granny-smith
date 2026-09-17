// The Cirrus 54M30's BitBLT engine and its legacy-window mirror.
//
// Register truth: Cirrus Logic, "Alpine VGA Family CL-GD543X/4X Technical Reference Manual",
// 4th ed. (Feb 1995) -- sections 9.13 and 9.39-9.42 for the registers, Appendix B20 for the
// memory-mapped block, Appendix D8 for the engine.
//
// The card model is driven the way the machine drives it: through a real pci_bus_t window with
// LANE REVERSAL ON, which is what a Bandit does for a little-endian PowerPC.  That matters more
// here than anywhere else in this model, because the whole reason the BitBLT registers were not
// decoded before is a question about which byte lane a driver's register writes arrive on --
// so the first thing this suite pins is that a plain byte store lands where it was aimed, and
// every later row writes registers exactly as a driver would.
//
// The centrepiece is `blt_captured`: the real register block a Windows NT display driver left
// in display memory on 16 September 2026, when there was no engine to consume it.  Every field
// in it is a measurement, not a guess, and an engine that executes it correctly is doing the
// right thing.

#include "card.h"
#include "config_space.h"
#include "display.h"
#include "pci.h"
#include "system_config.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shared harness ships a decimal assertion; every value in this suite is a register or a
// pixel, so it wants a hexadecimal one.
#define ASSERT_EQ_HEX(a, b)                                                                                            \
    do {                                                                                                               \
        unsigned _a = (unsigned)(a), _b = (unsigned)(b);                                                               \
        if (_a != _b) {                                                                                                \
            fprintf(stderr, "[FAIL] %s:%d: %s != %s ($%02X != $%02X)\n", __FILE__, __LINE__, #a, #b, _a, _b);          \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

// --- Stubs for the environment pci.c reaches into ---------------------------
//
// pci.c's card registry names every registered driver by extern.  This suite links the real
// cirrus54m30.c, so `cirrus_54m30_kind` is the one name NOT stubbed here.

const pci_card_kind_t tnt_control_kind = {
    .id = "tnt_control", .display_name = "Control / Chaos on-board video", .attach = PCI_ATTACH_BUILTIN};
const pci_card_kind_t mach64_gx_kind = {
    .id = "mach64_gx", .display_name = "ATI Mach64 GX", .attach = PCI_ATTACH_PCI, .requires_prom = true};
const pci_card_kind_t sym53c825_ch0_kind = {
    .id = "sym53c825_0", .display_name = "Symbios 53C825A (channel 0)", .attach = PCI_ATTACH_BUILTIN};
const pci_card_kind_t sym53c825_ch1_kind = {
    .id = "sym53c825_1", .display_name = "Symbios 53C825A (channel 1)", .attach = PCI_ATTACH_BUILTIN};
const pci_card_kind_t voodoo2_kind = {.id = "voodoo2", .display_name = "3dfx Voodoo2", .attach = PCI_ATTACH_PCI};

// The one card kind this suite does NOT stub: cirrus54m30.c is linked in and defines it.
extern const pci_card_kind_t cirrus_54m30_kind;

void memory_signal_bus_error(uint32_t addr, bool write) {
    (void)addr;
    (void)write;
}

void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    (void)iface;
    (void)device;
}

void machine_config_note_slot_card(int bus_kind, int slot, const char *card_id) {
    (void)bus_kind;
    (void)slot;
    (void)card_id;
}

void pci_objects_build(pci_root_t *root) {
    (void)root;
}
void pci_objects_teardown(void) {}
void pci_objects_teardown_owned(pci_root_t *root) {
    (void)root;
}

// The card reads the scheduler only for Input Status 1, and only when cfg->scheduler is set;
// this suite leaves it NULL, so this exists purely to satisfy the linker.
uint64_t scheduler_cpu_cycles(scheduler_t *s) {
    (void)s;
    return 0;
}

// --- The rig ----------------------------------------------------------------

#define WIN_MAP  0x80000000u // where the bridge's memory window lands on the physical map
#define WIN_SIZE 0x10000000u
#define FB_BASE  0x81000000u // where BAR0 is assigned: 16 MB, 16 MB-aligned

#define VRAM_SIZE 0x00100000u
#define WIN_BASE  0x00FE0000u // the mirror, at the top of the aperture
#define WIN_MMIO  0x00018000u // $B8000 within the mirror

static pci_root_t *g_root;
static pci_bus_t *g_bus;
static pci_device_t *g_dev;
static const memory_interface_t *g_win;
static void *g_wctx;

static machine_substrate_t g_substrate;
static hw_profile_t g_profile;
static config_t g_cfg;

// A little-endian PowerPC does not reorder bytes: it XORs the low three address bits of every
// access with 8 - size, and the bridge's lane reversal undoes it.  These four helpers apply the
// processor's half of that, so the addresses below are the ones a guest would compute.
static void g_write8(uint32_t phys, uint8_t v) {
    g_win->write_uint8(g_wctx, (phys - WIN_MAP) ^ 7u, v);
}
static uint8_t g_read8(uint32_t phys) {
    return g_win->read_uint8(g_wctx, (phys - WIN_MAP) ^ 7u);
}
static void g_write16(uint32_t phys, uint16_t v) {
    g_win->write_uint16(g_wctx, (phys - WIN_MAP) ^ 6u, v);
}
static void g_write32(uint32_t phys, uint32_t v) {
    g_win->write_uint32(g_wctx, (phys - WIN_MAP) ^ 4u, v);
}
static uint32_t g_read32(uint32_t phys) {
    return g_win->read_uint32(g_wctx, (phys - WIN_MAP) ^ 4u);
}

// Display memory, straight through the linear aperture.
static void vram_write(uint32_t off, uint8_t v) {
    g_write8(FB_BASE + off, v);
}
static uint8_t vram_read(uint32_t off) {
    return g_read8(FB_BASE + off);
}

// The VGA index/data pairs, as I/O would reach them -- but this rig has no I/O window, so the
// suite drives the sequencer and graphics files through the same door a driver uses for setup:
// a fixed legacy I/O region.  Simpler here to reach them through the card's own memory-mapped
// block where possible, and through these two helpers where not.
static const memory_interface_t *g_io;
static void *g_ioctx;

static void io_write(uint32_t port, uint8_t v) {
    // This window's PCI base is zero, so its offset is the port number itself; the XOR is the
    // processor's lane munge, which the bridge undoes exactly as it does for memory.
    g_io->write_uint8(g_ioctx, port ^ 7u, v);
}

static void crtc_write(uint8_t index, uint8_t value) {
    io_write(0x3D4u, index);
    io_write(0x3D5u, value);
}

static uint8_t io_read(uint32_t port) {
    return g_io->read_uint8(g_ioctx, port ^ 7u);
}

static void seq_write(uint8_t index, uint8_t value) {
    io_write(0x3C4u, index);
    io_write(0x3C5u, value);
}

static void gr_write(uint8_t index, uint8_t value) {
    io_write(0x3CEu, index);
    io_write(0x3CFu, value);
}

static void cfg_write32(uint32_t reg, uint32_t value) {
    for (uint32_t b = 0; b < 4u; b++)
        pci_bus_cfg_write(g_bus, 15, 0, reg, b, (uint8_t)(value >> (8u * b)));
}

static void rig_setup(void) {
    g_profile.substrate = &g_substrate;
    g_cfg.machine = &g_profile;
    g_root = pci_root_create(&g_cfg);
    g_bus = pci_bus_create(g_root, "bandit1", 1);
    pci_bus_add_window(g_bus, PCI_SPACE_MEM, WIN_MAP, WIN_SIZE, WIN_MAP, 0xFFFFFFFFu, "pci-mem");
    pci_bus_add_window(g_bus, PCI_SPACE_IO, WIN_MAP + WIN_SIZE, 0x10000u, 0, 0xFFFFu, "pci-io");
    // Bandit reverses its eight byte lanes for a little-endian client.
    pci_bus_set_lane_reverse(g_bus, true);

    g_dev = cirrus_54m30_kind.factory(0, &g_cfg, NULL);
    ASSERT_TRUE(g_dev != NULL);
    pci_bus_add_device(g_bus, g_dev, 15);

    cfg_write32(PCI_CFG_BAR0, FB_BASE);
    // Where Open Firmware really puts the relocatable I/O BAR: above the sixteen bits a Bandit
    // drives, so every actual access goes to the strapped legacy block instead.
    cfg_write32(PCI_CFG_BAR0 + 4u, 0x00010000u);
    cfg_write32(PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE);

    g_win = pci_bus_window_iface(g_bus, 0);
    g_wctx = pci_bus_window_ctx(g_bus, 0);
    g_io = pci_bus_window_iface(g_bus, 1);
    g_ioctx = pci_bus_window_ctx(g_bus, 1);
    ASSERT_TRUE(g_win != NULL && g_io != NULL);

    // Unlock the Cirrus extensions the way every Alpine driver does, then put the register
    // block in memory at $B8000 (SR17[2]) with the window mapped 64 KB at $A0000 (GR6[3:2]=01),
    // which is the configuration Appendix B20 requires for memory-mapped I/O.
    seq_write(0x06, 0x12);
    gr_write(0x06, 0x04);
    seq_write(0x17, 0x04);
}

static void rig_teardown(void) {
    pci_root_delete(g_root);
    g_root = NULL;
    g_bus = NULL;
    g_dev = NULL;
}

// The memory-mapped register block, written the way a driver writes it: plain byte stores at
// the offsets Table B20-1 gives, through the mirror.
static void mmio_write(uint8_t off, uint8_t value) {
    g_write8(FB_BASE + WIN_BASE + WIN_MMIO + off, value);
}
static uint8_t mmio_read(uint8_t off) {
    return g_read8(FB_BASE + WIN_BASE + WIN_MMIO + off);
}

// Table B20-1 offsets, by name.
#define MM_BG0    0x00u
#define MM_BG1    0x01u
#define MM_FG0    0x04u
#define MM_FG1    0x05u
#define MM_WIDTH  0x08u
#define MM_HEIGHT 0x0Au
#define MM_DPITCH 0x0Cu
#define MM_SPITCH 0x0Eu
#define MM_DST    0x10u
#define MM_SRC    0x14u
#define MM_MASK   0x17u
#define MM_MODE   0x18u
#define MM_ROP    0x1Au
#define MM_START  0x40u

static void blt_setup(uint32_t width, uint32_t height, uint32_t dpitch, uint32_t spitch, uint32_t dst, uint32_t src,
                      uint8_t mode, uint8_t rop) {
    mmio_write(MM_WIDTH, (uint8_t)(width - 1u));
    mmio_write(MM_WIDTH + 1u, (uint8_t)((width - 1u) >> 8));
    mmio_write(MM_HEIGHT, (uint8_t)(height - 1u));
    mmio_write(MM_HEIGHT + 1u, (uint8_t)((height - 1u) >> 8));
    mmio_write(MM_DPITCH, (uint8_t)dpitch);
    mmio_write(MM_DPITCH + 1u, (uint8_t)(dpitch >> 8));
    mmio_write(MM_SPITCH, (uint8_t)spitch);
    mmio_write(MM_SPITCH + 1u, (uint8_t)(spitch >> 8));
    mmio_write(MM_DST, (uint8_t)dst);
    mmio_write(MM_DST + 1u, (uint8_t)(dst >> 8));
    mmio_write(MM_DST + 2u, (uint8_t)(dst >> 16));
    mmio_write(MM_SRC, (uint8_t)src);
    mmio_write(MM_SRC + 1u, (uint8_t)(src >> 8));
    mmio_write(MM_SRC + 2u, (uint8_t)(src >> 16));
    mmio_write(MM_MODE, mode);
    mmio_write(MM_ROP, rop);
}

// ============================================================================
TEST(lane_contract) {
    // The premise everything else rests on, and the one this model's HAL already depends on:
    // a plain byte store through a lane-reversed window lands at the offset it was aimed at.
    // The HAL's console writes every pixel this way, with no compensation anywhere.
    rig_setup();
    vram_write(0x100u, 0xA5u);
    ASSERT_EQ_HEX(vram_read(0x100u), 0xA5u);
    ASSERT_EQ_HEX(vram_read(0x107u), 0x00u); // and nowhere near offset ^ 7

    // A 32-bit store is little-endian in display memory: its least significant byte lands at
    // the lowest address, which is what a packed-pixel framebuffer needs.
    g_write32(FB_BASE + 0x200u, 0x44332211u);
    ASSERT_EQ_HEX(vram_read(0x200u), 0x11u);
    ASSERT_EQ_HEX(vram_read(0x201u), 0x22u);
    ASSERT_EQ_HEX(vram_read(0x202u), 0x33u);
    ASSERT_EQ_HEX(vram_read(0x203u), 0x44u);
    ASSERT_EQ_HEX(g_read32(FB_BASE + 0x200u), 0x44332211u);

    // A 16-bit store, likewise.
    g_write16(FB_BASE + 0x300u, 0xBEEFu);
    ASSERT_EQ_HEX(vram_read(0x300u), 0xEFu);
    ASSERT_EQ_HEX(vram_read(0x301u), 0xBEu);
    rig_teardown();
}

// ============================================================================
TEST(mirror_and_register_block) {
    rig_setup();

    // Below $B8000 the mirror is display memory.  With GR9 at zero the window's first byte is
    // display memory's first byte.
    vram_write(0x000u, 0x11u);
    ASSERT_EQ_HEX(g_read8(FB_BASE + WIN_BASE + 0x000u), 0x11u);
    g_write8(FB_BASE + WIN_BASE + 0x004u, 0x22u);
    ASSERT_EQ_HEX(vram_read(0x004u), 0x22u);

    // At $B8000 the register block answers instead, and GR31 is the one offset that reads back
    // (Appendix B20 section 1).  Every write goes to the graphics register Table B20-1 names.
    mmio_write(MM_MODE, 0x40u);
    mmio_write(MM_ROP, 0x59u);
    mmio_write(MM_FG0, 0x7Eu);
    ASSERT_EQ_HEX(mmio_read(MM_MODE), 0x40u);
    ASSERT_EQ_HEX(mmio_read(MM_ROP), 0x59u);
    ASSERT_EQ_HEX(mmio_read(MM_FG0), 0x7Eu);
    // ...and none of it reached display memory.
    ASSERT_EQ_HEX(vram_read(WIN_MMIO + MM_MODE), 0x00u);

    // "Address bits 14:8 are 'don't care', so the block is actually aliased at every 256
    // boundary from B800:0-BFF0:0."
    g_write8(FB_BASE + WIN_BASE + WIN_MMIO + 0x700u + MM_ROP, 0x0Du);
    ASSERT_EQ_HEX(mmio_read(MM_ROP), 0x0Du);
    g_write8(FB_BASE + WIN_BASE + 0x1FF00u + MM_ROP, 0x05u);
    ASSERT_EQ_HEX(mmio_read(MM_ROP), 0x05u);

    // "GR6[3:2] must be programmed to '0,1'" for memory-mapped I/O, which is what keeps $B8000
    // out of the display-memory window in the first place: at that setting the window is the
    // 64 KB at $A0000 and nothing above it decodes at all.
    ASSERT_EQ_HEX(g_read8(FB_BASE + WIN_BASE + 0x14000u), 0x00u);

    // With SR17[2] clear and the window opened to its full 128 KB, those same addresses are
    // display memory -- which is exactly how the driver's register block came to be found
    // sitting in VRAM as pixels on 16 September, with nothing decoding it as registers.
    seq_write(0x17, 0x00);
    gr_write(0x06, 0x00);
    g_write8(FB_BASE + WIN_BASE + WIN_MMIO + MM_MODE, 0x5Au);
    ASSERT_EQ_HEX(vram_read(WIN_MMIO + MM_MODE), 0x5Au);
    ASSERT_EQ_HEX(mmio_read(MM_MODE), 0x5Au); // the read is display memory too, now

    // SR17[6] moves the block to the last 256 bytes of the linear address space instead -- but
    // ONLY once linear addressing is enabled, which on this part means SR7[7:4] non-zero (TRM
    // 9.13: "if linear addressing is not enabled, this bit is ignored").  With SR7[7:4] = 0 the
    // bit is a don't-care and the block stays at $B8000.
    //
    // This is what wall E28 was.  Open Firmware's own Cirrus driver runs before NT with the
    // console on the screen and leaves SR17 = $62; cirrus.sys then ORs in bit 2 and gets $66 --
    // and this model, which took bit 6 as unconditional, moved the registers out from under a
    // driver that was still writing them at $B8000.  A 17 September browser log shows the
    // whole thing: "SR17 = $66", then every register write logged as a plain "window write"
    // into display memory, START read back as $0, and not one "BLT start" in 750 lines.
    gr_write(0x06, 0x04);
    seq_write(0x07, 0x01); // packed pixel, SR7[7:4] = 0: linear addressing OFF
    seq_write(0x17, 0x66); // the value the browser saw
    g_write8(FB_BASE + WIN_BASE + WIN_MMIO + MM_ROP, 0x6Du);
    ASSERT_EQ_HEX(mmio_read(MM_ROP), 0x6Du); // decoded at $B8000
    ASSERT_EQ_HEX(vram_read(WIN_MMIO + MM_ROP), 0x00u); // and not as a pixel
    ASSERT_EQ_HEX(g_read8(FB_BASE + VRAM_SIZE - 0x100u + MM_ROP), 0x00u); // nor at the top

    // Linear addressing on (cirrus.sys programs SR07 = $F1): now bit 6 means what it says.
    seq_write(0x07, 0xF1);
    g_write8(FB_BASE + VRAM_SIZE - 0x100u + MM_ROP, 0x5Du);
    ASSERT_EQ_HEX(g_read8(FB_BASE + VRAM_SIZE - 0x100u + MM_ROP), 0x5Du);
    ASSERT_EQ_HEX(g_read8(FB_BASE + WIN_BASE + WIN_MMIO + MM_ROP), 0x00u); // and not at $B8000
    rig_teardown();
}

// ============================================================================
TEST(rop_truth_table) {
    // Every one of the sixteen two-operand raster operations in TRM Table D8-3, run as a
    // one-byte screen-to-screen BLT over the four (source, destination) bit combinations at
    // once: source $CC, destination $AA gives each operation's truth table in the result.
    static const struct {
        uint8_t code;
        uint8_t want;
    } rows[] = {
        {0x00u, 0x00u}, // 0
        {0x90u, 0x11u}, // ~S & ~D
        {0x50u, 0x22u}, // ~S & D
        {0xD0u, 0x33u}, // ~S
        {0x09u, 0x44u}, // S & ~D
        {0x0Bu, 0x55u}, // ~D
        {0x59u, 0x66u}, // S ^ D
        {0xDAu, 0x77u}, // ~S | ~D
        {0x05u, 0x88u}, // S & D
        {0x95u, 0x99u}, // ~(S ^ D)
        {0x06u, 0xAAu}, // D
        {0xD6u, 0xBBu}, // ~S | D
        {0x0Du, 0xCCu}, // S
        {0xADu, 0xDDu}, // S | ~D
        {0x6Du, 0xEEu}, // S | D
        {0x0Eu, 0xFFu}, // 1
    };
    rig_setup();
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        vram_write(0x1000u, 0xCCu); // source
        vram_write(0x2000u, 0xAAu); // destination
        blt_setup(1u, 1u, 1u, 1u, 0x2000u, 0x1000u, 0x00u, rows[i].code);
        mmio_write(MM_START, 0x02u);
        ASSERT_EQ_HEX(vram_read(0x2000u), rows[i].want);
    }
    rig_teardown();
}

// ============================================================================
TEST(start_bit_clears) {
    // The acceptance criterion the whole route exists for: the driver writes the start bit,
    // polls, and sees a finished engine.  "This bit will be cleared to '0' when the BLT is
    // completed" is said of the START bit (TRM 9.40) as well as of the status bit, so both go,
    // and so does the progress bit.
    rig_setup();
    blt_setup(4u, 4u, 640u, 640u, 0x3000u, 0x1000u, 0x00u, 0x0Du);
    mmio_write(MM_START, 0x02u);
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x0Bu, 0x00u);

    // A reset clears the status and progress bits too (Appendix D8 section 14.2).
    mmio_write(MM_START, 0x04u);
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x09u, 0x00u);
    rig_teardown();
}

// ============================================================================
TEST(blt_captured) {
    // THE MEASUREMENT.  This is the register block a Windows NT display driver left in display
    // memory at the legacy window's $B8000 on 16 September 2026, when the emulated part had no
    // engine to consume it: a one-byte-wide, thirteen-line vertical strip, XOR-drawn from an
    // 8 x 8 pattern held just past the end of the visible frame buffer.  A focus rectangle or a
    // caret, in the button's neighbourhood at the bottom of a 640x480 screen.
    //
    //   width  - 1 = 0        -> 1 byte
    //   height - 1 = 12       -> 13 lines
    //   dest pitch = 640, source pitch = 16
    //   dest       = $03C94A  -> pixel (458, 387) at pitch 640
    //   source     = $04B040  -> 64 bytes past $4B000, the end of 640 x 480
    //   mask       = 0        -> no left-edge clipping
    //   mode       = $40      -> 8 x 8 pattern copy
    //   rop        = $59      -> S ^ D, SRCINVERT / PATINVERT
    rig_setup();

    // An 8 x 8 colour pattern is 64 bytes on a 64-byte boundary (Table D8-10).  Give each of
    // its 64 bytes a distinct value so a wrong row or column cannot pass.
    for (uint32_t i = 0; i < 64u; i++)
        vram_write(0x4B040u + i, (uint8_t)(0x10u + i));
    // A known destination: the thirteen bytes the strip will touch, and the byte beside each.
    for (uint32_t y = 0; y < 13u; y++) {
        vram_write(0x3C94Au + y * 640u, (uint8_t)(0x80u + y));
        vram_write(0x3C94Au + y * 640u + 1u, 0x5Au);
    }

    blt_setup(1u, 13u, 640u, 16u, 0x03C94Au, 0x04B040u, 0x40u, 0x59u);
    mmio_write(MM_MASK, 0x00u);
    mmio_write(MM_START, 0x02u);

    // Column 0 of the pattern, one row further down for each scan line, XORed into what was
    // there.  The pattern's vertical preset is the source address's low three bits, which are
    // zero here, so row 0 is where it starts.
    for (uint32_t y = 0; y < 13u; y++) {
        uint8_t pat = (uint8_t)(0x10u + ((y & 7u) * 8u)); // row y mod 8, column 0
        ASSERT_EQ_HEX(vram_read(0x3C94Au + y * 640u), (uint8_t)((0x80u + y) ^ pat));
        ASSERT_EQ_HEX(vram_read(0x3C94Au + y * 640u + 1u), 0x5Au); // one byte wide, and no more
    }
    // Nothing above or below the strip.
    ASSERT_EQ_HEX(vram_read(0x3C94Au - 640u), 0x00u);
    ASSERT_EQ_HEX(vram_read(0x3C94Au + 13u * 640u), 0x00u);
    // And the engine let go of the start bit.
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x0Bu, 0x00u);
    rig_teardown();
}

// ============================================================================
TEST(pattern_vertical_preset) {
    // "The low-order three bits of the Source Start Address (GR2C[2:0]) select the scan line to
    // be used for the first, or only, scan line" (Appendix D8 section 9).  Same pattern, source
    // address offset by three: the fill starts on the pattern's fourth row.
    rig_setup();
    for (uint32_t i = 0; i < 64u; i++)
        vram_write(0x8000u + i, (uint8_t)(0x10u + i));
    blt_setup(8u, 8u, 8u, 8u, 0x9000u, 0x8000u + 3u, 0x40u, 0x0Du);
    mmio_write(MM_START, 0x02u);
    for (uint32_t y = 0; y < 8u; y++)
        for (uint32_t x = 0; x < 8u; x++)
            ASSERT_EQ_HEX(vram_read(0x9000u + y * 8u + x), (uint8_t)(0x10u + ((3u + y) & 7u) * 8u + x));
    rig_teardown();
}

// ============================================================================
TEST(colour_expand) {
    // One bit of monochrome source becomes one 8 bpp pixel, most significant bit of the first
    // byte first, foreground from GR1 and background from GR0 (Tables D8-5 and D8-6).
    rig_setup();
    vram_write(0x5000u, 0xA5u); // 1010 0101
    mmio_write(MM_FG0, 0xF0u);
    mmio_write(MM_BG0, 0x0Fu);
    blt_setup(8u, 1u, 8u, 8u, 0x6000u, 0x5000u, 0x80u, 0x0Du); // expand, SRCCOPY
    mmio_write(MM_START, 0x02u);
    for (unsigned b = 0; b < 8u; b++)
        ASSERT_EQ_HEX(vram_read(0x6000u + b), ((0xA5u >> (7u - b)) & 1u) ? 0xF0u : 0x0Fu);

    // With transparency the zeroes are not written at all, and the background registers are
    // ignored outright on this part (Table D8-7).
    for (unsigned b = 0; b < 8u; b++)
        vram_write(0x7000u + b, 0x33u);
    blt_setup(8u, 1u, 8u, 8u, 0x7000u, 0x5000u, 0x88u, 0x0Du); // expand + transparency
    mmio_write(MM_START, 0x02u);
    for (unsigned b = 0; b < 8u; b++)
        ASSERT_EQ_HEX(vram_read(0x7000u + b), ((0xA5u >> (7u - b)) & 1u) ? 0xF0u : 0x33u);
    rig_teardown();
}

// ============================================================================
TEST(left_edge_clip) {
    // "if GR2F[2:0] are programmed to any value other than zero, the first n pixels of each
    // scan line of the destination will not be written" (Appendix D8 section 7).
    rig_setup();
    for (uint32_t i = 0; i < 8u; i++) {
        vram_write(0xA000u + i, (uint8_t)(0xC0u + i)); // source
        vram_write(0xB000u + i, 0x77u); // destination
    }
    blt_setup(8u, 1u, 8u, 8u, 0xB000u, 0xA000u, 0x00u, 0x0Du);
    mmio_write(MM_MASK, 0x03u);
    mmio_write(MM_START, 0x02u);
    for (uint32_t i = 0; i < 3u; i++)
        ASSERT_EQ_HEX(vram_read(0xB000u + i), 0x77u); // skipped
    for (uint32_t i = 3u; i < 8u; i++)
        ASSERT_EQ_HEX(vram_read(0xB000u + i), (uint8_t)(0xC0u + i));
    rig_teardown();
}

// ============================================================================
TEST(backwards_copy) {
    // "If GR30[0] is programmed to '1', the source and destination addresses will be decremented
    // ... the starting address will be the highest addressed byte in each area."  The case it
    // exists for is an overlapping copy that would otherwise eat its own source.
    rig_setup();
    for (uint32_t i = 0; i < 16u; i++)
        vram_write(0xC000u + i, (uint8_t)(0x40u + i));
    // Shift the sixteen bytes up by four, which overlaps: forwards this smears, backwards it
    // copies cleanly.  Both start addresses are the highest byte of their area.
    blt_setup(16u, 1u, 16u, 16u, 0xC000u + 19u, 0xC000u + 15u, 0x01u /* GR30[0]: decrement */, 0x0Du);
    mmio_write(MM_START, 0x02u);
    for (uint32_t i = 0; i < 16u; i++)
        ASSERT_EQ_HEX(vram_read(0xC004u + i), (uint8_t)(0x40u + i));
    rig_teardown();
}

// ============================================================================
TEST(system_to_screen) {
    // "If GR30[2] is programmed to '1', the BLT source will be system memory.  The CPU will
    // perform the bus transfers; the CL-GD543X/4X will ignore the address provided with such
    // transfers" -- so the engine stays busy after the start bit and every write into display
    // memory is its source.  The CPU must transfer DWORDs, and up to three bytes of the last
    // transfer for each scan line are discarded.
    rig_setup();
    // Three bytes per scan line, two scan lines, pitch 8: each line takes one DWORD and throws
    // its fourth byte away.
    blt_setup(3u, 2u, 8u, 0u, 0xD000u, 0u, 0x04u, 0x0Du);
    mmio_write(MM_START, 0x02u);
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x01u, 0x01u); // busy: the source has not arrived yet

    g_write32(FB_BASE + 0x0u, 0x99332211u); // $99 is the discarded fourth byte
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x01u, 0x01u); // one scan line to go
    g_write32(FB_BASE + 0x0u, 0x99665544u);

    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x0Bu, 0x00u); // and now it is done
    ASSERT_EQ_HEX(vram_read(0xD000u), 0x11u);
    ASSERT_EQ_HEX(vram_read(0xD001u), 0x22u);
    ASSERT_EQ_HEX(vram_read(0xD002u), 0x33u);
    ASSERT_EQ_HEX(vram_read(0xD003u), 0x00u); // the discarded byte went nowhere
    ASSERT_EQ_HEX(vram_read(0xD008u), 0x44u);
    ASSERT_EQ_HEX(vram_read(0xD009u), 0x55u);
    ASSERT_EQ_HEX(vram_read(0xD00Au), 0x66u);
    // ...and the writes that fed it did not land in display memory at the address they used.
    ASSERT_EQ_HEX(vram_read(0x0u), 0x00u);
    rig_teardown();
}

// ============================================================================
TEST(blit_stays_inside_vram) {
    // The aperture promises that writes above the fitted DRAM vanish, and the engine has to
    // keep that promise too: a 21-bit destination address reaches twice the memory this board
    // fits.  Nothing to assert but the absence of a crash and an untouched buffer.
    rig_setup();
    vram_write(0x0u, 0x5Au);
    blt_setup(256u, 16u, 1024u, 1024u, 0x1F0000u, 0x1F8000u, 0x00u, 0x0Du);
    mmio_write(MM_START, 0x02u);
    ASSERT_EQ_HEX(vram_read(0x0u), 0x5Au);
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x0Bu, 0x00u);
    rig_teardown();
}

// ============================================================================
TEST(unknown_rop_is_refused) {
    // GR32 has sixteen legal values and this is not one of them.  An engine that guessed would
    // scribble; this one logs and leaves the destination alone.
    rig_setup();
    vram_write(0xE000u, 0x11u);
    vram_write(0xE100u, 0x22u);
    blt_setup(1u, 1u, 1u, 1u, 0xE100u, 0xE000u, 0x00u, 0x37u);
    mmio_write(MM_START, 0x02u);
    ASSERT_EQ_HEX(vram_read(0xE100u), 0x22u);
    ASSERT_EQ_HEX(mmio_read(MM_START) & 0x0Bu, 0x00u); // but the driver is not left spinning
    rig_teardown();
}

// ============================================================================
TEST(window_banking) {
    // GR9 offsets the window into display memory in 4 KB pages, and GRB[5] makes those pages
    // 16 KB (TRM 9.19 and 9.21).  MODELLED FROM THE MANUAL: nothing on this machine drives the
    // window through a bank register, so this row is the only thing that exercises it.
    rig_setup();
    seq_write(0x17, 0x00); // no register block in the way
    vram_write(0x0B000u, 0x61u);
    vram_write(0x23000u, 0x62u);

    gr_write(0x09, 0x0Bu); // offset 11 pages of 4 KB
    ASSERT_EQ_HEX(g_read8(FB_BASE + WIN_BASE + 0x000u), 0x61u);

    gr_write(0x0B, 0x20u); // GRB[5]: the same offset now counts 16 KB pages
    gr_write(0x09, 0x08u); // 8 x 16 KB = $20000, plus SA[14:12] = 3 -> $23000
    ASSERT_EQ_HEX(g_read8(FB_BASE + WIN_BASE + 0x3000u), 0x62u);
    rig_teardown();
}

// ============================================================================
TEST(reset_restores_six_bit_dac) {
    // The hidden DAC register sits in the register file that PCI RST# clears, so a warm reset
    // has to leave the palette back in its six-bit VGA reading.  This row exists because the
    // model used to keep the eight-bit flag across a reset: a guest that rebooted out of a
    // session where the Cirrus driver had selected eight-bit palette data then had every
    // subsequent VGA palette write taken raw, and the whole screen came up four times too dark.
    rig_setup();
    // The descriptor only exists once a packed-pixel mode is programmed, so put the card in
    // 640x480 at 8 bpp the way a driver does: SR07[0] selects packed pixels with SR07[3:1] = 0
    // for eight of them, and the CRTC carries the geometry.
    seq_write(0x07, 0x01);
    seq_write(0x01, 0x01); // eight dots per character clock
    crtc_write(0x01, 79); // (79 + 1) * 8 = 640
    crtc_write(0x12, 0xDF);
    crtc_write(0x07, 0x02); // VDE bit 8: 479 + 1 = 480
    crtc_write(0x13, 80); // 80 * 8 = 640 bytes per line
    display_t *d = g_dev->ops->display(g_dev);
    ASSERT_TRUE(d != NULL);

    // The hidden register is reached the way a driver reaches it: four reads of $3C6 arm the
    // door and the fifth access lands on the register (TRM 9.13).  Bit 1 selects eight bits.
    for (int i = 0; i < 4; i++)
        (void)io_read(0x3C6u);
    io_write(0x3C6u, 0x02u);

    // In eight-bit mode the written value is the intensity itself.
    io_write(0x3C8u, 0x01u);
    for (int ch = 0; ch < 3; ch++)
        io_write(0x3C9u, 0x3Fu);
    ASSERT_EQ_HEX(d->clut[1].r, 0x3Fu);

    g_dev->ops->reset(g_dev, &g_cfg); // RST#, as a machine reset delivers it

    // The same three bytes are a six-bit VGA value again, and $3F is full white.
    io_write(0x3C8u, 0x01u);
    for (int ch = 0; ch < 3; ch++)
        io_write(0x3C9u, 0x3Fu);
    ASSERT_EQ_HEX(d->clut[1].r, 0xFFu);
    ASSERT_EQ_HEX(d->clut[1].g, 0xFFu);
    ASSERT_EQ_HEX(d->clut[1].b, 0xFFu);

    // And the door itself is shut again: a bare write to $3C6 after the reset is a pel mask
    // write, not a hidden-register write, so it must not put the DAC back into eight-bit mode.
    io_write(0x3C6u, 0x02u);
    io_write(0x3C8u, 0x02u);
    for (int ch = 0; ch < 3; ch++)
        io_write(0x3C9u, 0x3Fu);
    ASSERT_EQ_HEX(d->clut[2].r, 0xFFu);
    rig_teardown();
}

// ============================================================================
int main(void) {
    RUN(lane_contract);
    RUN(mirror_and_register_block);
    RUN(rop_truth_table);
    RUN(start_bit_clears);
    RUN(blt_captured);
    RUN(pattern_vertical_preset);
    RUN(colour_expand);
    RUN(left_edge_clip);
    RUN(backwards_copy);
    RUN(system_to_screen);
    RUN(blit_stays_inside_vram);
    RUN(unknown_rop_is_refused);
    RUN(window_banking);
    RUN(reset_restores_six_bit_dac);
    fprintf(stderr, "[ OK ] cirrus54m30\n");
    return 0;
}
