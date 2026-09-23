// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// link_harness.c
// The fake SCC, scheduler and checkpoint stream around appletalk.c.  See
// link_harness.h.

#include "link_harness.h"

#include "appletalk.h"
#include "scc.h"
#include "scheduler.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- the SCC ------------------------------------------------------------------

// Only their addresses are ever used: the stack hands one back to the SCC
// functions below.  Two, because a checkpoint load builds a second machine
// while the first is still running (link_load).
static int g_fake_scc_storage[2];
#define FAKE_SCC_N(i) ((scc_t *)(void *)&g_fake_scc_storage[(i)])
#define FAKE_SCC      FAKE_SCC_N(0)

static scc_frame_fn g_sinks[2];
static void *g_sink_ctxs[2];
static int g_machine; // the SCC the guest's frames arrive on

static int scc_index(const scc_t *scc) {
    if (scc == FAKE_SCC_N(0))
        return 0;
    ASSERT_TRUE(scc == FAKE_SCC_N(1));
    return 1;
}
#define g_sink     g_sinks[g_machine]
#define g_sink_ctx g_sink_ctxs[g_machine]

#define WIRE_MAX 512
typedef struct {
    size_t len;
    uint8_t b[1100];
} frame_t;
static frame_t g_wire[WIRE_MAX];
static int g_nwire;
static int g_pending_cts = -1; // node whose RTS awaits the guest's CTS

void scc_set_frame_sink(scc_t *scc, scc_frame_fn fn, void *context) {
    int i = scc_index(scc);
    g_sinks[i] = fn;
    g_sink_ctxs[i] = fn ? context : NULL;
}

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len) {
    ASSERT_TRUE(scc_index(scc) == g_machine);
    if (len == 3 && buf[2] == LLAP_TYPE_RTS)
        g_pending_cts = buf[0];
    if (g_nwire < WIRE_MAX && len <= sizeof g_wire[0].b) {
        g_wire[g_nwire].len = len;
        memcpy(g_wire[g_nwire].b, buf, len);
        g_nwire++;
    }
    return 0;
}

bool scc_sdlc_ready(const scc_t *restrict scc) {
    (void)scc;
    return true;
}

// --- the scheduler ------------------------------------------------------------

struct scheduler {
    int unused;
};
static struct scheduler g_sched;

#define EV_MAX 256
typedef struct {
    bool used;
    event_callback_t cb;
    void *src;
    uint64_t data;
    double when;
} fev_t;
static fev_t g_ev[EV_MAX];
static double g_now_ns;

#define REG_MAX 64
typedef struct {
    char name[48];
    event_callback_t cb;
    void *src;
} reg_t;
static reg_t g_reg[REG_MAX];
static int g_nreg;

void scheduler_new_event_type(struct scheduler *restrict s, const char *source_name, void *source,
                              const char *event_name, event_callback_t cb) {
    ASSERT_TRUE(s == &g_sched);
    char name[48];
    snprintf(name, sizeof name, "%s.%s", source_name, event_name);
    for (int i = 0; i < g_nreg; i++)
        if (g_reg[i].cb == cb && g_reg[i].src == source) {
            snprintf(g_reg[i].name, sizeof g_reg[i].name, "%s", name);
            return;
        }
    ASSERT_TRUE(g_nreg < REG_MAX);
    snprintf(g_reg[g_nreg].name, sizeof g_reg[g_nreg].name, "%s", name);
    g_reg[g_nreg].cb = cb;
    g_reg[g_nreg].src = source;
    g_nreg++;
}

// The real scheduler asserts that a (callback, source) pair was registered
// before it is armed; so does this one.
event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict s, event_callback_t cb, void *source, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)cycles, (void)periodic;
    ASSERT_TRUE(s == &g_sched);
    bool registered = false;
    for (int i = 0; i < g_nreg; i++)
        registered |= g_reg[i].cb == cb && g_reg[i].src == source;
    ASSERT_TRUE(registered);
    for (int i = 0; i < EV_MAX; i++)
        if (!g_ev[i].used) {
            g_ev[i] = (fev_t){true, cb, source, data, g_now_ns + (double)ns};
            return (event_t *)(void *)&g_ev[i];
        }
    ASSERT_TRUE(!"fake scheduler full");
    return NULL;
}

void remove_event(struct scheduler *restrict s, event_callback_t cb, void *source) {
    (void)s;
    for (int i = 0; i < EV_MAX; i++)
        if (g_ev[i].used && g_ev[i].cb == cb && (source == NULL || g_ev[i].src == source))
            g_ev[i].used = false;
}

