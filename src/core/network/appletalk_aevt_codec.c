// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_aevt_codec.c
// The Apple event codec: AETF byte stream ⇄ V_MAP ⇄ text form.
//
// Coding reference: docs/core/network/ppc_appleevents.md §5.2 (stream
// layout), §5.4 (lists, records and factoring), §5.6 (descriptor types),
// §6.1 (the map form) and §6.2 (the text grammar).  Section numbers in the
// comments below refer to that document.
//
// Everything here is pure and defensive: a decode never reads past the
// buffer it was handed, and a malformed stream yields a V_ERROR naming the
// problem rather than a partial value.  Guest data is untrusted input.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk_aevt.h"

#include "log.h"
#include "value.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ppc");

// ============================================================================
// Constants and Macros
// ============================================================================

// How deep a nested list/record may go before we call the stream hostile.
#define AEVT_MAX_DEPTH 16

// ============================================================================
// Operations — small helpers
// ============================================================================

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// Every stream item is padded to an even offset (§5.2).
static int round_up_even(int n) {
    return (n + 1) & ~1;
}

// Four-character codes travel as four raw bytes and live in the map as
// four-character strings.  Bytes outside printable ASCII are escaped so a
// binary code cannot smuggle a quote into the text form.
static void fourcc_read(const uint8_t *p, char out[5]) {
    for (int i = 0; i < 4; i++)
        out[i] = (char)p[i];
    out[4] = '\0';
}

static void fourcc_write(const char *code, uint8_t out[4]) {
    size_t n = code ? strlen(code) : 0;
    for (int i = 0; i < 4; i++)
        out[i] = (i < (int)n) ? (uint8_t)code[i] : (uint8_t)' ';
}

// ============================================================================
// Operations — decode
// ============================================================================

// A cursor over the stream that refuses to run off the end.
typedef struct {
    const uint8_t *p;
    int len;
    bool bad;
    char why[128];
} rd_t;

static bool rd_fail(rd_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool rd_fail(rd_t *r, const char *fmt, ...) {
    if (!r->bad) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->why, sizeof(r->why), fmt, ap);
        va_end(ap);
        r->bad = true;
    }
    return false;
}

static bool rd_have(rd_t *r, int at, int n) {
    if (at < 0 || n < 0 || at > r->len || n > r->len - at)
        return rd_fail(r, "the stream ends inside a descriptor at offset %d", at);
    return true;
}

static value_t decode_desc(rd_t *r, const char *type, const uint8_t *data, int len, int depth);

// One list or record body: count, prefix size, prefix, then the items (§5.4).
static value_t decode_collection(rd_t *r, const uint8_t *data, int len, bool keyed, int depth) {
    if (len < 8)
        return (rd_fail(r, "a %s descriptor is shorter than its header", keyed ? "record" : "list"), val_none());
    uint32_t count = rd32(data);
    uint32_t prefix = rd32(data + 4);
    if (prefix != 0 && prefix != 4 && prefix < 8)
        return (rd_fail(r, "illegal factoring prefix size %u", (unsigned)prefix), val_none());
    if (prefix > (uint32_t)len - 8)
        return (rd_fail(r, "factoring prefix runs past the descriptor"), val_none());
    if (count > (uint32_t)AEVT_MAX_STREAM)
        return (rd_fail(r, "implausible item count %u", (unsigned)count), val_none());

    char shared_type[5] = "    ";
    int fixed_len = -1;
    if (prefix >= 4)
        fourcc_read(data + 8, shared_type);
    if (prefix >= 8) {
        fixed_len = (int)rd32(data + 12);
        if (fixed_len < 0)
            return (rd_fail(r, "negative factored item length"), val_none());
    }

    int pos = 8 + round_up_even((int)prefix);
    value_map_builder_t *rec = keyed ? val_map_new() : NULL;
    value_t *items = NULL;
    size_t items_len = 0, items_cap = 0;

    for (uint32_t i = 0; i < count && !r->bad; i++) {
        int start = pos;
        char key[5] = "    ";
        if (keyed) {
            if (pos + 4 > len) {
                rd_fail(r, "record item %u has no keyword", (unsigned)i);
                break;
            }
            fourcc_read(data + pos, key);
            pos += 4;
        }
        char type[5];
        if (prefix == 0) {
            if (pos + 4 > len) {
                rd_fail(r, "item %u has no type", (unsigned)i);
                break;
            }
            fourcc_read(data + pos, type);
            pos += 4;
        } else {
            memcpy(type, shared_type, sizeof(type));
        }
        int item_len;
        if (prefix <= 4) {
            if (pos + 4 > len) {
                rd_fail(r, "item %u has no length", (unsigned)i);
                break;
            }
            item_len = (int)rd32(data + pos);
            pos += 4;
        } else {
            item_len = fixed_len;
        }
        if (item_len < 0 || item_len > len - pos) {
            rd_fail(r, "item %u claims %d bytes but only %d remain", (unsigned)i, item_len, len - pos);
            break;
        }
        value_t leaf = decode_desc(r, type, data + pos, item_len, depth + 1);
        if (r->bad) {
            value_free(&leaf);
            break;
        }
        if (keyed) {
            val_map_put(rec, key, leaf);
        } else if (!val_list_push(&items, &items_len, &items_cap, leaf)) {
            rd_fail(r, "out of memory decoding a list");
            break;
        }

        // Advance by the item's own size, rounded to even — except for the
        // packed case, where each item is exactly one byte (§5.4).
        int total = (pos - start) + item_len;
        if (total != 1)
            total = round_up_even(total);
        pos = start + total;
    }

    if (r->bad) {
        if (rec) {
            value_t partial = val_map_finish(rec);
            value_free(&partial);
        }
        for (size_t i = 0; i < items_len; i++)
            value_free(&items[i]);
        free(items);
        return val_none();
    }
    if (keyed)
        return val_map_finish(rec);
    return val_list(items, items_len);
}

