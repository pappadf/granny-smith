// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// VIA control-line unit test (code review 2026-09-03, 05-chipsets-irq
// F-01/F-02).  Links the real via.c against recording stubs and pins the PCR
// CA2/CB2 mode field against the Rockwell R6522 datasheet:
//
//   Figure 11 (PCR), CA2 field = PCR bits 3,2,1 / CB2 field = bits 7,6,5:
//     000 input, negative active edge        010 input, positive active edge
//     001 INDEPENDENT interrupt input, neg   011 INDEPENDENT interrupt input, pos
//     1xx output modes
//
//   Figure 29 (IFR) note: "if the CA2/CB2 control in the PCR is selected as
//   'independent' interrupt input, then reading or writing the output register
//   ORA/ORB will NOT clear the flag bit.  Instead, the bit must be cleared by
//   writing into the IFR."
//
// Two defects these pin, both live before this suite existed:
//   F-01 the positive-edge selector was read as field bit 2 under a `field < 4`
//        guard, so it was always false and every input mode latched on the
//        negative edge.
//   F-02 a port access cleared CA2/CB2 unconditionally, so the two independent
//        modes lost the flag they exist to hold.

#include "object.h"
#include "test_assert.h"
#include "value.h"
#include "via.h"

#include <stdint.h>
#include <string.h>

// VIA register selects, as via.c decodes them from address lines 9-12.
#define REG_ORB    0
#define REG_ORA    1
#define REG_DDRB   2
#define REG_DDRA   3
#define REG_IFR    13
#define REG_PCR    12
#define REG_ORA_NH 15
#define REG_T1C_L  4
#define REG_T1C_H  5
#define REG_ACR    11

// IFR bits (via.c's private numbering, mirrored here so the test reads as the
// datasheet does).
#define IFR_CA2 0x01
#define IFR_CA1 0x02
#define IFR_CB2 0x08
#define IFR_CB1 0x10
#define IFR_T1  0x40

// PCR CA2/CB2 field values (Figure 11).
#define CA2_INPUT_NEG 0u
#define CA2_INDEP_NEG 1u
#define CA2_INPUT_POS 2u
#define CA2_INDEP_POS 3u

// ============================================================================
// Stubs — via.c reaches the scheduler, the memory map and the object tree.
// ============================================================================

// A settable cycle counter and the most recently armed event, so a test can
// run a timer out and then keep the clock moving past the timeout.
static uint64_t s_cycles;
static event_callback_t s_armed_cb;
static void *s_armed_src;

