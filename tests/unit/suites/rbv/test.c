// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// RBV sound-interrupt unit test (proposal-sound-support-all-models §6).
//
// Links the real rbv.c and asc.c against recording stubs and pins:
//
//  1. RvIFR/RvIER semantics for the sound source — the set/clr-via-bit-7
//     convention the shared OS code uses (VIAEnableSoundInts writes $90,
//     VIADisableSoundInts writes $10), bit 7 of IFR reading as
//     "any enabled interrupt pending", IER bit 7 reading 0, and the
//     VIA-spaced register aliases (Rv2IFR $1A03 / Rv2IER $1C13).
//  2. The full ASC→RBV interrupt chain as wired on the IIci/IIsi:
//     FIFO half-empty crossing → asc_set_irq_handler sink →
//     rbv_set_snd_irq (RvIFR bit 4) → combined machine IRQ; reading the
//     ASC's ascFifoInt clears the flags and releases the chain.

#include "asc.h"
#include "audio_out.h"
#include "object.h"
#include "rbv.h"
#include "sound_surface.h"
#include "test_assert.h"
#include "value.h"

#include <stdint.h>
#include <string.h>

// RBV register offsets (native + VIA-spaced aliases)
#define RV_IFR       0x003
#define RV_IER       0x013
#define RV_IFR_ALIAS 0x1A03
#define RV_SINT      0x002
#define RV_SENB      0x012
#define RV_IER_ALIAS 0x1C13

// ASC register offsets
#define R_MODE    0x801
#define R_FIFOINT 0x804

// ============================================================================
// Recording stubs (same shape as the asc suite)
// ============================================================================

// --- scheduler: capture the ASC producer event for manual ticking ---
static void (*s_cb)(void *, uint64_t);
static void *s_cb_src;

event_t *scheduler_new_cpu_event(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)sch;
    (void)data;
    (void)cycles;
    (void)ns;
    s_cb = (void (*)(void *, uint64_t))callback;
    s_cb_src = source;
    return NULL;
}
void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
    s_cb = NULL;
}
void scheduler_forget_source(scheduler_t *sch, void *source) {
    (void)sch;
    (void)source;
}
void scheduler_new_event_type(scheduler_t *sch, const char *source_name, void *source, const char *event_name,
                              event_callback_t callback) {
    (void)sch;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)callback;
}

static void tick(int n) {
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(s_cb != NULL);
        s_cb(s_cb_src, 0);
    }
}

// --- VIA (referenced by asc.c's CB1 adapter; unused in these tests) ---
void via_input_c(via_t *via, int port, int c, bool value) {
    (void)via;
    (void)port;
    (void)c;
    (void)value;
}

// --- audio_out sink: discard ---
void audio_out_open(uint32_t src_rate_hz, int channels) {
    (void)src_rate_hz;
    (void)channels;
}
void audio_out_push(const int16_t *frames, int nframes, int vol_0_7) {
    (void)frames;
    (void)nframes;
    (void)vol_0_7;
}
void audio_out_set_rate(uint32_t src_rate_hz) {
    (void)src_rate_hz;
}
struct object *audio_out_capture_attach(struct object *parent) {
    (void)parent;
    return NULL;
}
void audio_out_capture_detach(void) {}
value_t audio_out_match_value(const char *golden_wav) {
    (void)golden_wav;
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

// --- object model / values: inert ---
// asc.c presents machine.sound through the shared surface (sound_surface.h);
// this suite links asc.c but not the surface, so it is stubbed inert like the
// object model below.  asc_init already handles a NULL object.
struct object *sound_object_new(const sound_surface_t *s) {
    (void)s;
    return NULL;
}
void sound_object_delete(struct object *o) {
    (void)o;
}

// The object model used to be stubbed inert here.  It is the real thing
// now: rbv.c builds a machine.rbv node at init (05-chipsets-irq F-26), and
// a suite that stubs object_new to return NULL cannot tell a node that
// works from one that was never built.  Only machine_object() stays a stub
// (support/stub_machine_object.c) -- there is no machine here to parent to,
// and object_attach(NULL, child) is a no-op.

// --- memory map: unused (both devices get map == NULL) ---
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    (void)iface;
    (void)device;
}
void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                       void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    (void)iface;
    (void)device;
}

