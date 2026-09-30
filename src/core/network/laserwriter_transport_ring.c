// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_transport_ring.c
// The browser transport: the interpreter runs in a Web Worker with its own
// module, and the two sides share a control block and two byte rings in
// the wasm heap (laserwriter_ring_protocol.h).  This side writes OPEN /
// FEED / FINISH / ABANDON / PRINTER_FREE records into the outbound ring
// and, from
// laserwriter_transport_poll(), drains OPENED / OPEN_FAILED / FED /
// FINISHED from the inbound ring into the bridge's callbacks.  It never
// blocks: a record that does not fit fails the request.
//
// The region is allocated on the first open and the platform is asked to
// attach the worker (laserwriter_ring_attach_requested); until it does,
// records accumulate in the ring and the worker consumes them when it
// attaches.  The two platform hooks below are weak no-ops so the build
// links without the browser side (part 2B implements them: the attach
// posts the control block's address to the page, the notify wakes the
// worker with Atomics.notify / emscripten_futex_wake).
//
// Compiles natively too (tests/unit/suites/laserwriter_ring drives it with
// a simulated worker); the atomics are the compiler's builtins.

#include "common.h"
#include "laserwriter_transport.h"

#include "laserwriter_ring_protocol.h"
#include "log.h"
#include "mailbox/mailbox_ring.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("laserwriter");

// ============================================================================
// Constants and Macros
// ============================================================================

// Alignment of the control block inside the allocation.
#define LWRING_ALIGN 64u

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// The transport: the shared region and this side's cursors.
typedef struct {
    laserwriter_transport_callbacks_t cb;
    void *cb_ctx;
    uint8_t *region; // the allocation; NULL until the first open
    volatile uint32_t *ctrl; // the control block (64-byte aligned inside region)
    mbx_ring_t out; // outbound ring (this side writes; OUT_HEAD / OUT_TAIL)
    mbx_ring_t in; // inbound ring (this side reads; IN_HEAD / IN_TAIL)
    uint32_t job_id; // the job the bridge holds (0 = none); other ids' replies are dropped
    uint32_t outstanding; // the LWRING_R_* request awaiting its reply, 0 when none
    uint32_t outstanding_seq; // FEED: its sequence, for a failure report
} ring_state_t;

static ring_state_t g_ring;

// ============================================================================
// Platform hooks (weak defaults)
// ============================================================================

// The platform attaches the interpreter worker to the control block at
// `ctrl_addr` (part 2B: em_main.c posts it to the page, which hands the
// worker the shared memory and the address).  The default does nothing:
// no worker ever attaches and every open times out in the bridge.
__attribute__((weak)) void laserwriter_ring_attach_requested(uintptr_t ctrl_addr) {
    (void)ctrl_addr;
    LOG(1, "laserwriter: no interpreter worker on this platform (ring transport unattached)");
}

// Wakes a worker parked on `addr` (OUT_HEAD after a publish, IN_TAIL
// after a consume).  The default does nothing.
__attribute__((weak)) void laserwriter_ring_notify(volatile uint32_t *addr) {
    (void)addr;
}

// ============================================================================
// Static Helpers
// ============================================================================

// Reads a control word with acquire semantics (the other side's stores
// before its publish are visible after).
static inline uint32_t ring_load(int word) {
    return mbx_load(g_ring.ctrl, word);
}

// Publishes a control word (release: our ring writes precede it).
static inline void ring_store(int word, uint32_t v) {
    mbx_store(g_ring.ctrl, word, v);
}

