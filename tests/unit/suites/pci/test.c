// Generic PCI core unit tests (proposal-pci-architecture §13).
//
// Links the real config_space.c + pci.c against a stub bus environment and
// pins the contract every guest on these machines depends on: absent
// devices read all-ones and swallow writes; the type-0 header assembles
// from the declaration; BAR sizing works by masked read-back for every BAR
// kind; the command register gates the decode; the expansion-ROM BAR's
// enable bit is honoured; bar_map fires exactly on real transitions; the
// bridge-window dispatcher routes to the right device and faults on
// everything else; and staged card picks resolve by the documented
// precedence and are consumed at the slot walk.
//
// The empty-slot coverage in particular is the gap this suite closes: the
// hand-rolled model it replaces was never unit-tested at all.

#include "config_space.h"
#include "pci.h"
#include "system_config.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Stubs for the environment pci.c reaches into ---------------------------
//
// pci.c's card registry names the one registered driver by extern, exactly
// as nubus.c names its own (the "explicit list, no linker constructors"
// rule).  The real one lives in machines/tnt/control.c, which this suite
// deliberately does not link — so it gets a factory-less stand-in.
const pci_card_kind_t tnt_control_kind = {
    .id = "tnt_control", .display_name = "Control / Chaos on-board video", .attach = PCI_ATTACH_BUILTIN};
// ...and the pluggable one, from core/peripherals/pci/cards/mach64gx.c, which
// this suite also does not link (it would drag in the whole prom/object
// stack).  requires_prom is kept true so the socket-fit and staged-pick
// rows below exercise a card with a real ROM requirement.
const pci_card_kind_t mach64_gx_kind = {.id = "mach64_gx",
                                        .display_name = "Apple Accelerated PCI Graphics Card (ATI Mach64 GX)",
                                        .attach = PCI_ATTACH_PCI,
                                        .requires_prom = true,
                                        .card_class = "display"};
// ...and the three soldered-down devices of the Apple Network Server, whose
// real drivers live in cards/cirrus54m30.c and cards/sym53c825.c.  All
// BUILTIN, so they never appear in a socket-fit row; they are here because
// the registry is an explicit list and the linker wants every name in it.
const pci_card_kind_t cirrus_54m30_kind = {.id = "cirrus_54m30",
                                           .display_name = "Cirrus Logic 54M30 on-board video",
                                           .attach = PCI_ATTACH_BUILTIN,
                                           .card_class = "display"};
const pci_card_kind_t sym53c825_ch0_kind = {.id = "sym53c825_0",
                                            .display_name = "Symbios 53C825A fast/wide SCSI (channel 0)",
                                            .attach = PCI_ATTACH_BUILTIN,
                                            .card_class = "scsi"};
const pci_card_kind_t sym53c825_ch1_kind = {.id = "sym53c825_1",
                                            .display_name = "Symbios 53C825A fast/wide SCSI (channel 1)",
                                            .attach = PCI_ATTACH_BUILTIN,
                                            .card_class = "scsi"};

static uint32_t g_bus_error_addr;
static int g_bus_errors;
static int g_slot_irqs;
static int g_last_irq_slot;
static bool g_last_irq_active;

