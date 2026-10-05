// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_aevt.h
// Apple events: the AETF wire codec, the text authoring grammar, and the
// `appletalk.aevt` surface that sends events to guest applications and
// collects the ones they send us.
//
// Coding reference: docs/internals/core/network/ppc_appleevents.md — §5.2 for the
// flattened stream, §5.4 for lists and records, §6.1 for the V_MAP form and
// §6.2 for the text grammar.  Nothing here reaches for an outside source.
//
// The codec half is pure: bytes and values in, values and bytes out, no
// transport and no globals, so tests/unit/suites/aevt/ exercises it with no
// emulator at all.

#ifndef APPLETALK_AEVT_H
#define APPLETALK_AEVT_H

#include "appletalk_ppc.h"
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

// Once, when the network comes up: take inbound events and publish the host
// port (PPC's).  Returns the layer's part of the network, its auto-reply,
// which the network owns.
typedef struct aevt_host aevt_host_t;
aevt_host_t *atalk_aevt_init(void);
void atalk_aevt_install_objects(struct object *parent);

// The Apple-event layer's part of a machine's connection (atalk_conn_t): the
// events sent to that Mac, its inbox and their counters.  The network serves
// the plugged-in connection's link; atalk_aevt_plug with NULL, when the
// connection is unplugged, drops every event and inbox entry it held.
typedef struct aevt_link aevt_link_t;

aevt_link_t *atalk_aevt_link_new(void);
void atalk_aevt_link_free(aevt_link_t *link);
void atalk_aevt_plug(aevt_link_t *link);

// Delivery hook, called by the PPC session layer when a high-level event
// arrives on `session`: either the reply to a pending send, or a new inbox
// entry, answered on the session it came in on.
void atalk_aevt_deliver(ppc_session_t *session, const char *sender, const char *class4, const char *id4,
                        uint32_t return_id, bool is_reply, const uint8_t *stream, int len);

#endif // APPLETALK_AEVT_H
