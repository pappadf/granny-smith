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

// Only its address is ever used: the stack hands it back to the three SCC
// functions below.
static int g_fake_scc_storage;
#define FAKE_SCC ((scc_t *)(void *)&g_fake_scc_storage)

static scc_frame_fn g_sink;
static void *g_sink_ctx;

#define WIRE_MAX 512
typedef struct {
    size_t len;
    uint8_t b[1100];
} frame_t;
static frame_t g_wire[WIRE_MAX];
static int g_nwire;
static int g_pending_cts = -1; // node whose RTS awaits the guest's CTS

void scc_set_frame_sink(scc_t *scc, scc_frame_fn fn, void *context) {
    ASSERT_TRUE(scc == FAKE_SCC);
    g_sink = fn;
    g_sink_ctx = fn ? context : NULL;
}

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len) {
    ASSERT_TRUE(scc == FAKE_SCC);
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
// No test restores a checkpoint yet; appletalk_init is always called without
// one.  The functions exist because appletalk.c links against them.

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp, (void)data, (void)size, (void)tag, (void)file, (void)line;
    ASSERT_TRUE(!"no test reads a checkpoint");
}

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp, (void)data, (void)size, (void)tag, (void)file, (void)line;
}

// --- harness API ---------------------------------------------------------------

void link_boot(void) {
    memset(g_ev, 0, sizeof g_ev);
    memset(g_reg, 0, sizeof g_reg);
    g_nreg = 0;
    g_now_ns = 0;
    wire_clear();
    appletalk_init(&g_sched, FAKE_SCC, NULL);
}

void link_delete(void) {
    appletalk_delete();
    ASSERT_TRUE(g_sink == NULL);
    ASSERT_EQ_INT(0, sched_pending());
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