// Uppercase hex of a descriptor's raw bytes, the `hex` body of §6.1.
static value_t hex_of(const uint8_t *data, int len) {
    static const char DIGITS[] = "0123456789ABCDEF";
    char *hex = (char *)malloc((size_t)len * 2 + 1);
    if (!hex)
        return val_err("out of memory");
    for (int i = 0; i < len; i++) {
        hex[i * 2] = DIGITS[data[i] >> 4];
        hex[i * 2 + 1] = DIGITS[data[i] & 0x0F];
    }
    hex[len * 2] = '\0';
    value_t v = val_str(hex);
    free(hex);
    return v;
}

// A leaf, by descriptor type (§5.6).  Anything we do not model — and anything
// whose length contradicts its type — is kept as raw bytes, so it re-encodes
// byte for byte and a surprising descriptor is never an error.
static value_t decode_desc(rd_t *r, const char *type, const uint8_t *data, int len, int depth) {
    if (depth > AEVT_MAX_DEPTH)
        return (rd_fail(r, "descriptors nested more than %d deep", AEVT_MAX_DEPTH), val_none());

    value_t body = val_none();
    bool have_body = false;
    bool opaque = false;

    if (!strcmp(type, "TEXT") || !strcmp(type, "cstr") || !strcmp(type, "utxt")) {
        int n = len;
        if (!strcmp(type, "cstr") && n > 0 && data[n - 1] == '\0')
            n--; // the terminator is framing, not content
        char *s = (char *)malloc((size_t)n + 1);
        if (!s)
            return (rd_fail(r, "out of memory"), val_none());
        memcpy(s, data, (size_t)n);
        s[n] = '\0';
        body = val_str(s);
        have_body = true;
        free(s);
    } else if (!strcmp(type, "long") || !strcmp(type, "magn")) {
        opaque = (len != 4);
        if (!opaque) {
            body = val_int((int32_t)rd32(data));
            have_body = true;
        }
    } else if (!strcmp(type, "shor")) {
        opaque = (len != 2);
        if (!opaque) {
            body = val_int((int16_t)rd16(data));
            have_body = true;
        }
    } else if (!strcmp(type, "comp")) {
        opaque = (len != 8);
        if (!opaque) {
            body = val_int((int64_t)(((uint64_t)rd32(data) << 32) | rd32(data + 4)));
            have_body = true;
        }
    } else if (!strcmp(type, "bool")) {
        opaque = (len != 1);
        if (!opaque) {
            body = val_bool(data[0] != 0);
            have_body = true;
        }
    } else if (!strcmp(type, "true") || !strcmp(type, "fals")) {
        opaque = (len != 0); // a zero-length type with a body is not one of ours
    } else if (!strcmp(type, "null") || !strcmp(type, "msng")) {
        opaque = (len != 0); // carries neither data nor hex (§6.1)
    } else if (!strcmp(type, "type") || !strcmp(type, "enum") || !strcmp(type, "sign") || !strcmp(type, "prop") ||
               !strcmp(type, "keyw")) {
        opaque = (len != 4);
        if (!opaque) {
            char code[5];
            fourcc_read(data, code);
            body = val_str(code);
            have_body = true;
        }
    } else if (!strcmp(type, "list")) {
        body = decode_collection(r, data, len, false, depth);
        have_body = true;
    } else if (!strcmp(type, "reco") || !strcmp(type, "obj ") || !strcmp(type, "rang") || !strcmp(type, "insl")) {
        body = decode_collection(r, data, len, true, depth);
        have_body = true;
    } else {
        opaque = true;
    }

    if (r->bad) {
        value_free(&body);
        return val_none();
    }

    value_map_builder_t *b = val_map_new();
    val_map_put(b, "type", val_str(type));
    if (opaque)
        val_map_put(b, "hex", hex_of(data, len));
    else if (have_body)
        val_map_put(b, "data", body);
    else
        value_free(&body);
    return val_map_finish(b);
}

