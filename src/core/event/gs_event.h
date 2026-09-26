// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_event.h -- events from the core.
//
// The one way the core tells the outside that something happened on its
// own: a mode ended, a breakpoint hit, a checkpoint was saved.  Emitters
// call gs_event_emit from the emulator thread with a small JSON object;
// the platform decides delivery.  In the browser em_main.c writes the
// event as a record on the mailbox's event ring (mailbox.h, EVT_STATE /
// EVT_NOTIFY / EVT_LOG); a build with nowhere to deliver keeps the weak
// default, which drops the event.  Emitting never blocks and never runs
// guest code.
//
// Payloads are documented where they are emitted; the shapes the page
// consumes are listed in docs/guide/web.md ("Events from the core").

#ifndef GS_EVENT_H
#define GS_EVENT_H

#include <stdint.h>

typedef enum gs_event_kind {
    GS_EVENT_STATE = 1, // the machine's run state changed: {"event":"mode_ended",...}
    GS_EVENT_NOTIFY = 2, // something the UI shows: a media change, a checkpoint saved
    GS_EVENT_LOG = 3, // a log line
} gs_event_kind_t;

// Emits one event whose payload is the JSON object `json`.  Weak: the
// platform overrides it with its delivery; the default drops the event.
void gs_event_emit(gs_event_kind_t kind, const char *json);

// printf-style gs_event_emit.  The formatted payload is bounded by
// GS_EVENT_MAX bytes; a longer one is dropped.
#define GS_EVENT_MAX 1024
void gs_event_emitf(gs_event_kind_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// The client whose request the emulator thread is serving right now, or 0
// when it is not serving one (the tick itself, a signal handler, headless
// script mode).  A mode started while a request is served belongs to that
// client (scheduler.h).  Weak: the platform's mailbox supplies it.
uint32_t gs_current_client(void);

#endif // GS_EVENT_H