void memory_signal_bus_error(uint32_t addr, bool write) {
    (void)write;
    g_bus_error_addr = addr;
    g_bus_errors++;
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

// The object model is exercised by the integration suites, not here.
void pci_objects_build(pci_root_t *root) {
    (void)root;
}
void pci_objects_teardown(void) {}
void pci_objects_teardown_owned(pci_root_t *root) {
    (void)root;
}

static void stub_slot_irq(config_t *cfg, int slot, bool active) {
    (void)cfg;
    g_slot_irqs++;
    g_last_irq_slot = slot;
    g_last_irq_active = active;
}

// A machine just real enough for pci.c: a substrate with the slot-IRQ hook
// and a profile that points at it.
static machine_substrate_t g_substrate;
static hw_profile_t g_profile;
static config_t g_cfg;

static config_t *test_cfg(void) {
    g_substrate.pci_slot_irq = stub_slot_irq;
    g_profile.substrate = &g_substrate;
    g_cfg.machine = &g_profile;
    return &g_cfg;
}

// --- A test device: two memory BARs, one I/O BAR, an expansion ROM ---------

#define DEV_REGS_SIZE 0x1000u
#define DEV_VRAM_SIZE 0x100000u
#define DEV_IO_SIZE   0x100u
#define DEV_ROM_SIZE  0x10000u

static const pci_config_decl_t test_decl = {
    .vendor_id = 0x1002u,
    .device_id = 0x4758u,
    .revision = 0x02u,
    .class_code = 0x030000u, // display controller
    .header_type = 0x00u,
    .interrupt_pin = 1,
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
    .bar =
        {
              [0] = {.size = DEV_REGS_SIZE, .kind = PCI_BAR_MEM},
              [1] = {.size = DEV_VRAM_SIZE, .kind = PCI_BAR_MEM_PREFETCH},
              [2] = {.size = DEV_IO_SIZE, .kind = PCI_BAR_IO},
              },
    .rom_size = DEV_ROM_SIZE,
};

// Region backings: each records the last access so the dispatcher's routing
// is observable.
typedef struct {
    int reads, writes;
    uint32_t last_offset;
    uint32_t last_value;
} region_log_t;

static region_log_t g_regs, g_vram, g_io;

static uint8_t region_read8(void *ctx, uint32_t offset) {
    region_log_t *r = (region_log_t *)ctx;
    r->reads++;
    r->last_offset = offset;
    return (uint8_t)offset;
}
static uint16_t region_read16(void *ctx, uint32_t offset) {
    region_log_t *r = (region_log_t *)ctx;
    r->reads++;
    r->last_offset = offset;
    return 0x1234u;
}
static uint32_t region_read32(void *ctx, uint32_t offset) {
    region_log_t *r = (region_log_t *)ctx;
    r->reads++;
    r->last_offset = offset;
    return 0xDEADBEEFu;
}
static void region_write8(void *ctx, uint32_t offset, uint8_t value) {
    region_log_t *r = (region_log_t *)ctx;
    r->writes++;
    r->last_offset = offset;
    r->last_value = value;
}
static void region_write16(void *ctx, uint32_t offset, uint16_t value) {
    region_log_t *r = (region_log_t *)ctx;
    r->writes++;
    r->last_offset = offset;
    r->last_value = value;
}
static void region_write32(void *ctx, uint32_t offset, uint32_t value) {
    region_log_t *r = (region_log_t *)ctx;
    r->writes++;
    r->last_offset = offset;
    r->last_value = value;
}

static const memory_interface_t region_iface = {
    .read_uint8 = region_read8,
    .read_uint16 = region_read16,
    .read_uint32 = region_read32,
    .write_uint8 = region_write8,
    .write_uint16 = region_write16,
    .write_uint32 = region_write32,
};

// bar_map observations
static int g_map_calls;
static int g_last_map_bar;
static uint32_t g_last_map_base;
static bool g_last_map_on;

static void test_bar_map(pci_device_t *dev, int bar, uint32_t base, bool on) {
    (void)dev;
    g_map_calls++;
    g_last_map_bar = bar;
    g_last_map_base = base;
    g_last_map_on = on;
}

static const char *test_name(const pci_device_t *dev) {
    (void)dev;
    return "test device";
}

static const pci_device_ops_t test_ops = {
    .bar_map = test_bar_map,
    .name = test_name,
};

static pci_device_t g_dev;

static void device_reset(void) {
    memset(&g_dev, 0, sizeof(g_dev));
    memset(&g_regs, 0, sizeof(g_regs));
    memset(&g_vram, 0, sizeof(g_vram));
    memset(&g_io, 0, sizeof(g_io));
    g_map_calls = 0;
    g_bus_errors = 0;
    g_dev.ops = &test_ops;
    g_dev.decl = &test_decl;
    pci_cfg_reset(&g_dev);
    pci_bar_backing_iface(&g_dev, 0, &region_iface, &g_regs);
    pci_bar_backing_iface(&g_dev, 1, &region_iface, &g_vram);
    pci_bar_backing_iface(&g_dev, 2, &region_iface, &g_io);
}

// Write a full little-endian dword into a config register, one byte lane at
// a time — exactly how a bridge's data port delivers it.
static void cfg_write_dword(pci_device_t *dev, uint32_t reg, uint32_t value) {
    for (uint32_t b = 0; b < 4; b++)
        pci_cfg_write(dev, reg, b, (uint8_t)(value >> (8 * b)));
}

// === Tests ==================================================================

TEST(test_header_assembly) {
    device_reset();
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_ID) == 0x47581002u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_CLASS) == 0x03000002u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_COMMAND) == 0u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_MISC) == 0u);
    // The interrupt PIN is strapped and read-only; the LINE is a plain byte.
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_INTERRUPT) == 0x0100u);
    pci_cfg_write(&g_dev, PCI_CFG_INTERRUPT, 0, 23);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_INTERRUPT) == 0x0117u);
    pci_cfg_write(&g_dev, PCI_CFG_INTERRUPT, 1, 0xFF); // the pin ignores writes
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_INTERRUPT) == 0x0117u);
    // Cache line size and latency timer latch; header type does not.
    pci_cfg_write(&g_dev, PCI_CFG_MISC, 0, 8);
    pci_cfg_write(&g_dev, PCI_CFG_MISC, 1, 0x20);
    pci_cfg_write(&g_dev, PCI_CFG_MISC, 2, 0x80);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_MISC) == 0x00002008u);
    // A register the device does not implement reads ZERO — the device
    // exists; only absent DEVICES read all-ones.
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_SUBSYSTEM) == 0u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, 0x40u) == 0u);
}