value_t aevt_decode(const char *class4, const char *id4, const uint8_t *stream, int len) {
    rd_t r = {.p = stream, .len = len};
    value_map_builder_t *ev = val_map_new();
    val_map_put(ev, "class", val_str(class4 ? class4 : "????"));
    val_map_put(ev, "id", val_str(id4 ? id4 : "????"));

    value_map_builder_t *attrs = val_map_new();
    int pos = 0;
    bool in_params = false;

    // An event with no parameters may be sent as zero bytes (§5.2).
    if (len > 0) {
        if (len < 8 || memcmp(stream, AEVT_SIGNATURE, 4) != 0) {
            rd_fail(&r, "the stream does not start with an %s header", AEVT_SIGNATURE);
        } else {
            uint32_t version = rd32(stream + 4);
            if (version != AEVT_VERSION)
                rd_fail(&r, "unsupported AETF version 0x%08X", (unsigned)version);
            pos = 8;
        }
    }

    while (!r.bad && pos < len) {
        if (!rd_have(&r, pos, 4))
            break;
        char key[5];
        fourcc_read(stream + pos, key);
        if (!strcmp(key, AEVT_META_END)) {
            if (in_params) {
                rd_fail(&r, "a second meta-section terminator at offset %d", pos);
                break;
            }
            in_params = true;
            pos += 4; // the marker is the one item that is only four bytes
            continue;
        }
        if (!rd_have(&r, pos, 12))
            break;
        char type[5];
        fourcc_read(stream + pos + 4, type);
        int dlen = (int)rd32(stream + pos + 8);
        if (dlen < 0 || !rd_have(&r, pos + 12, dlen))
            break;
        value_t leaf = decode_desc(&r, type, stream + pos + 12, dlen, 0);
        if (r.bad) {
            value_free(&leaf);
            break;
        }
        if (in_params) {
            val_map_put(ev, key, leaf);
        } else {
            val_map_put(attrs, key, leaf);
        }
        pos += round_up_even(12 + dlen);
    }

    if (!r.bad && pos != len && len > 0)
        rd_fail(&r, "%d trailing bytes after the last parameter", len - pos);
    if (!r.bad && len > 0 && !in_params)
        rd_fail(&r, "the stream ends without a '%s' meta-section terminator", AEVT_META_END);

    if (r.bad) {
        // val_map_finish hands back an owning value even on the error path.
        value_t partial_attrs = val_map_finish(attrs);
        value_free(&partial_attrs);
        value_t partial_ev = val_map_finish(ev);
        value_free(&partial_ev);
        return val_err("malformed Apple event: %s", r.why);
    }
    val_map_put(ev, "attrs", val_map_finish(attrs));
    return val_map_finish(ev);
}

bool aevt_set_attr(value_t *event, const char *key, value_t leaf) {
    if (!event || event->kind != V_MAP || !key) {
        value_free(&leaf);
        return false;
    }
    // Rebuild the event map with the attribute merged into `attrs`; maps are
    // immutable once finished, and keeping the key order stable matters
    // because it decides the order attributes are written to the wire.
    value_map_builder_t *out = val_map_new();
    value_map_builder_t *attrs = val_map_new();
    bool had_attrs = false;
    for (size_t i = 0; i < event->map.len; i++) {
        const char *k = event->map.entries[i].key;
        if (!strcmp(k, "attrs")) {
            had_attrs = true;
            const value_t *existing = &event->map.entries[i].val;
            if (existing->kind == V_MAP) {
                for (size_t j = 0; j < existing->map.len; j++)
                    val_map_put(attrs, existing->map.entries[j].key, value_copy(&existing->map.entries[j].val));
            }
        }
    }
    val_map_put(attrs, key, leaf);
    value_t merged = val_map_finish(attrs);

    bool placed = false;
    for (size_t i = 0; i < event->map.len; i++) {
        const char *k = event->map.entries[i].key;
        if (!strcmp(k, "attrs")) {
            val_map_put(out, "attrs", merged);
            placed = true;
            continue;
        }
        val_map_put(out, k, value_copy(&event->map.entries[i].val));
    }
    if (!had_attrs || !placed)
        val_map_put(out, "attrs", merged);

    value_t rebuilt = val_map_finish(out);
    if (val_is_error(&rebuilt)) {
        value_free(&rebuilt);
        return false;
    }
    value_free(event);
    *event = rebuilt;
    return true;
}

int64_t aevt_reply_errn(const value_t *event) {
    // The reply's error number is an ordinary parameter (§5.5); the event
    // object republishes it so a script asserts on it without descending.
    const value_t *leaf = value_map_get(event, AEVT_KEY_ERRN);
    const value_t *data = leaf ? value_map_get(leaf, "data") : NULL;
    if (!data)
        return 0;
    return val_as_i64(data, NULL);
}

// ============================================================================
// Operations — encode
// ============================================================================