// ============================================================================
// Harness
// ============================================================================

static rbv_t *g_rbv;
static const memory_interface_t *g_rbv_if;
static int g_irq_rises, g_irq_falls;
static bool g_irq_state;

// Machine combined-IRQ recorder (what iici_rbv_irq would drive to IPL 2)
static void machine_irq(void *ctx, bool active) {
    (void)ctx;
    if (active && !g_irq_state)
        g_irq_rises++;
    if (!active && g_irq_state)
        g_irq_falls++;
    g_irq_state = active;
}

static uint8_t rrd(uint32_t addr) {
    return g_rbv_if->read_uint8(g_rbv, addr);
}
static void rwr(uint32_t addr, uint8_t v) {
    g_rbv_if->write_uint8(g_rbv, addr, v);
}

static void fresh_rbv(void) {
    if (g_rbv)
        rbv_delete(g_rbv);
    g_irq_rises = 0;
    g_irq_falls = 0;
    g_irq_state = false;
    g_rbv = rbv_init(RBV_VARIANT_IICI, NULL);
    ASSERT_TRUE(g_rbv != NULL);
    g_rbv_if = rbv_get_memory_interface(g_rbv);
    rbv_set_irq_callback(g_rbv, machine_irq, NULL);
}

// ============================================================================
// 1. RvIFR / RvIER sound-bit semantics
// ============================================================================

TEST(test_ier_set_clr_and_aliases) {
    fresh_rbv();

    // VIAEnableSoundInts: write $90 (bit 7 = set, bit 4 = RvSndIRQEn)
    rwr(RV_IER, 0x90);
    ASSERT_EQ_INT(rrd(RV_IER), 0x10); // bit 7 reads 0; bit 4 enabled

    // Set/clr is bitwise: enabling another source must not disturb bit 4
    rwr(RV_IER, 0x81); // enable RvSCSIDRQ too
    ASSERT_EQ_INT(rrd(RV_IER), 0x11);

    // VIADisableSoundInts: write $10 (bit 7 = clear, bit 4)
    rwr(RV_IER, 0x10);
    ASSERT_EQ_INT(rrd(RV_IER), 0x01); // sound gone, SCSIDRQ kept

    // The VIA-spaced aliases decode to the same registers
    rwr(RV_IER_ALIAS, 0x90);
    ASSERT_EQ_INT(rrd(RV_IER), 0x11);
    ASSERT_EQ_INT(rrd(RV_IER_ALIAS), 0x11);
    ASSERT_EQ_INT(rrd(RV_IFR_ALIAS), rrd(RV_IFR));
}

TEST(test_snd_flag_and_combined_irq) {
    fresh_rbv();

    // Source asserts while disabled: flag visible, no machine IRQ
    rbv_set_snd_irq(g_rbv, true);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x10); // RvSndIRQ, bit 7 clear (not enabled)
    ASSERT_EQ_INT(g_irq_rises, 0);

    // Enable ($90): combined IRQ fires; IFR bit 7 = any-enabled-pending
    rwr(RV_IER, 0x90);
    ASSERT_EQ_INT(g_irq_rises, 1);
    ASSERT_TRUE(g_irq_state);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x90);

    // Source deasserts (level input): flag and combined IRQ drop
    rbv_set_snd_irq(g_rbv, false);
    ASSERT_EQ_INT(g_irq_falls, 1);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x00);

    // Disable ($10) with the source asserted: flag stays, IRQ masked
    rbv_set_snd_irq(g_rbv, true);
    ASSERT_EQ_INT(g_irq_rises, 2);
    rwr(RV_IER, 0x10);
    ASSERT_EQ_INT(g_irq_falls, 2);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x10); // flag visible, bit 7 clear
}

// ============================================================================
// 2. The ASC→RBV chain (IIci/IIsi wiring)
// ============================================================================

// The machine-side adapter, byte-identical to iici_asc_irq / iisi_asc_irq
static void asc_to_rbv(void *ctx, bool active) {
    rbv_set_snd_irq((rbv_t *)ctx, active);
}

