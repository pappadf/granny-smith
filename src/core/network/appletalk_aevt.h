// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_aevt.h
// Apple events: the AETF wire codec, the text authoring grammar, and the
// `appletalk.aevt` surface that sends events to guest applications and
// collects the ones they send us.
//
// Coding reference: docs/core/network/ppc_appleevents.md — §5.2 for the
// flattened stream, §5.4 for lists and records, §6.1 for the V_MAP form and
// §6.2 for the text grammar.  Nothing here reaches for an outside source.
//
// The codec half is pure: bytes and values in, values and bytes out, no
// transport and no globals, so tests/unit/suites/aevt/ exercises it with no
// emulator at all.

#ifndef APPLETALK_AEVT_H
#define APPLETALK_AEVT_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Forward declarations ===
struct object;

// === Wire constants (ppc_appleevents.md §5.2, Appendix B) ===================

#define AEVT_SIGNATURE   "aevt"
#define AEVT_VERSION     0x00010001u
#define AEVT_META_END    ";;;;"
#define AEVT_DIRECT_OBJ  "----"
#define AEVT_KEY_ERRN    "errn"
#define AEVT_KEY_ERRS    "errs"
#define AEVT_REPLY_ID    "ansr"
#define AEVT_HEADER_SIZE 12 // signature + version + meta terminator
#define AEVT_MAX_STREAM  32768 // largest event we will build or accept

// === Codec ==================================================================

// Decode an AETF stream into the event map of §6.1.  The class and ID are not
// in the stream (§5.3) — they come from the message framing.  Returns a
// V_ERROR describing the first inconsistency if the stream is malformed;
// guest data is untrusted, so this never reads past `len`.
value_t aevt_decode(const char *class4, const char *id4, const uint8_t *stream, int len);

// Encode an event map back to an AETF stream.  Returns the byte count written
// to `out`, or -1 with the reason in `err`.  Lists and records are emitted
// unfactored (§5.4), and descriptors we do not decode round-trip through
// their `hex` form, so decode→encode of a captured event is byte-exact.
int aevt_encode(const value_t *event, uint8_t *out, int out_max, char *err, size_t err_len);

// Parse the text grammar of §6.2 into an event map.  Returns V_ERROR on a
// syntax error, with the offset and what was expected.
value_t aevt_parse_text(const char *text, char *err, size_t err_len);

// Render an event map back to text form.  Heap string, caller frees.
char *aevt_render_text(const value_t *event);

// Read the four-character class and ID out of an event map.
bool aevt_event_codes(const value_t *event, char class4[5], char id4[5]);

// The `errn` parameter of a reply, or 0 when it carries none (§5.5).
int64_t aevt_reply_errn(const value_t *event);

// Set one attribute in an event's meta section (§5.2), taking ownership of
// `leaf`.  Used to mark an outgoing event reply-requested.
bool aevt_set_attr(value_t *event, const char *key, value_t leaf);

// === Object model / lifecycle ==============================================

void atalk_aevt_init(void);
void atalk_aevt_shutdown(void);
void atalk_aevt_install_objects(struct object *parent);
void atalk_aevt_remove_objects(void);

// Durable configuration, the only part of this layer a checkpoint carries
// (ppc_appleevents.md §7): sessions, connections and the events collection
// are volatile client state and are dropped on restore.
typedef struct {
    bool enabled;
    char port_name[33];
    char auto_reply[256];
} atalk_aevt_config_t;

void atalk_aevt_get_config(atalk_aevt_config_t *out);
void atalk_aevt_set_config(const atalk_aevt_config_t *in);

// Drop every event, inbox entry and counter (checkpoint restore, machine
// teardown).
void atalk_aevt_reset_transient_state(void);

// Delivery hook, called by the PPC session layer when a high-level event
// arrives: either the reply to a pending send, or a new inbox entry.
void atalk_aevt_deliver(uint16_t session_id, const char *sender, const char *class4, const char *id4,
                        uint32_t return_id, bool is_reply, const uint8_t *stream, int len);

#endif // APPLETALK_AEVT_H
