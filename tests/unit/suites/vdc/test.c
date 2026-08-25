// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// AV video-in unit test (proposal-av-video-in.md §5.1).
//
// Links the real av/vdc.c + av/civic.c + av/cuda.c against recording stubs
// and pins the contracts from the AV video-in hardware notes:
//
//  1. Cuda pseudo-command $22 wire framing (§2.3): direction from bit 0 of
//     the slave address, subaddress as the first wire byte, multi-byte
//     auto-increment, reads = 4-byte header + data, unknown-slave error
//     packet — all through the real VIA1-side byte protocol.
//  2. The golden open sequence (§8): 25 DMSD + 17 VDC one-byte writes as
//     SendI2CBlock issues them, then VDC $00 := $70, read back through
//     subaddressed reads (the Enabler build's shadow bypass path).
//  3. The synthesized status bytes ($8B / $B9) connected and disconnected.
//  4. The frame engine (§5.6-§5.8, §9.2): 1-5-5-5 packing, greyscale MCT
//     polarity, the VidInSize stride, Enabler 088's $0001FEFF liveness
//     overwrite, VDCClk freeze semantics, interrupt polarity + the
//     VDCClr 0-then-1 ack on the shared slot line, VBL/VDC independence.

#include "av.h"
#include "civic.h"
#include "cuda.h"
#include "object.h"
#include "psc.h"
#include "value.h"
#include "vdc.h"

#include "adb.h"
#include "rtc.h"
#include "via.h"

#include "test_assert.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording stubs
// ============================================================================

// --- PSC slot-interrupt line (shared VBL/VDC, bit 6) ---
static int s_slot_line = -1; // -1 = never driven
void av_psc_slot_source(av_psc_t *psc, int bit, bool active) {
    (void)psc;
    if (bit == AV_PSC_SINT_VBL)
        s_slot_line = active ? 1 : 0;
}

uint32_t cpu_get_pc(cpu_t *cpu) {
    (void)cpu;
    return 0;
}

// --- scheduler: record every (callback, source) so tests can fire the vdc
// field event (and only it) by source match ---
#define MAX_EVENTS 16
static struct {
    void (*cb)(void *, uint64_t);
    void *src;
} s_events[MAX_EVENTS];
static int s_event_count;

event_t *scheduler_new_cpu_event(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)sch;
    (void)data;
    (void)cycles;
    (void)ns;
    for (int i = 0; i < s_event_count; i++) {
        if (s_events[i].src == source && s_events[i].cb == (void (*)(void *, uint64_t))callback)
            return NULL; // re-armed — already recorded
    }
    if (s_event_count < MAX_EVENTS) {
        s_events[s_event_count].cb = (void (*)(void *, uint64_t))callback;
        s_events[s_event_count].src = source;
        s_event_count++;
    }
    return NULL;
}
void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
}
void scheduler_new_event_type(scheduler_t *sch, const char *owner, void *source, const char *name,
                              event_callback_t callback) {
    (void)sch;
    (void)owner;
    (void)source;
    (void)name;
    (void)callback;
}

// Fire every event recorded for `source` once (the vdc field event).
static void fire_events(void *source) {
    for (int i = 0; i < s_event_count; i++)
        if (s_events[i].src == source)
            s_events[i].cb(source, 0);
}

// --- memory plumbing civic touches at install time (not exercised) ---
void mac030_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    (void)page_index;
    (void)host_ptr;
    (void)writable;
}
void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host, uint32_t base, uint32_t size,
                            bool writable) {
    (void)m;
    (void)name;
    (void)host;
    (void)base;
    (void)size;
    (void)writable;
}
void memory_map_add(memory_map_t *m, uint32_t base, uint32_t size, const char *name, memory_interface_t *iface,
                    void *ctx) {
    (void)m;
    (void)base;
    (void)size;
    (void)name;
    (void)iface;
    (void)ctx;
}