TEST(test_command_masking) {
    device_reset();
    // Only declared-writable bits stick.
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, 0xFFFFu);
    ASSERT_TRUE((pci_cfg_read(&g_dev, PCI_CFG_COMMAND) & 0xFFFFu) ==
                (PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER));
    // The status halfword is write-1-to-clear over bits we never set.
    ASSERT_TRUE((pci_cfg_read(&g_dev, PCI_CFG_COMMAND) >> 16) == 0u);

    // A device with a HARDWIRED command bit keeps it through any write —
    // Control's case, where the Chaos bus swallows config writes so the
    // device must decode unconditionally.
    static const pci_config_decl_t hardwired = {
        .vendor_id = 1, .command_reset = PCI_CMD_MEM_SPACE, .bar = {[0] = {.size = 0x1000u, .kind = PCI_BAR_MEM}}};
    pci_device_t d = {.decl = &hardwired};
    pci_cfg_reset(&d);
    ASSERT_TRUE((pci_cfg_read(&d, PCI_CFG_COMMAND) & PCI_CMD_MEM_SPACE) != 0);
    cfg_write_dword(&d, PCI_CFG_COMMAND, 0);
    ASSERT_TRUE((pci_cfg_read(&d, PCI_CFG_COMMAND) & PCI_CMD_MEM_SPACE) != 0);
}

TEST(test_bar_sizing) {
    device_reset();
    // The universal probe: write all-ones, read back the size mask with the
    // BAR's type bits in the low nibble.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0xFFFFFFFFu);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR0) == (0u - DEV_REGS_SIZE));
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 4, 0xFFFFFFFFu);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR0 + 4) == ((0u - DEV_VRAM_SIZE) | 0x8u)); // prefetchable
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 8, 0xFFFFFFFFu);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR0 + 8) == ((0u - DEV_IO_SIZE) | 0x1u)); // I/O
    // An unimplemented BAR reads zero and swallows writes — that is how the
    // guest learns the device has only three.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 12, 0xFFFFFFFFu);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR0 + 12) == 0u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR5) == 0u);

    // Assignment: the low bits are forced by the size mask.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0x80001234u);
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_BAR0) == 0x80001000u);
    ASSERT_EQ_INT((int)pci_cfg_bar_base(&g_dev, 0), (int)0x80001000u);
    ASSERT_EQ_INT((int)pci_cfg_bar_size(&g_dev, 0), (int)DEV_REGS_SIZE);
}