void scheduler_new_event_type(scheduler_t *sch, const char *source_name, void *source, const char *event_name,
                              event_callback_t callback) {
    (void)sch;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)callback;
}
event_t *scheduler_new_cpu_event(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)sch;
    (void)data;
    (void)ns;
    (void)cycles;
    s_armed_cb = callback;
    s_armed_src = source;
    return NULL;
}
void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
    s_armed_cb = NULL;
}
uint64_t scheduler_cpu_cycles(scheduler_t *sch) {
    (void)sch;
    return s_cycles;
}
// via_t is opaque to callers, so the test reaches its registers the way the
// machine does: through the memory_interface_t via_init registers here.
static memory_interface_t *s_iface;
static void *s_device;
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    s_iface = iface;
    s_device = device;
}
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    (void)cls;
    (void)instance_data;
    (void)name;
    return NULL;
}
void object_delete(struct object *o) {
    (void)o;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void object_detach(struct object *child) {
    (void)child;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}
struct object *machine_object(void) {
    return NULL;
}
value_t val_uint(uint8_t width, uint64_t u) {
    (void)width;
    (void)u;
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

// ============================================================================
// Harness
// ============================================================================

// A non-NULL map token: via_init only passes it straight to memory_map_add,
// which the stub below intercepts to capture the register interface.
static memory_map_t *const s_dummy_map_token = (memory_map_t *)(uintptr_t)1;

// Records the IRQ line so a test can tell a flag set from a flag delivered.
static bool s_irq_level;
static void irq_sink(void *ctx, bool active) {
    (void)ctx;
    s_irq_level = active;
}
static void output_sink(void *ctx, uint8_t port, uint8_t value) {
    (void)ctx;
    (void)port;
    (void)value;
}
static void shift_sink(void *ctx, uint8_t value) {
    (void)ctx;
    (void)value;
}

// The VIA sits on 512-byte register strides (address lines 9-12), even bytes.
static uint32_t reg_addr(int rs) {
    return (uint32_t)rs << 9;
}

static uint8_t rd(via_t *via, int rs) {
    (void)via;
    return s_iface->read_uint8(s_device, reg_addr(rs));
}
static void wr(via_t *via, int rs, uint8_t value) {
    (void)via;
    s_iface->write_uint8(s_device, reg_addr(rs), value);
}

// Fire the armed timer event the way the scheduler does: the event is consumed
// on delivery, so anything still armed afterwards was rearmed by the callback.
static void fire_armed(void) {
    event_callback_t cb = s_armed_cb;
    void *src = s_armed_src;
    ASSERT_TRUE(cb != NULL);
    s_armed_cb = NULL;
    cb(src, 0);
}

// A fresh VIA with the CA2 field (PCR bits 3-1) set to `ca2_mode`.
static via_t *make_via(unsigned ca2_mode) {
    s_irq_level = false;
    // Non-zero: arm_timer stores scheduler_cpu_cycles() as start_timestamp and
    // read_timer reads a zero timestamp as "never armed", so a timer armed at
    // cycle 0 would read as stopped.  Latent in the emulator too (W-02).
    s_cycles = 1000;
    s_armed_cb = NULL;
    via_t *via = via_init(s_dummy_map_token, NULL, 1, "via1", output_sink, shift_sink, irq_sink, NULL, NULL);
    ASSERT_TRUE(via != NULL);
    wr(via, REG_PCR, (uint8_t)(ca2_mode << 1));
    return via;
}

// Drive CA2 through one full low-high-low cycle from its idle-high rest state,
// clearing the IFR before each transition so the caller sees one edge at a time.
static bool edge_sets_ca2(via_t *via, bool going_high) {
    via_input_c(via, 0, 1, !going_high); // park at the opposite level
    wr(via, REG_IFR, 0x7F); // clear every flag
    via_input_c(via, 0, 1, going_high);
    return (rd(via, REG_IFR) & IFR_CA2) != 0;
}

// ============================================================================
// F-01 — which edge is active in each of the four CA2/CB2 input modes
// ============================================================================

// R6522 Figure 11: field bit 1 selects the positive edge, in both the plain
// and the independent input modes.
TEST(test_ca2_input_modes_latch_the_programmed_edge) {
    const struct {
        unsigned mode;
        bool pos;
        const char *name;
    } cases[] = {
        {CA2_INPUT_NEG, false, "000 input negative"      },
        {CA2_INDEP_NEG, false, "001 independent negative"},
        {CA2_INPUT_POS, true,  "010 input positive"      },
        {CA2_INDEP_POS, true,  "011 independent positive"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        via_t *via = make_via(cases[i].mode);

        // The programmed edge sets the flag; the opposite edge does not.
        ASSERT_EQ_INT(edge_sets_ca2(via, cases[i].pos), true);
        ASSERT_EQ_INT(edge_sets_ca2(via, !cases[i].pos), false);

        via_delete(via);
    }
}

// The four output modes (field 1xx) drive CA2, so an input transition must not
// forge an interrupt on a pin the VIA owns.
TEST(test_ca2_output_modes_ignore_input_edges) {
    for (unsigned mode = 4; mode < 8; mode++) {
        via_t *via = make_via(mode);
        ASSERT_EQ_INT(edge_sets_ca2(via, false), false);
        ASSERT_EQ_INT(edge_sets_ca2(via, true), false);
        via_delete(via);
    }
}

// CB2's field is PCR bits 7-5 and behaves identically — the two halves of the
// chip drifted apart once (F-01 was present in both), so pin them together.
TEST(test_cb2_mirrors_ca2_edge_selection) {
    via_t *via = via_init(s_dummy_map_token, NULL, 1, "via1", output_sink, shift_sink, irq_sink, NULL, NULL);
    ASSERT_TRUE(via != NULL);

    wr(via, REG_PCR, (uint8_t)(CA2_INPUT_POS << 5)); // CB2 = 010, positive edge
    via_input_c(via, 1, 1, false);
    wr(via, REG_IFR, 0x7F);
    via_input_c(via, 1, 1, true);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) != 0);

    wr(via, REG_PCR, (uint8_t)(CA2_INPUT_NEG << 5)); // CB2 = 000, negative edge
    via_input_c(via, 1, 1, true);
    wr(via, REG_IFR, 0x7F);
    via_input_c(via, 1, 1, false);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) != 0);

    via_delete(via);
}

// ============================================================================
// F-02 — which modes survive a port access
// ============================================================================