// --- VIA1 transport: record TREQ level + every byte Cuda clocks to the SR ---
static int s_treq = 1;
void via_input(via_t *via, int port, int pin, bool value) {
    (void)via;
    if (port == 1 && pin == 3)
        s_treq = value ? 1 : 0;
}
#define SR_MAX 512
static uint8_t s_sr_bytes[SR_MAX];
static int s_sr_count;
void via_input_sr(via_t *via, uint8_t byte) {
    (void)via;
    if (s_sr_count < SR_MAX)
        s_sr_bytes[s_sr_count++] = byte;
}
// Cuda's sync-vs-byte-ack discriminator reads the host SR mode; input
// mode (3) keeps the byte-ack reading for every existing test flow.
uint8_t via_get_acr(const via_t *via) {
    (void)via;
    return 0x0C;
}

// --- RTC / ADB (unused by the I2C paths) ---
uint32_t rtc_get_seconds(const rtc_t *rtc) {
    (void)rtc;
    return 0;
}
void rtc_set_seconds(rtc_t *restrict rtc, uint32_t secs) {
    (void)rtc;
    (void)secs;
}
uint8_t rtc_pram_read(const rtc_t *rtc, uint8_t addr) {
    (void)rtc;
    (void)addr;
    return 0;
}
bool rtc_pram_write(rtc_t *rtc, uint8_t addr, uint8_t value) {
    (void)rtc;
    (void)addr;
    (void)value;
    return true;
}
bool adb_iop_transact(adb_t *adb, uint8_t cmd, const uint8_t *in_data, int in_data_len, uint8_t *out_data,
                      int *out_len) {
    (void)adb;
    (void)cmd;
    (void)in_data;
    (void)in_data_len;
    (void)out_data;
    (void)out_len;
    return false;
}
uint8_t adb_keyboard_address(adb_t *adb) {
    (void)adb;
    return 2;
}
uint8_t adb_mouse_address(adb_t *adb) {
    (void)adb;
    return 3;
}