TEST(test_rom_bar) {
    device_reset();
    cfg_write_dword(&g_dev, PCI_CFG_ROM_BAR, 0xFFFFFFFFu);
    // The sizing read-back keeps the enable bit out of the mask.
    ASSERT_TRUE(pci_cfg_read(&g_dev, PCI_CFG_ROM_BAR) == ((0u - DEV_ROM_SIZE) | PCI_ROM_BAR_ENABLE));
    cfg_write_dword(&g_dev, PCI_CFG_ROM_BAR, 0x81000000u); // assigned, disabled
    ASSERT_TRUE(!pci_cfg_bar_enabled(&g_dev, PCI_ROM_BAR_INDEX));
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE);
    ASSERT_TRUE(!pci_cfg_bar_enabled(&g_dev, PCI_ROM_BAR_INDEX)); // still no enable bit
    cfg_write_dword(&g_dev, PCI_CFG_ROM_BAR, 0x81000000u | PCI_ROM_BAR_ENABLE);
    ASSERT_TRUE(pci_cfg_bar_enabled(&g_dev, PCI_ROM_BAR_INDEX));
    ASSERT_EQ_INT((int)pci_cfg_bar_base(&g_dev, PCI_ROM_BAR_INDEX), (int)0x81000000u);

    // A device without an expansion ROM reads $30 as zero and ignores it.
    static const pci_config_decl_t no_rom = {.vendor_id = 1};
    pci_device_t d = {.decl = &no_rom};
    pci_cfg_reset(&d);
    cfg_write_dword(&d, PCI_CFG_ROM_BAR, 0xFFFFFFFFu);
    ASSERT_TRUE(pci_cfg_read(&d, PCI_CFG_ROM_BAR) == 0u);
}

TEST(test_bar_map_transitions) {
    device_reset();
    // An assigned BAR does not decode until the command register enables
    // its space — and the notification fires only on real transitions.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0x80001000u);
    ASSERT_EQ_INT(g_map_calls, 0);
    ASSERT_TRUE(!pci_cfg_bar_enabled(&g_dev, 0));

    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE);
    ASSERT_EQ_INT(g_map_calls, 1);
    ASSERT_EQ_INT(g_last_map_bar, 0);
    ASSERT_EQ_INT((int)g_last_map_base, (int)0x80001000u);
    ASSERT_TRUE(g_last_map_on);

    // Re-writing the same value is not a transition.
    int before = g_map_calls;
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0x80001000u);
    ASSERT_EQ_INT(g_map_calls, before);

    // Moving it is.  The bridge data port delivers a BAR one byte at a
    // time, so an assignment that changes several lanes decodes at each
    // intermediate base — exactly as the hardware does; what the model
    // guarantees is that the FINAL state is right and that no transition
    // goes unreported.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0x82000000u);
    ASSERT_TRUE(g_map_calls > before);
    ASSERT_EQ_INT((int)g_last_map_base, (int)0x82000000u);
    ASSERT_TRUE(g_last_map_on);
    ASSERT_EQ_INT((int)pci_cfg_bar_base(&g_dev, 0), (int)0x82000000u);

    // Clearing the space-enable gate undecodes it.
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, 0);
    ASSERT_TRUE(!g_last_map_on);
    ASSERT_TRUE(!pci_cfg_bar_enabled(&g_dev, 0));

    // An I/O BAR is gated by the I/O bit, not the memory bit.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 8, 0x00001000u);
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE);
    ASSERT_TRUE(!pci_cfg_bar_enabled(&g_dev, 2));
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE);
    ASSERT_TRUE(pci_cfg_bar_enabled(&g_dev, 2));
}

TEST(test_absent_devices_read_all_ones) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    device_reset();
    pci_bus_add_device(bus, &g_dev, 13);

    // The seated device answers...
    ASSERT_TRUE(pci_bus_cfg_read(bus, 13, 0, PCI_CFG_ID) == 0x47581002u);
    // ...and every other IDSEL — the entire empty-slot model — is all-ones,
    // for every register, with writes vanishing.
    for (int d = 0; d < 32; d++) {
        if (d == 13)
            continue;
        ASSERT_TRUE(pci_bus_cfg_read(bus, d, 0, PCI_CFG_ID) == 0xFFFFFFFFu);
        ASSERT_TRUE(pci_bus_cfg_read(bus, d, 0, PCI_CFG_BAR0) == 0xFFFFFFFFu);
        pci_bus_cfg_write(bus, d, 0, PCI_CFG_BAR0, 0, 0xFF); // must not crash
    }
    // Functions other than 0 are all-ones until a multi-function device
    // exists.
    ASSERT_TRUE(pci_bus_cfg_read(bus, 13, 1, PCI_CFG_ID) == 0xFFFFFFFFu);
    pci_root_delete(root);
}