// Figure 29 note: the plain input modes clear CA2 on an ORA access; the two
// independent modes hold it until the IFR is written.
TEST(test_port_access_clears_ca2_only_in_dependent_modes) {
    const struct {
        unsigned mode;
        bool pos;
        bool cleared_by_port_access;
    } cases[] = {
        {CA2_INPUT_NEG, false, true },
        {CA2_INPUT_POS, true,  true },
        {CA2_INDEP_NEG, false, false},
        {CA2_INDEP_POS, true,  false},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        via_t *via = make_via(cases[i].mode);

        // Reading ORA (register 1, the handshaked access).
        ASSERT_EQ_INT(edge_sets_ca2(via, cases[i].pos), true);
        (void)rd(via, REG_ORA);
        ASSERT_EQ_INT((rd(via, REG_IFR) & IFR_CA2) == 0, cases[i].cleared_by_port_access);

        // Writing ORA must behave the same way.
        ASSERT_EQ_INT(edge_sets_ca2(via, cases[i].pos), true);
        wr(via, REG_ORA, 0x00);
        ASSERT_EQ_INT((rd(via, REG_IFR) & IFR_CA2) == 0, cases[i].cleared_by_port_access);

        // Writing the IFR clears it in every mode — the escape the note names.
        ASSERT_EQ_INT(edge_sets_ca2(via, cases[i].pos), true);
        wr(via, REG_IFR, IFR_CA2);
        ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA2) == 0);

        via_delete(via);
    }
}

// CA1 has no independent mode — PCR bit 0 selects its edge and nothing else —
// so a port access always clears it, whatever CA2 is doing.
TEST(test_port_access_always_clears_ca1) {
    via_t *via = make_via(CA2_INDEP_NEG); // the mode that holds CA2
    via_input_c(via, 0, 0, true);
    wr(via, REG_IFR, 0x7F);
    via_input_c(via, 0, 0, false); // PCR bit 0 = 0: negative edge
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA1) != 0);

    (void)rd(via, REG_ORA);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA1) == 0);
    via_delete(via);
}

// Register 15 is port A without handshake and clears nothing — the existing
// behaviour F-02's fix must not disturb.
TEST(test_ora_no_handshake_clears_nothing) {
    via_t *via = make_via(CA2_INPUT_NEG);
    ASSERT_EQ_INT(edge_sets_ca2(via, false), true);
    (void)rd(via, REG_ORA_NH);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA2) != 0);
    wr(via, REG_ORA_NH, 0x00);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA2) != 0);
    via_delete(via);
}

// Port B's ORB access clears CB1/CB2 under the same rule.
TEST(test_orb_access_respects_cb2_independent_mode) {
    via_t *via = via_init(s_dummy_map_token, NULL, 1, "via1", output_sink, shift_sink, irq_sink, NULL, NULL);
    ASSERT_TRUE(via != NULL);

    wr(via, REG_PCR, (uint8_t)(CA2_INPUT_NEG << 5)); // CB2 = 000, clears normally
    via_input_c(via, 1, 1, true);
    wr(via, REG_IFR, 0x7F);
    via_input_c(via, 1, 1, false);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) != 0);
    (void)rd(via, REG_ORB);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) == 0);

    wr(via, REG_PCR, (uint8_t)(CA2_INDEP_NEG << 5)); // CB2 = 001, holds
    via_input_c(via, 1, 1, true);
    wr(via, REG_IFR, 0x7F);
    via_input_c(via, 1, 1, false);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) != 0);
    (void)rd(via, REG_ORB);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CB2) != 0);

    via_delete(via);
}

// ============================================================================
// F-29 — T1 one-shot keeps counting after timeout, as T2 already did
// ============================================================================

// R6522 "Timer 1 One-Shot Mode": "When the counter reaches zero, the T1
// interrupt flag will be set... At this time the counter will continue to
// decrement at system clock rate.  This allows the system processor to read
// the contents of the counter to determine the time since interrupt."
// T1 used to freeze at 0xFFFF, which returned that constant forever and so
// defeated the one use the datasheet names.
TEST(test_t1_one_shot_counter_runs_on_after_timeout) {
    via_t *via = make_via(CA2_INPUT_NEG);

    wr(via, REG_ACR, 0x00); // ACR 7:6 = 00 -> one-shot, no PB7 output
    wr(via, REG_T1C_L, 0x10); // latch low
    wr(via, REG_T1C_H, 0x00); // writing the high byte starts the count
    ASSERT_TRUE(s_armed_cb != NULL);

    // Run the counter out and fire the timeout the scheduler would have.
    s_cycles += 0x11;
    fire_armed();
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_T1) != 0);

    // The counter must keep moving, and by the elapsed amount.
    uint16_t a = (uint16_t)((rd(via, REG_T1C_H) << 8) | rd(via, REG_T1C_L));
    s_cycles += 0x20;
    uint16_t b = (uint16_t)((rd(via, REG_T1C_H) << 8) | rd(via, REG_T1C_L));
    ASSERT_TRUE(a != b);
    ASSERT_EQ_INT((uint16_t)(a - b), 0x20);

    via_delete(via);
}

