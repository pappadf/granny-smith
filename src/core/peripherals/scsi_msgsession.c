// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The initiator-side message conversation shared by MESH and the 53C8xx
// SCRIPTS engine.  See scsi_msgsession.h for why it exists and where the
// capability numbers come from.

#include "scsi_msgsession.h"

#include <string.h>

// SCSI-2 message codes we act on.  Everything else is a single-byte message we
// have nothing to say about.
#define MSG_EXTENDED 0x01u
#define MSG_REJECT   0x07u
#define EXT_SDTR     0x01u
#define EXT_WDTR     0x03u

void scsi_msg_reset(scsi_msgsession_t *s) {
    if (s)
        memset(s, 0, sizeof(*s));
}

bool scsi_msg_collect(scsi_msgsession_t *s, uint8_t byte) {
    if (!s || s->out_len >= SCSI_MSG_OUT_MAX)
        return false;
    s->out[s->out_len++] = byte;
    return true;
}

bool scsi_msg_pending(const scsi_msgsession_t *s) {
    return s && s->in_rd < s->in_n;
}

bool scsi_msg_next(scsi_msgsession_t *s, uint8_t *out) {
    if (!scsi_msg_pending(s))
        return false;
    *out = s->in[s->in_rd++];
    return true;
}

// Queue an EXTENDED MESSAGE reply: 01 <len> <code> <params...>.
static void queue_ext(scsi_msgsession_t *s, uint8_t code, const uint8_t *params, uint8_t nparams) {
    s->in[0] = MSG_EXTENDED;
    s->in[1] = (uint8_t)(nparams + 1); // the code counts toward the length
    s->in[2] = code;
    for (uint8_t i = 0; i < nparams; i++)
        s->in[3 + i] = params[i];
    s->in_n = (uint8_t)(3 + nparams);
    s->in_rd = 0;
}

// Queue a bare single-byte reply.
static void queue_byte(scsi_msgsession_t *s, uint8_t msg) {
    s->in[0] = msg;
    s->in_n = 1;
    s->in_rd = 0;
}

void scsi_msg_complete(scsi_msgsession_t *s, const scsi_msg_caps_t *caps, scsi_msg_result_t *result) {
    memset(result, 0, sizeof(*result));
    if (!s || !caps)
        return;

    for (uint8_t i = 0; i < s->out_len;) {
        uint8_t b = s->out[i];

        if (b & 0x80u) { // IDENTIFY and its family: nothing to answer
            result->identify = true;
            i++;
            continue;
        }
        if (b != MSG_EXTENDED) {
            if (b == MSG_REJECT)
                result->rejected = true;
            i++;
            continue;
        }

        // EXTENDED: 01 <len> <code> <len-1 parameters>.  Both bounds are
        // checked before the length byte is trusted.
        if ((uint32_t)i + 2u > s->out_len || (uint32_t)i + 2u + s->out[i + 1] > s->out_len) {
            result->incomplete = true; // the rest is still on its way
            return; // keep what we have and wait
        }
        uint8_t len = s->out[i + 1];
        uint8_t code = s->out[i + 2];

        if (code == EXT_SDTR && len == 3) {
            // Answer within what the part can do.  A LARGER period is a SLOWER
            // bus, so the floor is a maximum on the number; the offset is a
            // depth, so it is a ceiling.  Agreeing to anything faster or
            // deeper would be describing a chip we are not emulating.
            uint8_t period = s->out[i + 3];
            uint8_t offset = s->out[i + 4];
            if (period < caps->min_period)
                period = caps->min_period;
            if (offset > caps->max_offset)
                offset = caps->max_offset;
            result->sdtr = true;
            result->period = period;
            result->offset = offset;
            uint8_t params[2] = {period, offset};
            queue_ext(s, EXT_SDTR, params, 2);
        } else if (code == EXT_WDTR && len == 2) {
            // A narrow part must SAY it is narrow.  Answering 8-bit is the
            // agreement SCSI-2 expects; dropping the message silently, which
            // MESH used to do, leaves the initiator waiting for a reply that
            // never comes.
            uint8_t width = (caps->wide && s->out[i + 3]) ? 1u : 0u;
            result->wdtr = true;
            result->wide = width != 0;
            queue_ext(s, EXT_WDTR, &width, 1);
        } else {
            // An extended message we do not implement.  SCSI-2 6.6.2: a target
            // that does not support an extended message rejects it, rather
            // than leaving the initiator to time out.
            queue_byte(s, MSG_REJECT);
        }
        i = (uint8_t)(i + 2 + len);
    }

    s->out_len = 0; // the whole stream was understood; start the next one clean
}