TEST(test_window_dispatch_and_faults) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    device_reset();
    pci_bus_add_device(bus, &g_dev, 13);
    pci_bus_add_window(bus, PCI_SPACE_MEM, 0x80000000u, 0x10000000u, 0x80000000u, 0xFFFFFFFFu, "mem");
    pci_bus_add_window(bus, PCI_SPACE_IO, 0xF2000000u, 0x00800000u, 0, 0xFFFFu, "io");

    // Nothing decoded yet: every access in the window faults recoverably.
    const memory_interface_t *mem_if = pci_bus_window_iface(bus, 0);
    void *mem_ctx = pci_bus_window_ctx(bus, 0);
    ASSERT_TRUE(mem_if->read_uint32(mem_ctx, 0x1000u) == 0xFFFFFFFFu);
    ASSERT_EQ_INT(g_bus_errors, 1);
    ASSERT_EQ_INT((int)g_bus_error_addr, (int)0x80001000u);

    // Assign and enable: the register block answers, at the right offset.
    cfg_write_dword(&g_dev, PCI_CFG_BAR0, 0x80001000u);
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 4, 0x81000000u);
    cfg_write_dword(&g_dev, PCI_CFG_BAR0 + 8, 0x00002000u);
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE);

    ASSERT_TRUE(mem_if->read_uint32(mem_ctx, 0x1040u) == 0xDEADBEEFu);
    ASSERT_EQ_INT(g_regs.reads, 1);
    ASSERT_EQ_INT((int)g_regs.last_offset, 0x40);

    mem_if->write_uint8(mem_ctx, 0x01000010u, 0x5A); // into the VRAM BAR
    ASSERT_EQ_INT(g_vram.writes, 1);
    ASSERT_EQ_INT((int)g_vram.last_offset, 0x10);
    ASSERT_EQ_INT((int)g_vram.last_value, 0x5A);

    // An I/O window drives only the low 16 bits, so the same physical page
    // reaches PCI I/O address $2000 + offset.
    const memory_interface_t *io_if = pci_bus_window_iface(bus, 1);
    void *io_ctx = pci_bus_window_ctx(bus, 1);
    io_if->write_uint16(io_ctx, 0x2004u, 0x1234);
    ASSERT_EQ_INT(g_io.writes, 1);
    ASSERT_EQ_INT((int)g_io.last_offset, 4);

    // A memory BAR never answers an I/O cycle at the same address.
    g_bus_errors = 0;
    io_if->read_uint8(io_ctx, 0x1000u);
    ASSERT_EQ_INT(g_bus_errors, 1);

    // Space the device does not decode still faults.
    g_bus_errors = 0;
    mem_if->read_uint8(mem_ctx, 0x0F000000u);
    ASSERT_EQ_INT(g_bus_errors, 1);

    // PCI RST# drops the command register, and with it the whole decode.
    pci_reset(root);
    g_bus_errors = 0;
    mem_if->read_uint32(mem_ctx, 0x1040u);
    ASSERT_EQ_INT(g_bus_errors, 1);
    pci_root_delete(root);
}