// A growable output cursor with a hard ceiling; overflow is an error, never
// a truncation.
typedef struct {
    uint8_t *out;
    int max;
    int pos;
    bool bad;
    char why[128];
} wr_t;

static bool wr_fail(wr_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool wr_fail(wr_t *w, const char *fmt, ...) {
    if (!w->bad) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(w->why, sizeof(w->why), fmt, ap);
        va_end(ap);
        w->bad = true;
    }
    return false;
}

static bool wr_room(wr_t *w, int n) {
    if (n < 0 || w->pos > w->max - n)
        return wr_fail(w, "the event does not fit in %d bytes", w->max);
    return true;
}

static void wr_bytes(wr_t *w, const void *p, int n) {
    if (!wr_room(w, n))
        return;
    if (n > 0)
        memcpy(w->out + w->pos, p, (size_t)n);
    w->pos += n;
}

static void wr_pad_even(wr_t *w) {
    if (w->pos & 1) {
        uint8_t z = 0;
        wr_bytes(w, &z, 1);
    }
}

static void wr_fourcc(wr_t *w, const char *code) {
    uint8_t buf[4];
    fourcc_write(code, buf);
    wr_bytes(w, buf, 4);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void encode_leaf_body(wr_t *w, const value_t *leaf, int depth);

// A descriptor: type, length, data, pad.  The length is backfilled once the
// body is known, which keeps nesting simple.
static void encode_desc(wr_t *w, const value_t *leaf, int depth) {
    const value_t *tv = value_map_get(leaf, "type");
    const char *type = tv ? val_as_str(tv) : NULL;
    if (!type) {
        wr_fail(w, "a descriptor has no type");
        return;
    }
    wr_fourcc(w, type);
    int len_at = w->pos;
    uint8_t zero[4] = {0};
    wr_bytes(w, zero, 4);
    int body_at = w->pos;
    encode_leaf_body(w, leaf, depth);
    if (w->bad)
        return;
    wr32(w->out + len_at, (uint32_t)(w->pos - body_at));
    wr_pad_even(w);
}

// A list or record body, always emitted unfactored (prefix size 0, §5.4):
// legal for every reader, and it keeps the encoder honest about lengths.
static void encode_collection(wr_t *w, const value_t *v, bool keyed, int depth) {
    size_t count = 0;
    if (keyed)
        count = (v && v->kind == V_MAP) ? v->map.len : 0;
    else
        count = (v && v->kind == V_LIST) ? v->list.len : 0;

    uint8_t hdr[8];
    wr32(&hdr[0], (uint32_t)count);
    wr32(&hdr[4], 0);
    wr_bytes(w, hdr, 8);

    for (size_t i = 0; i < count && !w->bad; i++) {
        if (keyed) {
            wr_fourcc(w, v->map.entries[i].key);
            encode_desc(w, &v->map.entries[i].val, depth + 1);
        } else {
            encode_desc(w, &v->list.items[i], depth + 1);
        }
    }
}

static void encode_leaf_body(wr_t *w, const value_t *leaf, int depth) {
    if (depth > AEVT_MAX_DEPTH) {
        wr_fail(w, "descriptors nested more than %d deep", AEVT_MAX_DEPTH);
        return;
    }
    const value_t *tv = value_map_get(leaf, "type");
    const char *type = val_as_str(tv);
    const value_t *hex = value_map_get(leaf, "hex");
    const value_t *data = value_map_get(leaf, "data");

    // Raw bytes win: this is how an undecoded descriptor round-trips.
    if (hex) {
        const char *h = val_as_str(hex);
        size_t n = h ? strlen(h) : 0;
        if (!h || (n & 1)) {
            wr_fail(w, "the hex body of a '%s' descriptor has an odd digit count", type ? type : "????");
            return;
        }
        for (size_t i = 0; i < n; i += 2) {
            int hi = hex_nibble(h[i]), lo = hex_nibble(h[i + 1]);
            if (hi < 0 || lo < 0) {
                wr_fail(w, "'%c%c' is not a hex byte", h[i], h[i + 1]);
                return;
            }
            uint8_t byte = (uint8_t)((hi << 4) | lo);
            wr_bytes(w, &byte, 1);
        }
        return;
    }

    if (!strcmp(type, "true") || !strcmp(type, "fals") || !strcmp(type, "null") || !strcmp(type, "msng")) {
        return; // zero-length by definition
    }
    if (!data) {
        wr_fail(w, "a '%s' descriptor carries neither data nor hex", type);
        return;
    }
    if (!strcmp(type, "TEXT") || !strcmp(type, "utxt")) {
        const char *s = val_as_str(data);
        wr_bytes(w, s ? s : "", s ? (int)strlen(s) : 0);
    } else if (!strcmp(type, "cstr")) {
        const char *s = val_as_str(data);
        wr_bytes(w, s ? s : "", s ? (int)strlen(s) : 0);
        uint8_t nul = 0;
        wr_bytes(w, &nul, 1);
    } else if (!strcmp(type, "long") || !strcmp(type, "magn")) {
        uint8_t b[4];
        wr32(b, (uint32_t)val_as_i64(data, NULL));
        wr_bytes(w, b, 4);
    } else if (!strcmp(type, "shor")) {
        uint8_t b[2];
        wr16(b, (uint16_t)val_as_i64(data, NULL));
        wr_bytes(w, b, 2);
    } else if (!strcmp(type, "comp")) {
        uint8_t b[8];
        uint64_t u = (uint64_t)val_as_i64(data, NULL);
        wr32(&b[0], (uint32_t)(u >> 32));
        wr32(&b[4], (uint32_t)u);
        wr_bytes(w, b, 8);
    } else if (!strcmp(type, "bool")) {
        uint8_t b = val_as_bool(data) ? 1 : 0;
        wr_bytes(w, &b, 1);
    } else if (!strcmp(type, "type") || !strcmp(type, "enum") || !strcmp(type, "sign") || !strcmp(type, "prop") ||
               !strcmp(type, "keyw")) {
        wr_fourcc(w, val_as_str(data));
    } else if (!strcmp(type, "list")) {
        if (data->kind != V_LIST) {
            wr_fail(w, "a 'list' descriptor needs a list body");
            return;
        }
        encode_collection(w, data, false, depth);
    } else if (!strcmp(type, "reco") || !strcmp(type, "obj ") || !strcmp(type, "rang") || !strcmp(type, "insl")) {
        if (data->kind != V_MAP) {
            wr_fail(w, "a '%s' descriptor needs a record body", type);
            return;
        }
        encode_collection(w, data, true, depth);
    } else {
        wr_fail(w, "cannot encode a '%s' descriptor without a hex body", type);
    }
}

int aevt_encode(const value_t *event, uint8_t *out, int out_max, char *err, size_t err_len) {
    if (!event || event->kind != V_MAP || !out) {
        if (err)
            snprintf(err, err_len, "an event must be a map with class, id and parameters");
        return -1;
    }
    wr_t w = {.out = out, .max = out_max};
    wr_bytes(&w, AEVT_SIGNATURE, 4);
    uint8_t ver[4];
    wr32(ver, AEVT_VERSION);
    wr_bytes(&w, ver, 4);

    // Meta section first, then the terminator, then the parameters (§5.2).
    const value_t *attrs = value_map_get(event, "attrs");
    if (attrs && attrs->kind == V_MAP) {
        for (size_t i = 0; i < attrs->map.len && !w.bad; i++) {
            wr_fourcc(&w, attrs->map.entries[i].key);
            encode_desc(&w, &attrs->map.entries[i].val, 0);
        }
    }
    wr_bytes(&w, AEVT_META_END, 4);

    for (size_t i = 0; i < event->map.len && !w.bad; i++) {
        const char *key = event->map.entries[i].key;
        // class, id and attrs are framing; everything else is a parameter,
        // including `errn`, which the event object also republishes (§6.1).
        if (!strcmp(key, "class") || !strcmp(key, "id") || !strcmp(key, "attrs"))
            continue;
        const value_t *leaf = &event->map.entries[i].val;
        if (leaf->kind != V_MAP) {
            wr_fail(&w, "parameter '%s' is not a descriptor", key);
            break;
        }
        wr_fourcc(&w, key);
        encode_desc(&w, leaf, 0);
    }

    if (w.bad) {
        if (err)
            snprintf(err, err_len, "%s", w.why);
        return -1;
    }
    return w.pos;
}

bool aevt_event_codes(const value_t *event, char class4[5], char id4[5]) {
    if (!event || event->kind != V_MAP)
        return false;
    const value_t *c = value_map_get(event, "class");
    const value_t *i = value_map_get(event, "id");
    const char *cs = val_as_str(c);
    const char *is = val_as_str(i);
    if (!cs || !is)
        return false;
    snprintf(class4, 5, "%-4.4s", cs);
    snprintf(id4, 5, "%-4.4s", is);
    return true;
}

// ============================================================================
// Operations — the text grammar (§6.2)
// ============================================================================

typedef struct {
    const char *s;
    int pos;
    bool bad;
    char why[160];
} tx_t;

static bool tx_fail(tx_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool tx_fail(tx_t *t, const char *fmt, ...) {
    if (!t->bad) {
        char msg[128];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        snprintf(t->why, sizeof(t->why), "%s at offset %d", msg, t->pos);
        t->bad = true;
    }
    return false;
}

static void tx_skip_space(tx_t *t) {
    while (t->s[t->pos] && isspace((unsigned char)t->s[t->pos]))
        t->pos++;
}

static bool tx_eat(tx_t *t, char c) {
    tx_skip_space(t);
    if (t->s[t->pos] != c)
        return false;
    t->pos++;
    return true;
}

static bool tx_expect(tx_t *t, char c) {
    if (tx_eat(t, c))
        return true;
    return tx_fail(t, "expected '%c'", c);
}

static bool tx_peek_word(tx_t *t, const char *word) {
    tx_skip_space(t);
    size_t n = strlen(word);
    if (strncmp(t->s + t->pos, word, n) != 0)
        return false;
    char after = t->s[t->pos + n];
    return !(isalnum((unsigned char)after) || after == '_');
}

static bool tx_eat_word(tx_t *t, const char *word) {
    if (!tx_peek_word(t, word))
        return false;
    t->pos += (int)strlen(word);
    return true;
}

// A four-character code: bare when identifier-safe, quoted otherwise.
static bool tx_fourcc(tx_t *t, char out[5]) {
    tx_skip_space(t);
    if (t->s[t->pos] == '\'') {
        t->pos++;
        int n = 0;
        while (t->s[t->pos] && t->s[t->pos] != '\'' && n < 4)
            out[n++] = t->s[t->pos++];
        if (t->s[t->pos] != '\'')
            return tx_fail(t, "unterminated four-character code");
        t->pos++;
        while (n < 4)
            out[n++] = ' ';
        out[4] = '\0';
        return true;
    }
    int n = 0;
    while (t->s[t->pos] && (isalnum((unsigned char)t->s[t->pos]) || t->s[t->pos] == '_') && n < 4)
        out[n++] = t->s[t->pos++];
    if (n == 0)
        return tx_fail(t, "expected a four-character code");
    while (n < 4)
        out[n++] = ' ';
    out[4] = '\0';
    return true;
}

// A double-quoted string with \" and \\ escapes.  The caller frees.
static char *tx_string(tx_t *t) {
    tx_skip_space(t);
    if (t->s[t->pos] != '"') {
        tx_fail(t, "expected a quoted string");
        return NULL;
    }
    t->pos++;
    size_t cap = 32, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        tx_fail(t, "out of memory");
        return NULL;
    }
    while (t->s[t->pos] && t->s[t->pos] != '"') {
        char c = t->s[t->pos++];
        if (c == '\\' && t->s[t->pos])
            c = t->s[t->pos++];
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = (char *)realloc(buf, cap);
            if (!grown) {
                free(buf);
                tx_fail(t, "out of memory");
                return NULL;
            }
            buf = grown;
        }
        buf[len++] = c;
    }
    if (t->s[t->pos] != '"') {
        free(buf);
        tx_fail(t, "unterminated string");
        return NULL;
    }
    t->pos++;
    buf[len] = '\0';
    return buf;
}