// --- object model: registration is skipped when object_new returns NULL ---
struct object *machine_object(void) {
    return NULL;
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
void object_set_label(struct object *o, const char *label) {
    (void)o;
    (void)label;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}
value_t val_none(void) {
    value_t v = {0};
    return v;
}
value_t val_bool(bool b) {
    (void)b;
    value_t v = {0};
    return v;
}
value_t val_uint(uint8_t width, uint64_t u) {
    (void)width;
    (void)u;
    value_t v = {0};
    return v;
}
value_t val_str(const char *s) {
    (void)s;
    value_t v = {0};
    return v;
}
value_t val_err(const char *fmt, ...) {
    (void)fmt;
    value_t v = {0};
    return v;
}
void value_free(value_t *v) {
    (void)v;
}

// --- host seams (system.c is not linked; strong test definitions) ---
static bool s_host_connected;
bool gs_video_in_connected(void) {
    return s_host_connected;
}
int gs_video_in_frame(uint8_t *rgba) {
    (void)rgba;
    return -1;
}
static int s_state_pushes;
static bool s_state_last;
void gs_video_in_state(bool active) {
    s_state_pushes++;
    s_state_last = active;
}
int debug_load_png_rgba(const char *filename, int width, int height, uint8_t *out_rgba) {
    (void)filename;
    (void)width;
    (void)height;
    (void)out_rgba;
    return -1;
}

// ============================================================================
// Fixtures
// ============================================================================

static config_t s_cfg;
static av_state_t s_st;

#define CIVIC_BASE 0x50F36000u

// Write one CIVIC 1-bit slot (byte lane 3 carries D[0]).
static void civic_w(uint32_t off, uint8_t bit) {
    av_civic_write(&s_cfg, CIVIC_BASE + off + 3, bit);
}
static uint8_t civic_r(uint32_t off) {
    return av_civic_read(&s_cfg, CIVIC_BASE + off + 3);
}

// Golden register tables from video-in.md §8 (the shipping ROM's open path).
static const uint8_t k_dmsd_defaults[25] = {0x50, 0x30, 0x00, 0xE8, 0xB6, 0xE5, 0x63, 0x00, 0xFE,
                                            0xF0, 0xFE, 0xE0, 0x20, 0x80, 0x78, 0x98, 0x00, 0x20,
                                            0x00, 0x00, 0x34, 0x0A, 0xF4, 0xCE, 0xE9};
static const uint8_t k_vdc_defaults[17] = {0x00, 0x40, 0x80, 0x0C, 0x89, 0xF0, 0xF0, 0x0F, 0xA0,
                                           0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x10};

// Replay the golden open sequence as lastSub+1 one-byte transactions,
// exactly as SendI2CBlock issues them (video-in.md §2.5), then $00 := $70.
static void replay_open_sequence(void) {
    for (uint8_t sub = 0; sub < 25; sub++) {
        uint8_t wire[2] = {sub, k_dmsd_defaults[sub]};
        av_vdc_i2c_write(s_st.vdc, 0x8A, wire, 2);
    }
    for (uint8_t sub = 0; sub < 17; sub++) {
        uint8_t wire[2] = {sub, k_vdc_defaults[sub]};
        av_vdc_i2c_write(s_st.vdc, 0xB8, wire, 2);
    }
    uint8_t start[2] = {0x00, 0x70};
    av_vdc_i2c_write(s_st.vdc, 0xB8, start, 2);
}

// Ungate the frame engine: clock on, 32-bit bus (both CIVIC slots).
static void engine_on(void) {
    civic_w(0x018, 0); // VDCClk = 0 (clock ON)
    civic_w(0x04C, 0); // BusSize = 0 (32-bit)
}

// ============================================================================
// Cuda $22 byte-protocol driver
// ============================================================================
// Reproduces the host side of the transport (cuda.c header comment): shift
// the command bytes, assert TIP after the first, negate TIP to execute,
// then accept the response with TIP + BYTEACK toggles until TREQ rises.

#define PB_TREQ    (1u << 3)
#define PB_BYTEACK (1u << 4)
#define PB_TIP     (1u << 5)

static uint8_t s_pb = PB_TIP | PB_BYTEACK; // idle: TIP + BYTEACK high

static void pb(uint8_t value) {
    s_pb = value;
    av_cuda_via1_pb_input(s_st.cuda, value);
}

// Run one command packet; returns the response byte count in `resp`.
static int cuda_xact(const uint8_t *cmd, int cmd_len, uint8_t *resp, int max) {
    s_sr_count = 0;
    av_cuda_via1_shift_input(s_st.cuda, cmd[0]);
    pb((uint8_t)(s_pb & ~PB_TIP)); // assert TIP
    for (int i = 1; i < cmd_len; i++)
        av_cuda_via1_shift_input(s_st.cuda, cmd[i]);
    pb((uint8_t)(s_pb | PB_TIP)); // negate TIP → Cuda processes + sends attn
    ASSERT_EQ_INT(1, s_sr_count); // the attention byte is in the SR
    pb((uint8_t)(s_pb & ~PB_TIP)); // accept: assert TIP (clocks byte 2)
    // Toggle BYTEACK for each further byte until TREQ rises with the last.
    while (s_treq == 0 && s_sr_count < max)
        pb((uint8_t)(s_pb ^ PB_BYTEACK));
    pb((uint8_t)(s_pb | PB_TIP)); // terminate
    int n = s_sr_count < max ? s_sr_count : max;
    memcpy(resp, s_sr_bytes, (size_t)n);
    return n;
}

// ============================================================================
// Tests
// ============================================================================

// §8: the golden open sequence lands byte-for-byte in both register files,
// read back through the subaddressed-read path (Enabler shadow bypass).
TEST(test_golden_open_sequence) {
    replay_open_sequence();
    uint8_t buf[32];
    int n = av_vdc_i2c_read(s_st.vdc, 0x8B, true, 0, buf, sizeof(buf));
    ASSERT_EQ_INT(25, n);
    ASSERT_EQ_INT(0, memcmp(buf, k_dmsd_defaults, 25));
    n = av_vdc_i2c_read(s_st.vdc, 0xB9, true, 0, buf, sizeof(buf));
    ASSERT_EQ_INT(17, n);
    ASSERT_EQ_INT(0x70, buf[0]); // $00 was rewritten to $70 after the block
    ASSERT_EQ_INT(0, memcmp(buf + 1, k_vdc_defaults + 1, 16));
}

// §3.1 / §4.7: the status bytes, disconnected and connected.
TEST(test_status_bytes) {
    // Disconnected: HLCK = 1 ("no signal"), plus STTC mirroring VTRC ($80).
    ASSERT_EQ_INT(0, av_vdc_set_source(s_st.vdc, "none"));
    uint8_t b = 0;
    ASSERT_EQ_INT(1, av_vdc_i2c_read(s_st.vdc, 0x8B, false, 0, &b, 1));
    ASSERT_EQ_INT(0xC0, b); // VTRC=1 from the open defaults, HLCK=1
    // Connected: locked, 60 Hz, colour.
    ASSERT_EQ_INT(0, av_vdc_set_source(s_st.vdc, "pattern"));
    ASSERT_EQ_INT(1, av_vdc_i2c_read(s_st.vdc, 0x8B, false, 0, &b, 1));
    ASSERT_EQ_INT(0xA1, b); // VTRC | FIDT | CODE, HLCK=0
    ASSERT_TRUE(av_vdc_connected(s_st.vdc));
    // VDC status: version nibble %0001 always; SVP mirrors VPE (= 1 after
    // the open sequence's $00 := $70).
    ASSERT_EQ_INT(1, av_vdc_i2c_read(s_st.vdc, 0xB9, false, 0, &b, 1));
    ASSERT_EQ_INT(0x11, b & 0xFD); // ignore OEF; ID=1, SVP=1
    // The "host" source follows the gs_video_in_connected seam.
    ASSERT_EQ_INT(0, av_vdc_set_source(s_st.vdc, "host"));
    s_host_connected = false;
    ASSERT_TRUE(!av_vdc_connected(s_st.vdc));
    s_host_connected = true;
    ASSERT_TRUE(av_vdc_connected(s_st.vdc));
    av_vdc_set_source(s_st.vdc, "pattern");
}

// §2.3: a write transaction through the real Cuda byte protocol.
TEST(test_cuda_i2c_write) {
    // [pseudoPkt, RdWrIIC, slave $8A, sub $07, data $42] → DMSD[$07] = $42.
    const uint8_t cmd[] = {0x01, 0x22, 0x8A, 0x07, 0x42};
    uint8_t resp[16];
    int n = cuda_xact(cmd, sizeof(cmd), resp, sizeof(resp));
    ASSERT_EQ_INT(4, n); // header-only acknowledgement
    ASSERT_EQ_INT(0x01, resp[1]); // pseudoPkt
    ASSERT_EQ_INT(0x22, resp[3]); // echoed command
    uint8_t b = 0;
    av_vdc_i2c_read(s_st.vdc, 0x8B, true, 0x07, &b, 1);
    ASSERT_EQ_INT(0x42, b);
    // Multi-byte write auto-increments the subaddress.
    const uint8_t cmd2[] = {0x01, 0x22, 0xB8, 0x0C, 0x11, 0x22};
    cuda_xact(cmd2, sizeof(cmd2), resp, sizeof(resp));
    av_vdc_i2c_read(s_st.vdc, 0xB9, true, 0x0C, &b, 1);
    ASSERT_EQ_INT(0x11, b);
    av_vdc_i2c_read(s_st.vdc, 0xB9, true, 0x0D, &b, 1);
    ASSERT_EQ_INT(0x22, b);
    // Restore the keyer-off defaults for later frame tests.
    replay_open_sequence();
}

// §2.3: reads — status byte (no subaddress) and subaddressed register.
TEST(test_cuda_i2c_read) {
    av_vdc_set_source(s_st.vdc, "pattern");
    const uint8_t cmd[] = {0x01, 0x22, 0x8B}; // DMSD status read
    uint8_t resp[16];
    int n = cuda_xact(cmd, sizeof(cmd), resp, sizeof(resp));
    ASSERT_EQ_INT(5, n); // 4-byte header + 1 data byte
    ASSERT_EQ_INT(0xA1, resp[4]); // connected NTSC status
    // Subaddressed read reaches the register file ($B9 sub $01 = XD low).
    const uint8_t cmd2[] = {0x01, 0x22, 0xB9, 0x01};
    n = cuda_xact(cmd2, sizeof(cmd2), resp, sizeof(resp));
    ASSERT_TRUE(n >= 5);
    ASSERT_EQ_INT(0x40, resp[4]); // XD7-0 = $40 (320 with the $04 high bits)
}

// §2.2: only the DMSD and VDC answer; other slaves get an error packet.
TEST(test_cuda_unknown_slave) {
    const uint8_t cmd[] = {0x01, 0x22, 0xA0, 0x00}; // an EEPROM that is not there
    uint8_t resp[16];
    int n = cuda_xact(cmd, sizeof(cmd), resp, sizeof(resp));
    ASSERT_EQ_INT(5, n); // [attn, errorPkt, code, pktType, cmd]
    ASSERT_EQ_INT(0x02, resp[1]); // errorPkt
    ASSERT_EQ_INT(0x01, resp[3]); // for a pseudo packet
    ASSERT_EQ_INT(0x22, resp[4]); // ... command $22
}

// §5.8 + §9.2: the frame engine writes 1-5-5-5 pixels over the liveness
// magic, gated by VDCClk, with one interrupt per field once armed.
TEST(test_frame_engine_1555) {
    replay_open_sequence(); // Apple defaults: 640→320 x 240, even field, RGB
    av_vdc_set_source(s_st.vdc, "pattern");
    uint8_t *vram = av_civic_vram(s_st.civic);

    // Enabler 088's probe: magic at video-in base + 4 must be overwritten.
    vram[0x100804] = 0x00;
    vram[0x100805] = 0x01;
    vram[0x100806] = 0xFE;
    vram[0x100807] = 0xFF;

    // Clock still off (power-on state): a field must NOT be written.
    civic_w(0x04C, 0); // BusSize = 0
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(0xFE, vram[0x100806]);

    engine_on();
    fire_events(s_st.vdc);
    // The magic was overwritten by pattern pixels: the first bar is white,
    // so pixels pack to $7FFF (alpha 0, 8→5 truncation).
    ASSERT_EQ_INT(0x7F, vram[0x100804]);
    ASSERT_EQ_INT(0xFF, vram[0x100805]);
    ASSERT_EQ_INT(0x7F, vram[0x100800]);
    // Row 1 sits at the small stride (VidInSize = 0 → 1024 bytes).
    ASSERT_EQ_INT(0x7F, vram[0x100800 + 1024]);

    // No interrupt yet: VDCEnb is off and VDCClr never armed.
    ASSERT_EQ_INT(1, civic_r(0x008)); // active low — idle

    // The ROM's arm sequence: VDCEnb=0, VDCClr 0 then 1, VDCEnb=1 (§5.6).
    civic_w(0x010, 0);
    civic_w(0x00C, 0);
    civic_w(0x00C, 1);
    civic_w(0x010, 1);
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(0, civic_r(0x008)); // pending (active low)
    ASSERT_EQ_INT(1, s_slot_line); // shared slot line asserted
    // Ack: VDCClr 0 then 1.
    civic_w(0x00C, 0);
    ASSERT_EQ_INT(1, civic_r(0x008));
    ASSERT_EQ_INT(0, s_slot_line);
    civic_w(0x00C, 1);

    // Freeze semantics: clock off stops writes immediately.
    civic_w(0x018, 1); // VDCClk = 1 (off)
    vram[0x100804] = 0xAA;
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(0xAA, vram[0x100804]);
    civic_w(0x018, 0);
}

// §4.2/§4.4: FS=11 greyscale four-per-longword with the MCT polarity, and
// the VidInSize stride switch.
TEST(test_frame_engine_greyscale_stride) {
    replay_open_sequence();
    av_vdc_set_source(s_st.vdc, "pattern");
    engine_on();
    uint8_t *vram = av_civic_vram(s_st.civic);

    // FS=11 (greyscale), keep OF=11 + VPE: $00 := $73; MCT=1 (non-inverse).
    uint8_t w0[2] = {0x00, 0x73};
    av_vdc_i2c_write(s_st.vdc, 0xB8, w0, 2);
    civic_w(0x014, 1); // VidInSize = 1 → 1536-byte rows
    memset(vram + 0x100800, 0x55, 0x2000);
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(0xFF, vram[0x100800]); // white bar → Y = 255, non-inverse
    ASSERT_EQ_INT(0xFF, vram[0x100800 + 1536]); // row 1 at the big stride
    ASSERT_EQ_INT(0x55, vram[0x100800 + 1024]); // NOT at the small stride

    // MCT=0: inverse greyscale.
    uint8_t w10[2] = {0x10, 0x00};
    av_vdc_i2c_write(s_st.vdc, 0xB8, w10, 2);
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(0x00, vram[0x100800]);

    // Restore defaults.
    civic_w(0x014, 0);
    replay_open_sequence();
}

// The VBL and VDC latches share PSC slot bit 6 but ack independently.
TEST(test_shared_line_independence) {
    replay_open_sequence();
    av_vdc_set_source(s_st.vdc, "pattern");
    engine_on();
    // Arm + take a VDC field interrupt.
    civic_w(0x00C, 0);
    civic_w(0x00C, 1);
    civic_w(0x010, 1);
    fire_events(s_st.vdc);
    ASSERT_EQ_INT(1, s_slot_line);
    // Arm + take a VBL (timing generator + VBLEnb + VBLClr re-arm).
    civic_w(0x004, 1); // Enable
    civic_w(0x110, 1); // VBLEnb
    civic_w(0x120, 0);
    civic_w(0x120, 1); // VBLClr re-arm
    fire_events(s_st.civic); // civic frame event latches VBL
    ASSERT_EQ_INT(1, s_slot_line);
    // Ack the VBL: line stays asserted (VDC still pending).
    civic_w(0x120, 0);
    ASSERT_EQ_INT(1, s_slot_line);
    civic_w(0x120, 1);
    // Ack the VDC: line finally drops.
    civic_w(0x00C, 0);
    ASSERT_EQ_INT(0, s_slot_line);
    civic_w(0x00C, 1);
    civic_w(0x010, 0);
    civic_w(0x004, 0);
    civic_w(0x110, 0);
}

// VDCClk transitions notify the host camera lifecycle exactly on change.
TEST(test_clock_gate_notifications) {
    civic_w(0x018, 1); // ensure off
    int base = s_state_pushes;
    civic_w(0x018, 0); // off → on
    ASSERT_EQ_INT(base + 1, s_state_pushes);
    ASSERT_TRUE(s_state_last);
    civic_w(0x018, 0); // no change — no push
    ASSERT_EQ_INT(base + 1, s_state_pushes);
    civic_w(0x018, 1); // on → off
    ASSERT_EQ_INT(base + 2, s_state_pushes);
    ASSERT_TRUE(!s_state_last);
}

int main(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_st, 0, sizeof(s_st));
    s_cfg.machine_context = &s_st;
    s_st.psc = (av_psc_t *)&s_st; // any non-NULL handle will do
    s_st.civic = av_civic_init(&s_cfg, NULL);
    ASSERT_TRUE(s_st.civic != NULL);
    s_st.vdc = av_vdc_init(&s_cfg, NULL);
    ASSERT_TRUE(s_st.vdc != NULL);
    s_st.cuda = av_cuda_init((struct via *)&s_st, NULL, NULL, NULL, NULL, /*mode3_clock=*/false);
    ASSERT_TRUE(s_st.cuda != NULL);
    av_cuda_attach_vdc(s_st.cuda, s_st.vdc);

    RUN(test_golden_open_sequence);
    RUN(test_status_bytes);
    RUN(test_cuda_i2c_write);
    RUN(test_cuda_i2c_read);
    RUN(test_cuda_unknown_slave);
    RUN(test_frame_engine_1555);
    RUN(test_frame_engine_greyscale_stride);
    RUN(test_shared_line_independence);
    RUN(test_clock_gate_notifications);

    fprintf(stderr, "vdc: all tests passed\n");
    return 0;
}