// A device that decodes I/O at STRAPPED addresses, with no I/O BAR — the
// mach64 GX's arrangement, and the reason pci_device_add_fixed_region
// exists.  Three things are pinned: a sparse region answers only its own
// congruence class, an address inside the same window that is NOT in that
// class still faults, and the command register's space-enable bit gates
// the whole thing exactly as it gates a BAR.
TEST(test_fixed_region_sparse_decode) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    device_reset();
    pci_bus_add_device(bus, &g_dev, 13);
    pci_bus_add_window(bus, PCI_SPACE_IO, 0xF2000000u, 0x00800000u, 0, 0xFFFFu, "io");

    // ISA-style sparse decoding: compare the low 10 bits against the base,
    // use bits 15:10 as a register select.  64 KB span, base $2EC.
    //
    // The mask is $3FC, not $3FF: each selected register is 32 bits wide,
    // so base+0..base+3 are byte lanes of the SAME register and all four
    // must decode.  (Every strap-selectable base — $2EC / $1CC / $1C8 — is
    // dword-aligned, so masking the low two bits off is exactly right.)
    // The card's own FCode proves the lanes are used: it drives
    // CONFIG_CNTL's upper halfword at $6AEE and reads the monitor-sense
    // byte at DAC_CNTL+3 = $62EF.
    pci_device_add_fixed_region(&g_dev, PCI_SPACE_IO, 0x0u, 0x10000u, 0x3FCu, 0x2ECu, &region_iface, &g_io);
    const memory_interface_t *io_if = pci_bus_window_iface(bus, 0);
    void *io_ctx = pci_bus_window_ctx(bus, 0);

    // The command register has not enabled I/O space yet: nothing decodes,
    // even at an address the match would accept.
    g_bus_errors = 0;
    io_if->read_uint32(io_ctx, 0x72ECu);
    ASSERT_EQ_INT(g_bus_errors, 1);
    ASSERT_EQ_INT(g_io.reads, 0);

    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_IO_SPACE);

    // Now the congruence class answers, and the handler is given the raw
    // I/O address so the card can do its own (sel << 10) sub-decode.
    g_bus_errors = 0;
    ASSERT_TRUE(io_if->read_uint32(io_ctx, 0x72ECu) == 0xDEADBEEFu);
    ASSERT_EQ_INT(g_bus_errors, 0);
    ASSERT_EQ_INT(g_io.reads, 1);
    ASSERT_EQ_INT((int)g_io.last_offset, 0x72EC);

    // Byte and halfword accesses at base+2 / base+3 reach it too — the
    // card's own FCode drives CONFIG_CNTL's upper halfword at $6AEE and
    // the monitor-sense byte at DAC_CNTL+3 ($62EF).
    io_if->write_uint16(io_ctx, 0x6AEEu, 0x000F);
    ASSERT_EQ_INT((int)g_io.last_offset, 0x6AEE);
    io_if->write_uint8(io_ctx, 0x62EFu, 0x07);
    ASSERT_EQ_INT((int)g_io.last_offset, 0x62EF);
    ASSERT_EQ_INT(g_io.writes, 2);

    // An address in the same 64 KB window but a DIFFERENT congruence class
    // is not ours: it must still fault.  (A contiguous "claim base..base+
    // size" region would have swallowed all 64 KB here.)  $72E8 is the
    // dword immediately below CONFIG_STAT0 and is NOT decoded.
    g_bus_errors = 0;
    g_io.reads = 0;
    io_if->read_uint32(io_ctx, 0x72E8u);
    ASSERT_EQ_INT(g_bus_errors, 1);
    io_if->read_uint32(io_ctx, 0x7000u);
    ASSERT_EQ_INT(g_bus_errors, 2);
    ASSERT_EQ_INT(g_io.reads, 0);

    // The gate flips decode back off — the card's own firmware clears the
    // I/O-space bit when it is done probing.
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, 0);
    g_bus_errors = 0;
    io_if->read_uint32(io_ctx, 0x72ECu);
    ASSERT_EQ_INT(g_bus_errors, 1);
    ASSERT_EQ_INT(g_io.reads, 0);

    // A fixed region lives in ONE space: a memory cycle at the same
    // numeric address never reaches an I/O region.
    pci_bus_add_window(bus, PCI_SPACE_MEM, 0x80000000u, 0x10000000u, 0x80000000u, 0xFFFFFFFFu, "mem");
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE);
    const memory_interface_t *mem_if = pci_bus_window_iface(bus, 1);
    void *mem_ctx = pci_bus_window_ctx(bus, 1);
    g_bus_errors = 0;
    mem_if->read_uint32(mem_ctx, 0x72ECu);
    ASSERT_EQ_INT(g_bus_errors, 1);
    ASSERT_EQ_INT(g_io.reads, 0);
    pci_root_delete(root);
}

