// The ADB keyboard queue's overflow policy.
//
// Register 0 reports a key release as bit 7 of the keycode, so WHICH byte the
// queue discards when it fills is not a detail: a dropped release leaves the
// guest holding a modifier it will never see let go, and this side's
// kbd_pressed[] has already moved on, so the two halves disagree from then on.
// With Shift stuck, every later character arrives shifted -- in a digits-only
// field the digits come through as `!@#$%^&*()` and are discarded without a
// sound, which reads exactly like software rejecting the input.  That is a real
// afternoon lost to a wrong diagnosis, which is why this suite exists.
//
// The queue is drained the way a guest drains it: Talk Register 0 at the
// keyboard's address, through the same dispatch the IOP path uses.

#include "adb.h"
#include "machine_profile.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_EQ_HEX(a, b)                                                                                            \
    do {                                                                                                               \
        unsigned _a = (unsigned)(a), _b = (unsigned)(b);                                                               \
        if (_a != _b) {                                                                                                \
            fprintf(stderr, "[FAIL] %s:%d: %s != %s ($%02X != $%02X)\n", __FILE__, __LINE__, #a, #b, _a, _b);          \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

// --- The environment adb.c reaches into -------------------------------------
//
// None of it is needed to exercise the queue: no VIA (the transport is the IOP
// path here), no scheduler (nothing in this suite waits), and no object model.
// adb_init takes a NULL VIA and a NULL scheduler, and the guards in adb.c mean
// these are never called on the paths below -- they exist for the linker.

void via_input(via_t *via, int port, int pin, bool value) {
    (void)via;
    (void)port;
    (void)pin;
    (void)value;
}
void via_input_sr(via_t *via, uint8_t byte) {
    (void)via;
    (void)byte;
}
uint8_t via_read_sr(via_t *via) {
    (void)via;
    return 0;
}
void via_cancel_pending_shift(via_t *via) {
    (void)via;
}

event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)s;
    (void)cb;
    (void)source;
    (void)data;
    (void)cycles;
    (void)ns;
    return NULL;
}
void scheduler_new_event_type(struct scheduler *restrict s, const char *source_name, void *source,
                              const char *event_name, event_callback_t cb) {
    (void)s;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)cb;
}
void remove_event(struct scheduler *restrict s, event_callback_t cb, void *source) {
    (void)s;
    (void)cb;
    (void)source;
}
double scheduler_time_ns(struct scheduler *restrict s) {
    (void)s;
    return 0.0;
}

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
void object_set_label(struct object *o, const char *label) {
    (void)o;
    (void)label;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}