// The flag must not be set a second time by the counter wrapping again: the
// datasheet requires a rewrite of T1C-H first, and scheduling no follow-up
// event is what enforces it.
TEST(test_t1_one_shot_does_not_refire) {
    via_t *via = make_via(CA2_INPUT_NEG);

    wr(via, REG_ACR, 0x00);
    wr(via, REG_T1C_L, 0x10);
    wr(via, REG_T1C_H, 0x00);
    s_cycles += 0x11;
    fire_armed();

    // Acknowledge, then run well past a full 16-bit wrap.
    (void)rd(via, REG_T1C_L); // reading T1C-L clears the T1 flag
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_T1) == 0);
    ASSERT_TRUE(s_armed_cb == NULL); // nothing rearmed
    s_cycles += 0x20000;
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_T1) == 0);

    via_delete(via);
}

// Free-run (ACR 7:6 = 01) must still rearm — the fix must not flatten the two
// modes into one.
TEST(test_t1_free_run_rearms) {
    via_t *via = make_via(CA2_INPUT_NEG);

    wr(via, REG_ACR, 0x40); // ACR 7:6 = 01 -> continuous interrupts
    wr(via, REG_T1C_L, 0x10);
    wr(via, REG_T1C_H, 0x00);
    ASSERT_TRUE(s_armed_cb != NULL);

    s_cycles += 0x11;
    fire_armed();
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_T1) != 0);
    ASSERT_TRUE(s_armed_cb != NULL); // rearmed for the next period

    via_delete(via);
}

// ============================================================================
// F-27 — a wider access degrades, it does not abort
// ============================================================================

// The VIA is 8-bit on the upper byte of the bus.  A word or long access is not
// something it answers, but a guest, a `memory.peek width=w` or a memory
// logpoint can issue one -- and on the Plus and the Lisa the VIA is in the
// memory map where they reach it.  These used to be GS_ASSERT(0).
TEST(test_wide_accesses_compose_from_byte_ops) {
    via_t *via = make_via(CA2_INPUT_NEG);

    // DDRA is a plain read/write register with no side effects.
    wr(via, REG_DDRA, 0xAB);
    ASSERT_EQ_INT(rd(via, REG_DDRA), 0xAB);

    // Register select comes from address lines 9-12, so every byte of a wide
    // access at a register's base address selects that SAME register: the even
    // bytes read it, the odd bytes fall to via_read_uint8's odd-address path
    // and read 0 (the VIA drives the upper byte only).  A word read is
    // therefore <reg>,00 and a long read is <reg>,00,<reg>,00.
    ASSERT_EQ_INT(s_iface->read_uint16(s_device, reg_addr(REG_DDRA)), 0xAB00);
    ASSERT_TRUE(s_iface->read_uint32(s_device, reg_addr(REG_DDRA)) == 0xAB00AB00u);

    // A word write puts its high byte in the register and drops the odd byte.
    s_iface->write_uint16(s_device, reg_addr(REG_DDRA), 0xCD00);
    ASSERT_EQ_INT(rd(via, REG_DDRA), 0xCD);

    // A long write lands two even bytes in the same register, so the second
    // one wins -- which is what the bus does, not something to paper over.
    s_iface->write_uint32(s_device, reg_addr(REG_DDRA), 0xEF00A500u);
    ASSERT_EQ_INT(rd(via, REG_DDRA), 0xA5);

    via_delete(via);
}

// ============================================================================
// Power-on state (F-12, fixed in via_init before this suite existed — pinned
// here so it is not silently undone)
// ============================================================================

// All four control lines idle high on the board pull-ups, so the first
// assertion is a falling edge rather than a no-op 0 -> 0 write.
TEST(test_control_lines_idle_high_at_power_on) {
    via_t *via = make_via(CA2_INPUT_NEG);
    wr(via, REG_IFR, 0x7F);

    // One falling edge, with no preparatory high write, must be seen.
    via_input_c(via, 0, 0, false);
    ASSERT_TRUE((rd(via, REG_IFR) & IFR_CA1) != 0);

    via_delete(via);
}

int main(void) {
    RUN(test_ca2_input_modes_latch_the_programmed_edge);
    RUN(test_ca2_output_modes_ignore_input_edges);
    RUN(test_cb2_mirrors_ca2_edge_selection);
    RUN(test_port_access_clears_ca2_only_in_dependent_modes);
    RUN(test_port_access_always_clears_ca1);
    RUN(test_ora_no_handshake_clears_nothing);
    RUN(test_orb_access_respects_cb2_independent_mode);
    RUN(test_t1_one_shot_counter_runs_on_after_timeout);
    RUN(test_t1_one_shot_does_not_refire);
    RUN(test_t1_free_run_rearms);
    RUN(test_wide_accesses_compose_from_byte_ops);
    RUN(test_control_lines_idle_high_at_power_on);
    fprintf(stderr, "All VIA control-line tests passed\n");
    return 0;
}