TEST(test_asc_to_rbv_chain) {
    fresh_rbv();
    rwr(RV_IER, 0x90); // sound interrupts enabled

    s_cb = NULL;
    asc_t *asc = asc_init(NULL, (scheduler_t *)0x1, NULL);
    ASSERT_TRUE(asc != NULL);
    asc_set_irq_handler(asc, asc_to_rbv, g_rbv);
    const memory_interface_t *aif = asc_get_memory_interface(asc);

    // FIFO mode; fill channel A to the half threshold
    aif->write_uint8(asc, R_MODE, 1);
    for (int i = 0; i < 512; i++)
        aif->write_uint8(asc, 0x000, 0x80);
    ASSERT_EQ_INT(g_irq_rises, 0);

    // One drain tick crosses below 512: half-empty latches and propagates
    // ASC → RvSndIRQ → combined machine IRQ
    tick(1);
    ASSERT_EQ_INT(g_irq_rises, 1);
    ASSERT_TRUE(g_irq_state);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x90); // any-pending | RvSndIRQ

    // Servicing the ASC (read ascFifoInt, read-clears) releases the chain
    ASSERT_EQ_INT(aif->read_uint8(asc, R_FIFOINT), 0x01);
    ASSERT_EQ_INT(g_irq_falls, 1);
    ASSERT_EQ_INT(rrd(RV_IFR), 0x00);

    asc_delete(asc);
}

// ============================================================================

// ============================================================================
// 4. RvIFR writes clear the RvIRQ0 latch (2026-09-03 code review, 05 F-33)
// ============================================================================

// slot_pending bit 6 (RvIRQ0, the built-in video frame interrupt) is a LATCH:
// asserted by the vblank, and otherwise cleared only by reading RvSInt.  It
// feeds RvAnySlot.  The RvIFR write path used to discard the written value
// entirely, so a driver acknowledging the frame interrupt through RvIFR --
// or through the Rv2IFR alias, which is the path the shared VIA2/RBV OS code
// takes -- never cleared it, and the machine took a level-2 interrupt
// continuously.
TEST(test_rvifr_write_clears_rvirq0_latch) {
    fresh_rbv();

    // Enable RvIRQ0 as a slot source, and RvAnySlot as an interrupt.
    rwr(RV_SENB, 0x80 | (1u << 6));
    rwr(RV_IER, 0x80 | 0x02); // RvAnySlot = IFR bit 1

    rbv_assert_slot_irq(g_rbv, 0); // slot 0 = built-in video = RvIRQ0
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) != 0);
    ASSERT_TRUE(g_irq_state);

    // Acknowledge through RvIFR: bit 7 = 0 selects clear, RvAnySlot names it.
    rwr(RV_IFR, 0x02);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) == 0);
    ASSERT_TRUE(!g_irq_state);

    // The Rv2IFR alias must behave identically -- it is the one the shared
    // OS path actually uses.
    rbv_assert_slot_irq(g_rbv, 0);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) != 0);
    rwr(RV_IFR_ALIAS, 0x02);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) == 0);

    // Reading RvSInt remains a valid way to clear it.
    rbv_assert_slot_irq(g_rbv, 0);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) != 0);
    (void)rrd(RV_SINT);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) == 0);

    // A NuBus slot source is level, not latched: an RvIFR write must NOT
    // clear it, or a real pending card interrupt would be lost.
    rwr(RV_SENB, 0x80 | 0x01); // enable RvIRQ1
    rbv_assert_slot_irq(g_rbv, 1);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) != 0);
    rwr(RV_IFR, 0x02);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) != 0); // still asserted by the card
    rbv_clear_slot_irq(g_rbv, 1);
    ASSERT_TRUE((rrd(RV_IFR) & 0x02) == 0);
}

int main(void) {
    RUN(test_ier_set_clr_and_aliases);
    RUN(test_snd_flag_and_combined_irq);
    RUN(test_asc_to_rbv_chain);
    RUN(test_rvifr_write_clears_rvirq0_latch);
    fprintf(stderr, "rbv: all tests passed\n");
    return 0;
}