// A mask of zero makes the match vacuous, which is how the same mechanism
// expresses an ordinary contiguous legacy claim.
TEST(test_fixed_region_contiguous) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    device_reset();
    pci_bus_add_device(bus, &g_dev, 13);
    pci_bus_add_window(bus, PCI_SPACE_IO, 0xF2000000u, 0x00800000u, 0, 0xFFFFu, "io");
    pci_device_add_fixed_region(&g_dev, PCI_SPACE_IO, 0x3B0u, 0x30u, 0, 0, &region_iface, &g_io);
    cfg_write_dword(&g_dev, PCI_CFG_COMMAND, PCI_CMD_IO_SPACE);

    const memory_interface_t *io_if = pci_bus_window_iface(bus, 0);
    void *io_ctx = pci_bus_window_ctx(bus, 0);
    // Inside the range, and the handler sees a BASE-RELATIVE offset.
    g_bus_errors = 0;
    io_if->read_uint8(io_ctx, 0x3B4u);
    ASSERT_EQ_INT(g_io.reads, 1);
    ASSERT_EQ_INT((int)g_io.last_offset, 4);
    // Either side of it faults.
    io_if->read_uint8(io_ctx, 0x3AFu);
    io_if->read_uint8(io_ctx, 0x3E0u);
    ASSERT_EQ_INT(g_bus_errors, 2);
    ASSERT_EQ_INT(g_io.reads, 1);
    pci_root_delete(root);
}

TEST(test_slot_interrupts) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    device_reset();
    pci_bus_add_device(bus, &g_dev, 13);
    g_dev.slot_index = 2;
    g_slot_irqs = 0;

    pci_assert_irq(&g_dev);
    ASSERT_EQ_INT(g_slot_irqs, 1);
    ASSERT_EQ_INT(g_last_irq_slot, 2);
    ASSERT_TRUE(g_last_irq_active);
    // The deassert edge is load-bearing on a level-sensitive line.
    pci_deassert_irq(&g_dev);
    ASSERT_EQ_INT(g_slot_irqs, 2);
    ASSERT_TRUE(!g_last_irq_active);
    pci_root_delete(root);
}

// --- Slot-table resolution ---------------------------------------------------

static int g_factory_calls;
static int g_factory_slots[8];

static pci_device_t *counting_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    (void)cfg;
    (void)cp;
    if (g_factory_calls < 8)
        g_factory_slots[g_factory_calls] = slot_index;
    g_factory_calls++;
    pci_device_t *d = (pci_device_t *)calloc(1, sizeof(pci_device_t));
    if (!d)
        return NULL;
    d->ops = &test_ops;
    d->decl = &test_decl;
    pci_cfg_reset(d);
    return d;
}

// Two kinds so the fits-socket predicate has something to reject.
static const pci_card_kind_t g_socket_kind = {
    .id = "test_card", .display_name = "Test Card", .attach = PCI_ATTACH_PCI, .factory = counting_factory};
static const pci_card_kind_t g_builtin_kind = {
    .id = "test_builtin", .display_name = "Test Builtin", .attach = PCI_ATTACH_BUILTIN, .factory = counting_factory};

TEST(test_card_fits_socket) {
    pci_slot_decl_t socket = {.slot = 1, .kind = PCI_SLOT_SOCKET, .bus = 0, .device = 13};
    pci_slot_decl_t builtin = {.slot = 2, .kind = PCI_SLOT_BUILTIN, .bus = 0, .device = 14};
    ASSERT_TRUE(pci_card_fits_socket(&socket, &g_socket_kind));
    // A soldered-down device is never offered on a socket — the
    // conservative zero default.
    ASSERT_TRUE(!pci_card_fits_socket(&socket, &g_builtin_kind));
    // A builtin slot is not user-configurable at all.
    ASSERT_TRUE(!pci_card_fits_socket(&builtin, &g_socket_kind));
    ASSERT_TRUE(!pci_card_fits_socket(NULL, &g_socket_kind));
    ASSERT_TRUE(!pci_card_fits_socket(&socket, NULL));
}