void remove_event_by_data(struct scheduler *restrict s, event_callback_t cb, void *source, uint64_t data) {
    (void)s;
    for (int i = 0; i < EV_MAX; i++)
        if (g_ev[i].used && g_ev[i].cb == cb && (source == NULL || g_ev[i].src == source) && g_ev[i].data == data)
            g_ev[i].used = false;
}

void scheduler_forget_source(struct scheduler *restrict s, void *source) {
    (void)s;
    for (int i = 0; i < EV_MAX; i++)
        if (g_ev[i].used && g_ev[i].src == source)
            g_ev[i].used = false;
}

bool has_event(struct scheduler *restrict s, event_callback_t cb) {
    (void)s;
    for (int i = 0; i < EV_MAX; i++)
        if (g_ev[i].used && g_ev[i].cb == cb)
            return true;
    return false;
}

double scheduler_time_ns(struct scheduler *restrict s) {
    (void)s;
    return g_now_ns;
}

// Fire every event due at or before `t`, in time order.
static void run_until(double t) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < EV_MAX; i++)
            if (g_ev[i].used && g_ev[i].when <= t && (best < 0 || g_ev[i].when < g_ev[best].when))
                best = i;
        if (best < 0)
            break;
        fev_t e = g_ev[best];
        g_ev[best].used = false;
        if (e.when > g_now_ns)
            g_now_ns = e.when;
        e.cb(e.src, e.data);
    }
    if (t > g_now_ns)
        g_now_ns = t;
}

// --- the checkpoint stream -------------------------------------------------------
//
// A byte stream: appletalk_checkpoint appends, appletalk_init reads back in
// order.  A "failed" stream hands the saved bytes back AND reports the
// checkpoint in error -- the reader's contract says nothing read from it may
// be applied, and this is how a test can tell whether it was.

static int g_cp_storage;
#define FAKE_CP ((checkpoint_t *)(void *)&g_cp_storage)
static uint8_t g_cp_buf[1 << 16];
static size_t g_cp_len, g_cp_pos;
static bool g_cp_fail, g_cp_error;

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag, (void)file, (void)line;
    ASSERT_TRUE(cp == FAKE_CP);
    if (g_cp_pos + size > g_cp_len) {
        g_cp_error = true;
        memset(data, 0, size);
        return;
    }
    memcpy(data, g_cp_buf + g_cp_pos, size);
    g_cp_pos += size;
    if (g_cp_fail)
        g_cp_error = true;
}

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag, (void)file, (void)line;
    ASSERT_TRUE(cp == FAKE_CP);
    ASSERT_TRUE(g_cp_len + size <= sizeof g_cp_buf);
    memcpy(g_cp_buf + g_cp_len, data, size);
    g_cp_len += size;
}

bool checkpoint_has_error(checkpoint_t *cp) {
    (void)cp;
    return g_cp_error;
}

void checkpoint_set_error(checkpoint_t *cp) {
    (void)cp;
    g_cp_error = true;
}

// checkpoint.c's string pair, over the stream above.
void checkpoint_write_string(checkpoint_t *cp, const char *s) {
    uint32_t len = (s && *s) ? (uint32_t)strlen(s) + 1 : 0;
    system_write_checkpoint_data_loc(cp, &len, sizeof(len), NULL, NULL, 0);
    if (len)
        system_write_checkpoint_data_loc(cp, s, len, NULL, NULL, 0);
}