// Allocates the region and asks the platform to attach the worker.
static bool ring_create(void) {
    if (g_ring.region)
        return true;
    uint32_t ctrl_bytes = LWRING_CTRL_WORDS * 4u;
    uint32_t out_size = LWRING_OUT_BYTES;
    uint32_t in_size = LWRING_IN_BYTES;
    size_t bytes = LWRING_ALIGN + ctrl_bytes + out_size + in_size;
    g_ring.region = (uint8_t *)calloc(1, bytes);
    if (!g_ring.region) {
        LOG(1, "laserwriter: cannot allocate the interpreter ring (%zu bytes)", bytes);
        return false;
    }
    // Align the control block inside the allocation
    uintptr_t base = ((uintptr_t)g_ring.region + (LWRING_ALIGN - 1u)) & ~(uintptr_t)(LWRING_ALIGN - 1u);
    g_ring.ctrl = (volatile uint32_t *)base;
    uint8_t *out = (uint8_t *)(base + ctrl_bytes);
    mbx_ring_init(&g_ring.out, g_ring.ctrl, LWRING_C_OUT_HEAD, LWRING_C_OUT_TAIL, out, out_size);
    mbx_ring_init(&g_ring.in, g_ring.ctrl, LWRING_C_IN_HEAD, LWRING_C_IN_TAIL, out + out_size, in_size);
    for (int i = 0; i < LWRING_CTRL_WORDS; i++)
        g_ring.ctrl[i] = 0;
    g_ring.ctrl[LWRING_C_MAGIC] = LWRING_MAGIC;
    g_ring.ctrl[LWRING_C_VERSION] = LWRING_PROTOCOL_VERSION;
    g_ring.ctrl[LWRING_C_OUT_OFF] = ctrl_bytes;
    g_ring.ctrl[LWRING_C_OUT_SIZE] = out_size;
    g_ring.ctrl[LWRING_C_IN_OFF] = ctrl_bytes + out_size;
    g_ring.ctrl[LWRING_C_IN_SIZE] = in_size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    LOG(2, "laserwriter: interpreter ring allocated (out %u KB, in %u KB); attach requested", out_size >> 10,
        in_size >> 10);
    laserwriter_ring_attach_requested((uintptr_t)base);
    return true;
}

// Reserves `len` bytes (header included; rounded up to the 8-byte record
// rule) for an outbound record; returns the payload pointer or NULL when
// there is no room (the platen policy: a full ring fails the request).
// Nothing is published until ring_publish.
static uint8_t *ring_reserve(uint32_t kind, uint32_t len) {
    uint32_t at = mbx_reserve(&g_ring.out, kind, len);
    return at == UINT32_MAX ? NULL : mbx_payload(&g_ring.out, at);
}

// Publishes everything reserved so far and wakes the worker.
static void ring_publish(void) {
    mbx_publish(&g_ring.out);
    laserwriter_ring_notify(&g_ring.ctrl[LWRING_C_OUT_HEAD]);
}

// Marks a request of `kind` outstanding (the bridge keeps one at a time).
static void ring_issue(uint32_t kind, uint32_t seq) {
    g_ring.outstanding = kind;
    g_ring.outstanding_seq = seq;
}

// The worker is gone: fail the outstanding request the way its answer
// would have, so the bridge takes one path for every failure.
static void ring_fail_outstanding(const char *why) {
    uint32_t job_id = g_ring.job_id;
    uint32_t kind = g_ring.outstanding;
    uint32_t seq = g_ring.outstanding_seq;
    g_ring.outstanding = 0;
    g_ring.job_id = 0;
    LOG(1, "laserwriter: job %u: %s", (unsigned)job_id, why);
    if (kind == LWRING_R_OPEN) {
        if (g_ring.cb.on_open_failed)
            g_ring.cb.on_open_failed(job_id, why, g_ring.cb_ctx);
    } else if (kind == LWRING_R_FEED) {
        if (g_ring.cb.on_fed)
            g_ring.cb.on_fed(job_id, seq, LASERWRITER_FEED_FAILED, 0, NULL, 0, NULL, 0, false, g_ring.cb_ctx);
    } else if (kind == LWRING_R_FINISH) {
        laserwriter_finish_result_t res;
        memset(&res, 0, sizeof(res));
        res.outcome = LASERWRITER_OUTCOME_FAILED;
        res.error_name = why;
        res.offending = "";
        if (g_ring.cb.on_finished)
            g_ring.cb.on_finished(job_id, &res, g_ring.cb_ctx);
    }
}