static bool tx_integer(tx_t *t, int64_t *out) {
    tx_skip_space(t);
    char *end = NULL;
    const char *start = t->s + t->pos;
    long long v = strtoll(start, &end, 0);
    if (end == start)
        return tx_fail(t, "expected an integer");
    t->pos += (int)(end - start);
    *out = (int64_t)v;
    return true;
}

static value_t tx_desc(tx_t *t, int depth);

// Build the standard {type, data} leaf.
static value_t leaf_of(const char *type, value_t data, bool has_data) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "type", val_str(type));
    if (has_data)
        val_map_put(b, "data", data);
    return val_map_finish(b);
}

// key ':' desc, appended to a builder.
static bool tx_param(tx_t *t, value_map_builder_t *into, int depth) {
    char key[5];
    if (!tx_fourcc(t, key))
        return false;
    if (!tx_expect(t, ':'))
        return false;
    value_t v = tx_desc(t, depth + 1);
    if (t->bad) {
        value_free(&v);
        return false;
    }
    val_map_put(into, key, v);
    return true;
}

static value_t tx_desc(tx_t *t, int depth) {
    if (depth > AEVT_MAX_DEPTH)
        return (tx_fail(t, "descriptors nested too deeply"), val_none());
    tx_skip_space(t);
    char c = t->s[t->pos];

    if (c == '"') {
        char *s = tx_string(t);
        if (!s)
            return val_none();
        value_t leaf = leaf_of("TEXT", val_str(s), true);
        free(s);
        return leaf;
    }
    if (c == '[') {
        t->pos++;
        value_t *items = NULL;
        size_t len = 0, cap = 0;
        if (!tx_eat(t, ']')) {
            do {
                value_t item = tx_desc(t, depth + 1);
                if (t->bad) {
                    value_free(&item);
                    break;
                }
                if (!val_list_push(&items, &len, &cap, item)) {
                    tx_fail(t, "out of memory");
                    break;
                }
            } while (tx_eat(t, ','));
            if (!t->bad)
                tx_expect(t, ']');
        }
        if (t->bad) {
            for (size_t i = 0; i < len; i++)
                value_free(&items[i]);
            free(items);
            return val_none();
        }
        return leaf_of("list", val_list(items, len), true);
    }
    if (tx_eat_word(t, "true"))
        return leaf_of("true", val_none(), false);
    if (tx_eat_word(t, "false"))
        return leaf_of("fals", val_none(), false);

    // Constructor forms: name(...) or name{...}
    if (tx_peek_word(t, "null")) {
        t->pos += 4;
        if (!tx_expect(t, '(') || !tx_expect(t, ')'))
            return val_none();
        return leaf_of("null", val_none(), false);
    }
    if (tx_peek_word(t, "type") || tx_peek_word(t, "enum")) {
        char kind[5] = {0};
        memcpy(kind, t->s + t->pos, 4);
        t->pos += 4;
        char code[5];
        if (!tx_expect(t, '(') || !tx_fourcc(t, code) || !tx_expect(t, ')'))
            return val_none();
        return leaf_of(kind, val_str(code), true);
    }
    if (tx_eat_word(t, "hex")) {
        char code[5];
        if (!tx_expect(t, '(') || !tx_fourcc(t, code) || !tx_expect(t, ','))
            return val_none();
        char *h = tx_string(t);
        if (!h)
            return val_none();
        if (!tx_expect(t, ')')) {
            free(h);
            return val_none();
        }
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "type", val_str(code));
        val_map_put(b, "hex", val_str(h));
        free(h);
        return val_map_finish(b);
    }
    if (tx_eat_word(t, "fss")) {
        // An FSSpec: volume reference, parent directory, name (§6.2).  The
        // Apple Event Manager coerces this to an alias on the guest side.
        int64_t vref = 0, parid = 0;
        if (!tx_expect(t, '(') || !tx_integer(t, &vref) || !tx_expect(t, ',') || !tx_integer(t, &parid) ||
            !tx_expect(t, ','))
            return val_none();
        char *name = tx_string(t);
        if (!name)
            return val_none();
        if (!tx_expect(t, ')')) {
            free(name);
            return val_none();
        }
        size_t n = strlen(name);
        if (n > 63)
            n = 63;
        uint8_t body[2 + 4 + 64];
        memset(body, 0, sizeof(body));
        wr16(&body[0], (uint16_t)vref);
        wr32(&body[2], (uint32_t)parid);
        body[6] = (uint8_t)n;
        memcpy(&body[7], name, n);
        free(name);
        static const char DIGITS[] = "0123456789ABCDEF";
        char hex[sizeof(body) * 2 + 1];
        for (size_t i = 0; i < sizeof(body); i++) {
            hex[i * 2] = DIGITS[body[i] >> 4];
            hex[i * 2 + 1] = DIGITS[body[i] & 0x0F];
        }
        hex[sizeof(body) * 2] = '\0';
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "type", val_str("fss "));
        val_map_put(b, "hex", val_str(hex));
        return val_map_finish(b);
    }
    if (tx_peek_word(t, "rec") || tx_peek_word(t, "obj")) {
        bool is_obj = tx_peek_word(t, "obj");
        t->pos += 3;
        if (!tx_expect(t, '{'))
            return val_none();
        value_map_builder_t *b = val_map_new();
        if (!tx_eat(t, '}')) {
            do {
                if (!tx_param(t, b, depth))
                    break;
            } while (tx_eat(t, ','));
            if (!t->bad)
                tx_expect(t, '}');
        }
        value_t body = val_map_finish(b);
        if (t->bad) {
            value_free(&body);
            return val_none();
        }
        return leaf_of(is_obj ? "obj " : "reco", body, true);
    }

    // Anything else must be an integer.
    int64_t n = 0;
    if (!tx_integer(t, &n))
        return val_none();
    return leaf_of("long", val_int(n), true);
}