int system_input_key(const char *key, bool down) {
    (void)key;
    (void)down;
    return 0;
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
value_t val_err(const char *fmt, ...) {
    (void)fmt;
    value_t v = {0};
    return v;
}

int debug_mac_resolve_ascii(char c, bool *shift) {
    (void)c;
    (void)shift;
    return -1;
}

#define KEY_SHIFT 0x38u
#define KEY_A     0x00u

// Talk Register 0 at the keyboard's address: the command byte a guest sends to
// collect pending key transitions.
static uint8_t talk_r0_cmd(adb_t *adb) {
    return (uint8_t)((adb_keyboard_address(adb) << 4) | 0x0Cu);
}

// Drain the whole queue, returning the bytes in order.  ADB hands back two key
// transitions per Talk, with $FF in an unused half.
static int drain(adb_t *adb, uint8_t *out, int max) {
    int n = 0;
    for (int guard = 0; guard < 512 && n < max; guard++) {
        uint8_t reply[8];
        int len = 0;
        if (!adb_iop_transact(adb, talk_r0_cmd(adb), NULL, 0, reply, &len) || len < 2)
            break;
        bool got = false;
        for (int i = 0; i < len && n < max; i++) {
            if (reply[i] == 0xFFu)
                continue;
            out[n++] = reply[i];
            got = true;
        }
        if (!got)
            break;
    }
    return n;
}

// The invariant that matters is not that presses and releases balance -- a
// dropped press legitimately leaves its release unpaired -- but that the guest
// is not left HOLDING anything when the stream runs out.  `also_held` is a key
// known to be down before this stream began.
static void assert_nothing_left_held(const uint8_t *bytes, int n, int also_held) {
    int held[128];
    memset(held, 0, sizeof(held));
    if (also_held >= 0)
        held[also_held] = 1;
    for (int i = 0; i < n; i++) {
        int key = bytes[i] & 0x7F;
        if (bytes[i] & 0x80u)
            held[key]--;
        else
            held[key]++;
    }
    for (int k = 0; k < 128; k++)
        if (held[k] > 0) {
            fprintf(stderr, "[FAIL] key $%02X is left held when the stream ends\n", k);
            exit(1);
        }
}

// ============================================================================
TEST(release_survives_overflow) {
    // The shape that actually loses a release, and the one that cost an
    // afternoon: the guest COLLECTS the press, then falls behind while the
    // queue fills, and the release is still waiting when the overflow comes.
    //
    // Enqueueing everything and draining at the end does not reproduce it --
    // both the old policy and the new one keep the newest bytes, so the press
    // and the release go together. The press has to be delivered first.
    adb_t *adb = adb_init(NULL, NULL, NULL);
    ASSERT_TRUE(adb != NULL);

    uint8_t bytes[2048];

    // 1. Shift goes down and the guest collects it: from here the guest holds
    //    Shift and only a release will let it go.
    adb_keyboard_event(adb, key_down, (int)KEY_SHIFT);
    int n = drain(adb, bytes, (int)sizeof(bytes));
    ASSERT_EQ_HEX(n, 1);
    ASSERT_EQ_HEX(bytes[0], KEY_SHIFT);

    // 2. Shift comes up -- but the guest is busy, and a long burst of typing
    //    piles in behind the release without anything being collected.
    adb_keyboard_event(adb, key_up, (int)KEY_SHIFT);
    for (int i = 0; i < 400; i++) {
        adb_keyboard_event(adb, key_down, (int)KEY_A);
        adb_keyboard_event(adb, key_up, (int)KEY_A);
    }

    // 3. The guest catches up.  Whatever else was lost, the release must be in
    //    there: without it the guest holds Shift for good, and every character
    //    after this arrives shifted.
    n = drain(adb, bytes, (int)sizeof(bytes));
    bool saw_shift_up = false;
    for (int i = 0; i < n; i++)
        if (bytes[i] == (KEY_SHIFT | 0x80u))
            saw_shift_up = true;
    if (!saw_shift_up) {
        fprintf(stderr, "[FAIL] the Shift release was dropped: the guest is left holding Shift\n");
        exit(1);
    }
    assert_nothing_left_held(bytes, n, KEY_SHIFT);
    adb_delete(adb);
}

// ============================================================================
TEST(a_quiet_queue_keeps_everything) {
    // The policy must not disturb the ordinary case: well inside the queue's
    // capacity, every transition arrives, in order.
    adb_t *adb = adb_init(NULL, NULL, NULL);
    ASSERT_TRUE(adb != NULL);
    adb_keyboard_event(adb, key_down, (int)KEY_SHIFT);
    adb_keyboard_event(adb, key_down, (int)KEY_A);
    adb_keyboard_event(adb, key_up, (int)KEY_A);
    adb_keyboard_event(adb, key_up, (int)KEY_SHIFT);

    uint8_t bytes[16];
    int n = drain(adb, bytes, (int)sizeof(bytes));
    ASSERT_EQ_HEX(n, 4);
    ASSERT_EQ_HEX(bytes[0], KEY_SHIFT);
    ASSERT_EQ_HEX(bytes[1], KEY_A);
    ASSERT_EQ_HEX(bytes[2], KEY_A | 0x80u);
    ASSERT_EQ_HEX(bytes[3], KEY_SHIFT | 0x80u);
    adb_delete(adb);
}

// ============================================================================
int main(void) {
    RUN(a_quiet_queue_keeps_everything);
    RUN(release_survives_overflow);
    fprintf(stderr, "[ OK ] adb_kbdqueue\n");
    return 0;
}