char *checkpoint_read_string(checkpoint_t *cp, uint32_t max, const char *what) {
    (void)what;
    uint32_t len = 0;
    system_read_checkpoint_data_loc(cp, &len, sizeof(len), NULL, NULL, 0);
    if (g_cp_error || len == 0)
        return NULL;
    if (len > max) {
        g_cp_error = true;
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    ASSERT_TRUE(buf != NULL);
    system_read_checkpoint_data_loc(cp, buf, len, NULL, NULL, 0);
    buf[len] = '\0';
    return buf;
}

// --- harness API ---------------------------------------------------------------

void link_boot(void) {
    g_machine = 0;
    memset(g_ev, 0, sizeof g_ev);
    memset(g_reg, 0, sizeof g_reg);
    g_nreg = 0;
    g_now_ns = 0;
    wire_clear();
    appletalk_init(&g_sched, FAKE_SCC, NULL);
}

void link_checkpoint(void) {
    g_cp_len = 0;
    appletalk_checkpoint(FAKE_CP);
    ASSERT_TRUE(g_cp_len > 0);
}

void link_boot_from_checkpoint(bool read_fails) {
    g_machine = 0;
    memset(g_ev, 0, sizeof g_ev);
    memset(g_reg, 0, sizeof g_reg);
    g_nreg = 0;
    g_now_ns = 0;
    wire_clear();
    g_cp_fail = read_fails;
    g_cp_error = false;
    g_cp_pos = 0;
    appletalk_init(&g_sched, FAKE_SCC, FAKE_CP);
}

void link_delete(void) {
    appletalk_delete(FAKE_SCC_N(g_machine));
    ASSERT_TRUE(g_sinks[0] == NULL && g_sinks[1] == NULL);
    ASSERT_EQ_INT(0, sched_pending());
}

void link_load(bool fails) {
    int prev = g_machine, next = 1 - g_machine;
    g_cp_fail = false; // the stack's own record reads fine...
    g_cp_error = false;
    g_cp_pos = 0;
    appletalk_init(&g_sched, FAKE_SCC_N(next), FAKE_CP);
    if (fails) {
        // ...but something later in the checkpoint does not, and the load
        // destroys the machine it was building; the old one keeps running.
        appletalk_delete(FAKE_SCC_N(next));
        g_machine = prev;
    } else {
        appletalk_delete(FAKE_SCC_N(prev));
        g_machine = next;
    }
}

bool link_sink_on(int machine) {
    return g_sinks[machine] != NULL;
}

void guest_frame(const uint8_t *frame, size_t len) {
    ASSERT_TRUE(g_sink != NULL);
    g_sink(g_sink_ctx, frame, len);
}

void guest_ddp(uint8_t src_node, uint8_t dst_sock, uint8_t src_sock, uint8_t ddp_type, const uint8_t *payload,
               size_t plen) {
    uint8_t f[1100];
    size_t dlen = 5 + plen;
    ASSERT_TRUE(3 + dlen <= sizeof f);
    f[0] = HOST_NODE;
    f[1] = src_node;
    f[2] = LLAP_TYPE_DDP_SHORT;
    f[3] = (uint8_t)((dlen >> 8) & 3);
    f[4] = (uint8_t)dlen;
    f[5] = dst_sock;
    f[6] = src_sock;
    f[7] = ddp_type;
    if (plen)
        memcpy(f + 8, payload, plen);
    guest_frame(f, 3 + dlen);
}

void guest_advance_to(double t_ns) {
    while (g_now_ns < t_ns) {
        double step = g_now_ns + 3e5;
        run_until(step < t_ns ? step : t_ns);
        if (g_pending_cts >= 0) {
            uint8_t cts[3] = {HOST_NODE, (uint8_t)g_pending_cts, LLAP_TYPE_CTS};
            g_pending_cts = -1;
            guest_frame(cts, sizeof cts);
        }
    }
}

void guest_idle_until(double t_ns) {
    run_until(t_ns);
}

int wire_count(void) {
    return g_nwire;
}

const uint8_t *wire_frame(int i, size_t *len) {
    if (i < 0 || i >= g_nwire)
        return NULL;
    if (len)
        *len = g_wire[i].len;
    return g_wire[i].b;
}

int wire_count_type(uint8_t dst, uint8_t type) {
    int n = 0;
    for (int i = 0; i < g_nwire; i++)
        n += g_wire[i].len >= 3 && g_wire[i].b[0] == dst && g_wire[i].b[2] == type;
    return n;
}

const uint8_t *wire_last_ddp(uint8_t dst, uint8_t ddp_type, size_t *len) {
    for (int i = g_nwire - 1; i >= 0; i--) {
        const frame_t *f = &g_wire[i];
        if (f->len >= 8 && f->b[0] == dst && f->b[2] == LLAP_TYPE_DDP_SHORT && f->b[7] == ddp_type) {
            if (len)
                *len = f->len;
            return f->b;
        }
    }
    return NULL;
}

int wire_count_atp(uint8_t dst, uint8_t ctl_type, uint8_t user0) {
    int n = 0;
    for (int i = 0; i < g_nwire; i++) {
        const frame_t *f = &g_wire[i];
        if (f->len < 3 + 5 + 8 || f->b[0] != dst || f->b[2] != LLAP_TYPE_DDP_SHORT || f->b[7] != 3 /* ATP */)
            continue;
        if ((f->b[8] & 0xC0) == ctl_type && (user0 == 0xFF || f->b[12] == user0))
            n++;
    }
    return n;
}

void wire_clear(void) {
    g_nwire = 0;
    g_pending_cts = -1;
}

double link_now_ns(void) {
    return g_now_ns;
}

int sched_pending(void) {
    int n = 0;
    for (int i = 0; i < EV_MAX; i++)
        n += g_ev[i].used;
    return n;
}

bool sched_registered(const char *name) {
    for (int i = 0; i < g_nreg; i++)
        if (strcmp(g_reg[i].name, name) == 0)
            return true;
    return false;
}

bool link_sink_installed(void) {
    return g_sink != NULL;
}