value_t aevt_parse_text(const char *text, char *err, size_t err_len) {
    if (!text || !*text) {
        if (err)
            snprintf(err, err_len, "the event text is empty");
        return val_err("the event text is empty");
    }
    tx_t t = {.s = text};
    char class4[5], id4[5];
    if (!tx_fourcc(&t, class4) || !tx_expect(&t, '/') || !tx_fourcc(&t, id4)) {
        if (err)
            snprintf(err, err_len, "%s", t.why);
        return val_err("%s", t.why);
    }

    value_map_builder_t *ev = val_map_new();
    val_map_put(ev, "class", val_str(class4));
    val_map_put(ev, "id", val_str(id4));
    value_map_builder_t *params = val_map_new();

    if (tx_eat(&t, '{')) {
        if (!tx_eat(&t, '}')) {
            do {
                if (!tx_param(&t, params, 0))
                    break;
            } while (tx_eat(&t, ','));
            if (!t.bad)
                tx_expect(&t, '}');
        }
    }
    tx_skip_space(&t);
    if (!t.bad && t.s[t.pos])
        tx_fail(&t, "unexpected trailing text");

    value_t body = val_map_finish(params);
    if (t.bad) {
        value_free(&body);
        value_t partial = val_map_finish(ev);
        value_free(&partial);
        if (err)
            snprintf(err, err_len, "%s", t.why);
        return val_err("%s", t.why);
    }
    // Parameters sit at the top level of the event map (§6.1).
    if (body.kind == V_MAP) {
        for (size_t i = 0; i < body.map.len; i++)
            val_map_put(ev, body.map.entries[i].key, value_copy(&body.map.entries[i].val));
    }
    value_free(&body);
    return val_map_finish(ev);
}