TEST(test_staged_precedence_and_consumption) {
    pci_staged_clear_all();
    // The wildcard is the machine-independent channel; a concrete entry
    // beats it for that slot.
    pci_staged_card_set(PCI_STAGED_WILDCARD, "wild");
    pci_staged_card_set(3, "concrete");
    ASSERT_TRUE(strcmp(pci_staged_card_get(PCI_STAGED_WILDCARD), "wild") == 0);
    ASSERT_TRUE(strcmp(pci_staged_card_get(3), "concrete") == 0);
    // "" clears.
    pci_staged_card_set(3, "");
    ASSERT_TRUE(pci_staged_card_get(3) == NULL);

    // Keyed options round-trip and clear.
    pci_staged_option_set(1, "video_mode", "640x480");
    ASSERT_TRUE(strcmp(pci_staged_option_get(1, "video_mode"), "640x480") == 0);
    ASSERT_TRUE(pci_staged_option_get(1, "nope") == NULL);
    pci_staged_option_set(1, "video_mode", "");
    ASSERT_TRUE(pci_staged_option_get(1, "video_mode") == NULL);

    pci_staged_clear_all();
    ASSERT_TRUE(pci_staged_card_get(PCI_STAGED_WILDCARD) == NULL);
}

TEST(test_slot_walk) {
    config_t *cfg = test_cfg();
    static const pci_slot_decl_t slots[] = {
        {.slot = 1, .kind = PCI_SLOT_SOCKET, .label = "A1", .bus = 0, .device = 13, .int_line = 23},
        {.slot = 2, .kind = PCI_SLOT_SOCKET, .label = "B1", .bus = 0, .device = 14, .int_line = 24},
        {.slot = 3, .kind = PCI_SLOT_SOCKET, .label = "C1", .bus = 0, .device = 15, .int_line = 25},
        {0},
    };
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *bus = pci_bus_create(root, "test", 0);
    pci_init(root, slots);

    // A socket ships empty unless something is staged, so nothing is seated
    // and the declarations are still visible.
    g_factory_calls = 0;
    pci_staged_clear_all();
    pci_seat_slots(root, NULL);
    ASSERT_EQ_INT(g_factory_calls, 0);
    ASSERT_TRUE(pci_slot_decl_get(root, 2)->int_line == 24);
    ASSERT_TRUE(pci_slot_decl_get(root, 9) == NULL);
    ASSERT_TRUE(pci_bus_cfg_read(bus, 13, 0, PCI_CFG_ID) == 0xFFFFFFFFu);
    pci_root_delete(root);
}

// pci_bus_is_populated is the whole question behind "which bridge decodes
// $90000000 on a 9500" (tnt_bandit_claim_memory): the two claimants never
// coexist on real hardware, so the model breaks the tie on whether the bus
// that would otherwise want it actually seated anything.
TEST(test_bus_population) {
    config_t *cfg = test_cfg();
    pci_root_t *root = pci_root_create(cfg);
    pci_bus_t *empty = pci_bus_create(root, "empty", 0);
    pci_bus_t *seated = pci_bus_create(root, "seated", 1);
    device_reset();

    ASSERT_TRUE(!pci_bus_is_populated(empty));
    ASSERT_TRUE(!pci_bus_is_populated(seated));
    pci_bus_add_device(seated, &g_dev, 13);
    ASSERT_TRUE(pci_bus_is_populated(seated));
    // A device on one bus must not make its sibling look populated — the
    // decision is per-bus, and getting that wrong would hand the window to
    // both bridges at once.
    ASSERT_TRUE(!pci_bus_is_populated(empty));
    // A NULL bus is "not populated" rather than a crash: a family asks this
    // about a bus its board may not have created at all.
    ASSERT_TRUE(!pci_bus_is_populated(NULL));
    pci_root_delete(root);
}

int main(void) {
    RUN(test_header_assembly);
    RUN(test_command_masking);
    RUN(test_bar_sizing);
    RUN(test_rom_bar);
    RUN(test_bar_map_transitions);
    RUN(test_absent_devices_read_all_ones);
    RUN(test_window_dispatch_and_faults);
    RUN(test_fixed_region_sparse_decode);
    RUN(test_fixed_region_contiguous);
    RUN(test_slot_interrupts);
    RUN(test_card_fits_socket);
    RUN(test_staged_precedence_and_consumption);
    RUN(test_slot_walk);
    RUN(test_bus_population);
    fprintf(stderr, "pci: all tests passed\n");
    return 0;
}