// Reads one text field of `len` bytes at `p`, capped by what the record
// holds, into a NUL-terminated scratch copy (the callbacks take C strings).
static const char *ring_text(const uint8_t *p, uint32_t len, char *scratch, size_t cap) {
    if (len >= cap)
        len = (uint32_t)(cap - 1);
    memcpy(scratch, p, len);
    scratch[len] = '\0';
    return scratch;
}

// Dispatches one inbound record (payload at `p`, `words` header words
// followed by the text fields).  A record for another job is dropped.
static void ring_dispatch(uint32_t kind, const uint8_t *p, uint32_t payload_len) {
    uint32_t job_id = payload_len >= 4 ? RD_LE32(p) : 0;
    if (job_id != g_ring.job_id || g_ring.job_id == 0) {
        LOG(3, "laserwriter: ring record kind %u for job %u dropped (holding job %u)", (unsigned)kind, (unsigned)job_id,
            (unsigned)g_ring.job_id);
        return;
    }
    char scratch[256];
    switch (kind) {
    case LWRING_R_OPENED: {
        uint32_t flags = payload_len >= 4 * LWRING_OPENED_WORDS ? RD_LE32(p + 4 * LWRING_OPENED_FLAGS) : 0;
        g_ring.outstanding = 0;
        if (g_ring.cb.on_opened)
            g_ring.cb.on_opened(job_id, (flags & LWRING_OPENED_F_RESTARTED) != 0, g_ring.cb_ctx);
        break;
    }
    case LWRING_R_OPEN_FAILED: {
        uint32_t text_len = RD_LE32(p + 4 * LWRING_OPEN_FAILED_TEXT);
        const uint8_t *text = p + 4 * LWRING_OPEN_FAILED_WORDS;
        if (4 * LWRING_OPEN_FAILED_WORDS + text_len > payload_len)
            text_len = payload_len - 4 * LWRING_OPEN_FAILED_WORDS;
        g_ring.outstanding = 0;
        g_ring.job_id = 0;
        if (g_ring.cb.on_open_failed)
            g_ring.cb.on_open_failed(job_id, ring_text(text, text_len, scratch, sizeof(scratch)), g_ring.cb_ctx);
        break;
    }
    case LWRING_R_FED: {
        uint32_t seq = RD_LE32(p + 4 * LWRING_FED_SEQ);
        uint32_t status = RD_LE32(p + 4 * LWRING_FED_STATUS);
        uint32_t pages = RD_LE32(p + 4 * LWRING_FED_PAGES);
        uint32_t reply_len = RD_LE32(p + 4 * LWRING_FED_REPLY_LEN);
        uint32_t error_len = RD_LE32(p + 4 * LWRING_FED_ERROR_LEN);
        uint32_t flags = RD_LE32(p + 4 * LWRING_FED_FLAGS);
        const uint8_t *reply = p + 4 * LWRING_FED_WORDS;
        const uint8_t *errors = reply + LWRING_PAD4(reply_len);
        if (4 * LWRING_FED_WORDS + LWRING_PAD4(reply_len) + LWRING_PAD4(error_len) > payload_len) {
            LOG(1, "laserwriter: job %u: malformed FED record (lengths exceed the record)", (unsigned)job_id);
            reply_len = 0;
            error_len = 0;
        }
        laserwriter_feed_status_t st = status == LWRING_FEED_WAITING ? LASERWRITER_FEED_WAITING
                                       : status == LWRING_FEED_ENDED ? LASERWRITER_FEED_ENDED
                                                                     : LASERWRITER_FEED_FAILED;
        g_ring.outstanding = 0;
        if (g_ring.cb.on_fed)
            g_ring.cb.on_fed(job_id, seq, st, pages, reply, reply_len, errors, error_len,
                             (flags & LWRING_FED_F_TRUNCATED) != 0, g_ring.cb_ctx);
        break;
    }
    case LWRING_R_FINISHED: {
        uint32_t outcome = RD_LE32(p + 4 * LWRING_FINISHED_OUTCOME);
        uint32_t pages = RD_LE32(p + 4 * LWRING_FINISHED_PAGES);
        uint32_t name_len = RD_LE32(p + 4 * LWRING_FINISHED_ERRNAME_LEN);
        uint32_t offend_len = RD_LE32(p + 4 * LWRING_FINISHED_OFFEND_LEN);
        uint32_t reply_len = RD_LE32(p + 4 * LWRING_FINISHED_REPLY_LEN);
        uint32_t error_len = RD_LE32(p + 4 * LWRING_FINISHED_ERROR_LEN);
        const uint8_t *name = p + 4 * LWRING_FINISHED_WORDS;
        const uint8_t *offend = name + LWRING_PAD4(name_len);
        const uint8_t *reply = offend + LWRING_PAD4(offend_len);
        const uint8_t *errors = reply + LWRING_PAD4(reply_len);
        if (4 * LWRING_FINISHED_WORDS + LWRING_PAD4(name_len) + LWRING_PAD4(offend_len) + LWRING_PAD4(reply_len) +
                LWRING_PAD4(error_len) >
            payload_len) {
            LOG(1, "laserwriter: job %u: malformed FINISHED record (lengths exceed the record)", (unsigned)job_id);
            name_len = offend_len = reply_len = error_len = 0;
        }
        char name_buf[128];
        char offend_buf[128];
        laserwriter_finish_result_t res;
        memset(&res, 0, sizeof(res));
        res.outcome = outcome == LWRING_OUTCOME_OK       ? LASERWRITER_OUTCOME_OK
                      : outcome == LWRING_OUTCOME_ERROR  ? LASERWRITER_OUTCOME_ERROR
                      : outcome == LWRING_OUTCOME_BUDGET ? LASERWRITER_OUTCOME_BUDGET
                                                         : LASERWRITER_OUTCOME_FAILED;
        res.pages = pages;
        res.error_name = ring_text(name, name_len, name_buf, sizeof(name_buf));
        res.offending = ring_text(offend, offend_len, offend_buf, sizeof(offend_buf));
        res.reply = reply;
        res.reply_len = reply_len;
        res.errors = errors;
        res.errors_len = error_len;
        // No PDF here: the worker posted it to the main thread
        g_ring.outstanding = 0;
        g_ring.job_id = 0;
        if (g_ring.cb.on_finished)
            g_ring.cb.on_finished(job_id, &res, g_ring.cb_ctx);
        break;
    }
    default:
        LOG(1, "laserwriter: unknown ring record kind %u (%u bytes) ignored", (unsigned)kind, (unsigned)payload_len);
        break;
    }
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// The ring has no guest-time timers: the bridge's poll tick drains it.
void laserwriter_transport_init(void) {}

void laserwriter_transport_set_callbacks(const laserwriter_transport_callbacks_t *callbacks, void *ctx) {
    if (callbacks)
        g_ring.cb = *callbacks;
    else
        memset(&g_ring.cb, 0, sizeof(g_ring.cb));
    g_ring.cb_ctx = ctx;
}

bool laserwriter_transport_open(uint32_t printer_id, uint32_t job_id, const laserwriter_job_config_t *cfg) {
    if (!cfg || printer_id == 0 || !ring_create())
        return false;
    if (g_ring.outstanding || g_ring.job_id) {
        LOG(1, "laserwriter: job %u: open while job %u is held", (unsigned)job_id, (unsigned)g_ring.job_id);
        return false;
    }
    // Size the identity block: NUL-terminated key/value pairs
    uint32_t id_bytes = 0;
    for (size_t i = 0; i < cfg->identity_len; i++)
        id_bytes += (uint32_t)strlen(cfg->identity[i].key) + 1u + (uint32_t)strlen(cfg->identity[i].value) + 1u;
    uint32_t body = 4u * LWRING_OPEN_WORDS + id_bytes + (uint32_t)cfg->prelude_len;
    uint32_t len = LWRING_PAD8(LWRING_HDR_BYTES + body);
    if (len > g_ring.out.size / 2u) {
        LOG(1, "laserwriter: job %u: open record (%u bytes) too large for the ring", (unsigned)job_id, (unsigned)len);
        return false;
    }
    uint8_t *p = ring_reserve(LWRING_R_OPEN, len);
    if (!p) {
        LOG(1, "laserwriter: job %u: no room in the interpreter ring for open", (unsigned)job_id);
        return false;
    }
    WR_LE32(p + 4 * LWRING_OPEN_JOB, job_id);
    WR_LE32(p + 4 * LWRING_OPEN_PRINTER, printer_id);
    WR_LE32(p + 4 * LWRING_OPEN_COMPRESS, cfg->compress ? 1u : 0u);
    WR_LE32(p + 4 * LWRING_OPEN_EMBED, cfg->embed_all_fonts ? 1u : 0u);
    WR_LE32(p + 4 * LWRING_OPEN_BUDGET_L, (uint32_t)cfg->step_budget);
    WR_LE32(p + 4 * LWRING_OPEN_BUDGET_H, (uint32_t)(cfg->step_budget >> 32));
    WR_LE32(p + 4 * LWRING_OPEN_PASSWORD, (uint32_t)cfg->server_password);
    WR_LE32(p + 4 * LWRING_OPEN_ID_COUNT, (uint32_t)cfg->identity_len);
    WR_LE32(p + 4 * LWRING_OPEN_ID_BYTES, id_bytes);
    WR_LE32(p + 4 * LWRING_OPEN_PRELUDE, (uint32_t)cfg->prelude_len);
    uint8_t *q = p + 4 * LWRING_OPEN_WORDS;
    for (size_t i = 0; i < cfg->identity_len; i++) {
        size_t k = strlen(cfg->identity[i].key) + 1u;
        size_t v = strlen(cfg->identity[i].value) + 1u;
        memcpy(q, cfg->identity[i].key, k);
        q += k;
        memcpy(q, cfg->identity[i].value, v);
        q += v;
    }
    if (cfg->prelude_len)
        memcpy(q, cfg->prelude, cfg->prelude_len);
    g_ring.job_id = job_id;
    ring_issue(LWRING_R_OPEN, 0);
    ring_publish();
    LOG(3, "laserwriter: job %u open queued on printer %u (ring, %u bytes)", (unsigned)job_id, (unsigned)printer_id,
        (unsigned)len);
    return true;
}

bool laserwriter_transport_feed(uint32_t job_id, uint32_t sequence, const uint8_t *bytes, size_t len) {
    if (!g_ring.region || g_ring.job_id != job_id) {
        LOG(1, "laserwriter: job %u: feed with no such job", (unsigned)job_id);
        return false;
    }
    if (len > LWRING_FEED_MAX) {
        LOG(1, "laserwriter: job %u: feed of %zu bytes exceeds one flow quantum", (unsigned)job_id, len);
        return false;
    }
    if (g_ring.outstanding)
        return false;
    uint32_t rec = LWRING_PAD8(LWRING_HDR_BYTES + 4u * LWRING_FEED_WORDS + len);
    uint8_t *p = ring_reserve(LWRING_R_FEED, rec);
    if (!p) {
        LOG(1, "laserwriter: job %u: no room in the interpreter ring for feed seq=%u", (unsigned)job_id,
            (unsigned)sequence);
        return false;
    }
    WR_LE32(p + 4 * LWRING_FEED_JOB, job_id);
    WR_LE32(p + 4 * LWRING_FEED_SEQ, sequence);
    WR_LE32(p + 4 * LWRING_FEED_LEN, (uint32_t)len);
    if (len)
        memcpy(p + 4 * LWRING_FEED_WORDS, bytes, len);
    ring_issue(LWRING_R_FEED, sequence);
    ring_publish();
    return true;
}

bool laserwriter_transport_finish(uint32_t job_id, const char *title) {
    if (!g_ring.region || g_ring.job_id != job_id) {
        LOG(1, "laserwriter: job %u: finish with no such job", (unsigned)job_id);
        return false;
    }
    if (g_ring.outstanding)
        return false;
    // The title names the download on the page; cut to the protocol's cap
    uint32_t title_len = title ? (uint32_t)strlen(title) : 0u;
    if (title_len > LWRING_TITLE_MAX)
        title_len = LWRING_TITLE_MAX;
    uint8_t *p = ring_reserve(LWRING_R_FINISH, LWRING_HDR_BYTES + 4u * LWRING_FINISH_WORDS + LWRING_PAD4(title_len));
    if (!p) {
        LOG(1, "laserwriter: job %u: no room in the interpreter ring for finish", (unsigned)job_id);
        return false;
    }
    WR_LE32(p + 4 * LWRING_FINISH_JOB, job_id);
    WR_LE32(p + 4 * LWRING_FINISH_TITLE_LEN, title_len);
    if (title_len)
        memcpy(p + 4 * LWRING_FINISH_WORDS, title, title_len);
    ring_issue(LWRING_R_FINISH, 0);
    ring_publish();
    return true;
}

void laserwriter_transport_abandon(uint32_t job_id) {
    if (!g_ring.region || g_ring.job_id != job_id)
        return;
    // Whatever the worker answers for this job from now on is dropped
    g_ring.job_id = 0;
    g_ring.outstanding = 0;
    uint8_t *p = ring_reserve(LWRING_R_ABANDON, LWRING_HDR_BYTES + 4u * LWRING_ABANDON_WORDS);
    if (!p) {
        // The worker will meet the next OPEN with this job still alive; it frees on its own open
        LOG(1, "laserwriter: job %u: no room in the interpreter ring for abandon", (unsigned)job_id);
        return;
    }
    WR_LE32(p + 4 * LWRING_ABANDON_JOB, job_id);
    ring_publish();
}

void laserwriter_transport_printer_free(uint32_t printer_id) {
    // No region, no worker: no printer was ever created
    if (!g_ring.region || printer_id == 0)
        return;
    uint8_t *p = ring_reserve(LWRING_R_PRINTER_FREE, LWRING_HDR_BYTES + 4u * LWRING_PRINTER_FREE_WORDS);
    if (!p) {
        // The worker keeps the printer until the page closes; nothing else goes wrong
        LOG(1, "laserwriter: no room in the interpreter ring to free printer %u", (unsigned)printer_id);
        return;
    }
    WR_LE32(p + 4 * LWRING_PRINTER_FREE_ID, printer_id);
    ring_publish();
    LOG(3, "laserwriter: printer %u free queued (ring)", (unsigned)printer_id);
}

void laserwriter_transport_poll(void) {
    if (!g_ring.region)
        return;
    bool consumed = false;
    for (;;) {
        uint32_t head = ring_load(LWRING_C_IN_HEAD);
        mbx_rec_t rec;
        int got = mbx_next(&g_ring.in, head, &rec);
        if (got == 0)
            break;
        if (got < 0) {
            // The worker's framing is broken: nothing after this can be trusted
            LOG(1, "laserwriter: inbound ring corrupt; worker lost");
            ring_store(LWRING_C_STATUS, LWRING_STATUS_LOST);
            mbx_abandon(&g_ring.in, head);
            break;
        }
        if (rec.kind != LWRING_R_PAD)
            ring_dispatch(rec.kind, mbx_rec_payload(&g_ring.in, &rec), rec.len - LWRING_HDR_BYTES);
        mbx_consume(&g_ring.in, &rec);
        consumed = true;
    }
    if (consumed)
        laserwriter_ring_notify(&g_ring.ctrl[LWRING_C_IN_TAIL]);
    // A worker that gave up fails whatever is waiting on it
    if (g_ring.outstanding && ring_load(LWRING_C_STATUS) == LWRING_STATUS_LOST)
        ring_fail_outstanding("interpreter worker lost");
}

const char *laserwriter_transport_name(void) {
    return "ring";
}