// ============================================================================
// Operations — rendering back to text
// ============================================================================

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool bad;
} sb_t;

static void sb_add(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_add(sb_t *s, const char *fmt, ...) {
    if (s->bad)
        return;
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        s->bad = true;
        return;
    }
    if (s->len + (size_t)need + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 128;
        while (cap < s->len + (size_t)need + 1)
            cap *= 2;
        char *grown = (char *)realloc(s->buf, cap);
        if (!grown) {
            s->bad = true;
            return;
        }
        s->buf = grown;
        s->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    s->len += (size_t)need;
}

// A code is written bare when it is identifier-safe, quoted otherwise.
static void sb_fourcc(sb_t *s, const char *code) {
    bool safe = code && *code;
    for (const char *p = code; safe && *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            safe = false;
    if (safe)
        sb_add(s, "%s", code);
    else
        sb_add(s, "'%s'", code ? code : "");
}

static void sb_quoted(sb_t *s, const char *text) {
    sb_add(s, "\"");
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '"' || *p == '\\')
            sb_add(s, "\\%c", *p);
        else
            sb_add(s, "%c", *p);
    }
    sb_add(s, "\"");
}

static void sb_desc(sb_t *s, const value_t *leaf);

static void sb_params(sb_t *s, const value_t *map) {
    for (size_t i = 0; i < map->map.len; i++) {
        if (i)
            sb_add(s, ", ");
        sb_fourcc(s, map->map.entries[i].key);
        sb_add(s, ":");
        sb_desc(s, &map->map.entries[i].val);
    }
}

static void sb_desc(sb_t *s, const value_t *leaf) {
    const value_t *tv = value_map_get(leaf, "type");
    const char *type = val_as_str(tv);
    const value_t *hex = value_map_get(leaf, "hex");
    const value_t *data = value_map_get(leaf, "data");
    if (!type) {
        sb_add(s, "null()");
        return;
    }
    if (hex) {
        sb_add(s, "hex(");
        sb_fourcc(s, type);
        sb_add(s, ", ");
        sb_quoted(s, val_as_str(hex));
        sb_add(s, ")");
        return;
    }
    if (!strcmp(type, "TEXT") && data) {
        sb_quoted(s, val_as_str(data));
        return;
    }
    if ((!strcmp(type, "long") || !strcmp(type, "shor") || !strcmp(type, "magn") || !strcmp(type, "comp")) && data) {
        sb_add(s, "%lld", (long long)val_as_i64(data, NULL));
        return;
    }
    if (!strcmp(type, "true"))
        return sb_add(s, "true");
    if (!strcmp(type, "fals"))
        return sb_add(s, "false");
    if (!strcmp(type, "bool") && data)
        return sb_add(s, "%s", val_as_bool(data) ? "true" : "false");
    if (!strcmp(type, "null") || !strcmp(type, "msng"))
        return sb_add(s, "null()");
    if ((!strcmp(type, "type") || !strcmp(type, "enum")) && data) {
        sb_add(s, "%s(", type);
        sb_fourcc(s, val_as_str(data));
        sb_add(s, ")");
        return;
    }
    if (!strcmp(type, "list") && data && data->kind == V_LIST) {
        sb_add(s, "[");
        for (size_t i = 0; i < data->list.len; i++) {
            if (i)
                sb_add(s, ", ");
            sb_desc(s, &data->list.items[i]);
        }
        sb_add(s, "]");
        return;
    }
    if (data && data->kind == V_MAP) {
        sb_add(s, "%s{", !strcmp(type, "obj ") ? "obj" : "rec");
        sb_params(s, data);
        sb_add(s, "}");
        return;
    }
    // Nothing better to say: name the type and leave the body empty.
    sb_add(s, "hex(");
    sb_fourcc(s, type);
    sb_add(s, ", \"\")");
}

char *aevt_render_text(const value_t *event) {
    char class4[5], id4[5];
    if (!aevt_event_codes(event, class4, id4))
        return NULL;
    sb_t s = {0};
    sb_fourcc(&s, class4);
    sb_add(&s, "/");
    sb_fourcc(&s, id4);
    sb_add(&s, "{");
    bool first = true;
    for (size_t i = 0; i < event->map.len; i++) {
        const char *key = event->map.entries[i].key;
        if (!strcmp(key, "class") || !strcmp(key, "id") || !strcmp(key, "attrs"))
            continue;
        if (!first)
            sb_add(&s, ", ");
        first = false;
        sb_fourcc(&s, key);
        sb_add(&s, ":");
        sb_desc(&s, &event->map.entries[i].val);
    }
    sb_add(&s, "}");
    if (s.bad) {
        free(s.buf);
        return NULL;
    }
    return s.buf ? s.buf : strdup("");
}
