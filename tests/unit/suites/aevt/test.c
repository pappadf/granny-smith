// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Codec unit tests for Apple events (src/core/network/appletalk_aevt_codec.c).
//
// Three representations, two conversions, one invariant: an AETF stream
// decodes to a map, the map re-encodes to the same bytes, and the text form
// round-trips through both.  Layouts are transcribed from
// docs/core/network/ppc_appleevents.md §5.2 (stream), §5.4 (lists and
// records) and §6 (map and text forms) — no transport is involved, so every
// case is a fraction of a millisecond.

#include "appletalk_aevt.h"
#include "test_assert.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

// A stream builder that mirrors the layout tables, so the goldens below read
// like the document rather than like hex.
typedef struct {
    uint8_t b[1024];
    int len;
} sbuf_t;

static void put_code(sbuf_t *s, const char *code) {
    for (int i = 0; i < 4; i++)
        s->b[s->len++] = (uint8_t)(code[i] ? code[i] : ' ');
}
static void put32(sbuf_t *s, uint32_t v) {
    s->b[s->len++] = (uint8_t)(v >> 24);
    s->b[s->len++] = (uint8_t)(v >> 16);
    s->b[s->len++] = (uint8_t)(v >> 8);
    s->b[s->len++] = (uint8_t)v;
}
static void put_bytes(sbuf_t *s, const void *p, int n) {
    memcpy(s->b + s->len, p, (size_t)n);
    s->len += n;
}
static void put_pad(sbuf_t *s) {
    if (s->len & 1)
        s->b[s->len++] = 0;
}
// header: signature + version
static void put_header(sbuf_t *s) {
    put_code(s, "aevt");
    put32(s, 0x00010001u);
}
// one parameter: keyword, type, length, data, pad
static void put_param(sbuf_t *s, const char *key, const char *type, const void *data, int len) {
    put_code(s, key);
    put_code(s, type);
    put32(s, (uint32_t)len);
    put_bytes(s, data, len);
    put_pad(s);
}

static const value_t *leaf(const value_t *ev, const char *key) {
    const value_t *v = value_map_get(ev, key);
    ASSERT_TRUE(v != NULL);
    return v;
}
static const char *leaf_type(const value_t *ev, const char *key) {
    return val_as_str(value_map_get(leaf(ev, key), "type"));
}
static const value_t *leaf_data(const value_t *ev, const char *key) {
    return value_map_get(leaf(ev, key), "data");
}

// Decode, re-encode, and prove the bytes came back identical (§5.2).
static void assert_round_trips(const uint8_t *stream, int len) {
    value_t ev = aevt_decode("aevt", "test", stream, len);
    ASSERT_TRUE(!val_is_error(&ev));
    uint8_t out[2048];
    char err[192] = "";
    int n = aevt_encode(&ev, out, (int)sizeof(out), err, sizeof(err));
    if (n < 0)
        fprintf(stderr, "encode failed: %s\n", err);
    ASSERT_EQ_INT(n, len);
    ASSERT_TRUE(memcmp(out, stream, (size_t)len) == 0);
    value_free(&ev);
}

// ============================================================================
// Decode
// ============================================================================

// The smallest legal event: header plus the meta terminator (§5.2).
TEST(test_decode_empty_event) {
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    ASSERT_EQ_INT(s.len, 12);

    value_t ev = aevt_decode("aevt", "oapp", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&ev, "class")), "aevt") == 0);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&ev, "id")), "oapp") == 0);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// A sender may shorten an empty event to nothing at all (§5.2).
TEST(test_decode_zero_length_stream) {
    value_t ev = aevt_decode("aevt", "quit", NULL, 0);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&ev, "id")), "quit") == 0);
    value_free(&ev);
}

// Scalars of every decoded kind (§5.6), including the odd-length text that
// exercises the pad byte.
TEST(test_decode_scalars) {
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "TEXT", "odd", 3); // odd length -> one pad byte
    uint8_t four[4] = {0x00, 0x00, 0x04, 0xD2}; // 1234
    put_param(&s, "num ", "long", four, 4);
    uint8_t two[2] = {0xFF, 0xFE}; // -2
    put_param(&s, "shrt", "shor", two, 2);
    uint8_t one[1] = {1};
    put_param(&s, "flag", "bool", one, 1);
    put_param(&s, "yes ", "true", NULL, 0);
    put_param(&s, "kind", "type", "docu", 4);
    put_param(&s, "none", "null", NULL, 0);

    value_t ev = aevt_decode("core", "getd", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "----"), "TEXT") == 0);
    ASSERT_TRUE(strcmp(val_as_str(leaf_data(&ev, "----")), "odd") == 0);
    ASSERT_EQ_INT((int)val_as_i64(leaf_data(&ev, "num "), NULL), 1234);
    ASSERT_EQ_INT((int)val_as_i64(leaf_data(&ev, "shrt"), NULL), -2);
    ASSERT_TRUE(val_as_bool(leaf_data(&ev, "flag")));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "yes "), "true") == 0);
    ASSERT_TRUE(strcmp(val_as_str(leaf_data(&ev, "kind")), "docu") == 0);
    // A null descriptor carries neither data nor hex (§6.1).
    ASSERT_TRUE(value_map_get(leaf(&ev, "none"), "data") == NULL);
    ASSERT_TRUE(value_map_get(leaf(&ev, "none"), "hex") == NULL);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// Attributes live before the terminator, parameters after it (§5.2).
TEST(test_decode_meta_section) {
    sbuf_t s = {0};
    put_header(&s);
    uint8_t timeout[4] = {0, 0, 0x0E, 0x10}; // 3600 ticks
    put_param(&s, "timo", "long", timeout, 4);
    put_param(&s, "repq", "true", NULL, 0);
    put_code(&s, ";;;;");
    put_param(&s, "----", "TEXT", "hi", 2);

    value_t ev = aevt_decode("aevt", "odoc", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *attrs = value_map_get(&ev, "attrs");
    ASSERT_TRUE(attrs && attrs->kind == V_MAP);
    ASSERT_EQ_INT((int)attrs->map.len, 2);
    ASSERT_EQ_INT((int)val_as_i64(value_map_get(value_map_get(attrs, "timo"), "data"), NULL), 3600);
    // Attributes must not leak into the parameter namespace.
    ASSERT_TRUE(value_map_get(&ev, "timo") == NULL);
    ASSERT_TRUE(value_map_get(&ev, "----") != NULL);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// An unfactored list: count, zero prefix, then type/length/data per item
// (§5.4).
TEST(test_decode_unfactored_list) {
    sbuf_t items = {0};
    put32(&items, 2); // count
    put32(&items, 0); // prefix size: unfactored
    put_code(&items, "TEXT");
    put32(&items, 3);
    put_bytes(&items, "one", 3);
    put_pad(&items);
    put_code(&items, "TEXT");
    put32(&items, 3);
    put_bytes(&items, "two", 3);
    put_pad(&items);

    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);

    value_t ev = aevt_decode("aevt", "odoc", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *list = leaf_data(&ev, "----");
    ASSERT_TRUE(list && list->kind == V_LIST);
    ASSERT_EQ_INT((int)list->list.len, 2);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&list->list.items[0], "data")), "one") == 0);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&list->list.items[1], "data")), "two") == 0);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// Prefix size 4: every item shares the type held in the prefix (§5.4).
TEST(test_decode_type_factored_list) {
    sbuf_t items = {0};
    put32(&items, 2);
    put32(&items, 4); // prefix holds the shared type
    put_code(&items, "long");
    put32(&items, 4);
    put32(&items, 11);
    put32(&items, 4);
    put32(&items, 22);

    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);

    value_t ev = aevt_decode("aevt", "test", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *list = leaf_data(&ev, "----");
    ASSERT_EQ_INT((int)list->list.len, 2);
    ASSERT_EQ_INT((int)val_as_i64(value_map_get(&list->list.items[0], "data"), NULL), 11);
    ASSERT_EQ_INT((int)val_as_i64(value_map_get(&list->list.items[1], "data"), NULL), 22);
    value_free(&ev);
    // Re-encoding normalises to the unfactored form, so only the value must
    // survive the round trip here, not the bytes.
}

// Prefix size 8: type and length are both factored out, so each item is bare
// data (§5.4).
TEST(test_decode_fully_factored_list) {
    sbuf_t items = {0};
    put32(&items, 3);
    put32(&items, 8);
    put_code(&items, "type");
    put32(&items, 4); // every item is four bytes
    put_code(&items, "cwin");
    put_code(&items, "cfol");
    put_code(&items, "docu");

    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);

    value_t ev = aevt_decode("core", "getd", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *list = leaf_data(&ev, "----");
    ASSERT_EQ_INT((int)list->list.len, 3);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(&list->list.items[2], "data")), "docu") == 0);
    value_free(&ev);
}

// The packed case: fully factored with a one-byte item length, where items
// are *not* rounded up to even (§5.4).
TEST(test_decode_packed_list) {
    sbuf_t items = {0};
    put32(&items, 3);
    put32(&items, 8);
    put_code(&items, "bool");
    put32(&items, 1);
    uint8_t packed[3] = {1, 0, 1};
    put_bytes(&items, packed, 3);
    put_pad(&items);

    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);

    value_t ev = aevt_decode("aevt", "test", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *list = leaf_data(&ev, "----");
    ASSERT_EQ_INT((int)list->list.len, 3);
    ASSERT_TRUE(val_as_bool(value_map_get(&list->list.items[0], "data")));
    ASSERT_TRUE(!val_as_bool(value_map_get(&list->list.items[1], "data")));
    ASSERT_TRUE(val_as_bool(value_map_get(&list->list.items[2], "data")));
    value_free(&ev);
}

// An object specifier is a record of form/want/seld/from (§5.4), the shape
// that drives the Scriptable Finder.
TEST(test_decode_object_specifier) {
    sbuf_t rec = {0};
    put32(&rec, 4);
    put32(&rec, 0);
    put_code(&rec, "form");
    put_code(&rec, "enum");
    put32(&rec, 4);
    put_code(&rec, "prop");
    put_code(&rec, "want");
    put_code(&rec, "type");
    put32(&rec, 4);
    put_code(&rec, "prop");
    put_code(&rec, "seld");
    put_code(&rec, "type");
    put32(&rec, 4);
    put_code(&rec, "pnam");
    put_code(&rec, "from");
    put_code(&rec, "null");
    put32(&rec, 0);

    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "obj ", rec.b, rec.len);

    value_t ev = aevt_decode("core", "getd", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "----"), "obj ") == 0);
    const value_t *spec = leaf_data(&ev, "----");
    ASSERT_TRUE(spec && spec->kind == V_MAP);
    ASSERT_EQ_INT((int)spec->map.len, 4);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(value_map_get(spec, "seld"), "data")), "pnam") == 0);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(value_map_get(spec, "from"), "type")), "null") == 0);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// A descriptor type we do not model is kept as raw bytes and re-encodes
// exactly — the property that makes captures safe to replay (§5.6).
TEST(test_unknown_descriptor_round_trips) {
    uint8_t alias[9] = {0x02, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "alis", alias, (int)sizeof(alias));

    value_t ev = aevt_decode("aevt", "odoc", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "----"), "alis") == 0);
    const char *hex = val_as_str(value_map_get(leaf(&ev, "----"), "hex"));
    ASSERT_TRUE(hex != NULL);
    ASSERT_TRUE(strcmp(hex, "0200DEADBEEF010203") == 0);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// A scalar whose length contradicts its type is preserved rather than
// rejected: we are not the guest's validator.
TEST(test_wrong_length_scalar_kept_opaque) {
    uint8_t three[3] = {1, 2, 3};
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "long", three, 3);

    value_t ev = aevt_decode("aevt", "test", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(value_map_get(leaf(&ev, "----"), "hex") != NULL);
    value_free(&ev);
    assert_round_trips(s.b, s.len);
}

// The reply shape of §5.5.
TEST(test_decode_reply_errn) {
    uint8_t errn[4] = {0xFF, 0xFF, 0xF9, 0x54}; // -1708, errAEEventNotHandled
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "errn", "long", errn, 4);
    put_param(&s, "errs", "TEXT", "not handled", 11);

    value_t ev = aevt_decode("aevt", "ansr", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_EQ_INT((int)aevt_reply_errn(&ev), -1708);
    ASSERT_TRUE(strcmp(val_as_str(leaf_data(&ev, "errs")), "not handled") == 0);
    value_free(&ev);

    // A reply with no error parameter reads as success.
    sbuf_t ok = {0};
    put_header(&ok);
    put_code(&ok, ";;;;");
    value_t good = aevt_decode("aevt", "ansr", ok.b, ok.len);
    ASSERT_EQ_INT((int)aevt_reply_errn(&good), 0);
    value_free(&good);
}

// ============================================================================
// Malformed input — every case must be an error, never a crash (§7)
// ============================================================================

static void assert_rejects(const uint8_t *stream, int len, const char *what) {
    value_t ev = aevt_decode("aevt", "test", stream, len);
    if (!val_is_error(&ev))
        fprintf(stderr, "[FAIL] accepted malformed stream: %s\n", what);
    ASSERT_TRUE(val_is_error(&ev));
    value_free(&ev);
}

TEST(test_malformed_streams_rejected) {
    sbuf_t s;
    // Wrong signature.
    s = (sbuf_t){0};
    put_code(&s, "xxxx");
    put32(&s, 0x00010001u);
    put_code(&s, ";;;;");
    assert_rejects(s.b, s.len, "bad signature");

    // Unsupported version.
    s = (sbuf_t){0};
    put_code(&s, "aevt");
    put32(&s, 0x00020000u);
    put_code(&s, ";;;;");
    assert_rejects(s.b, s.len, "bad version");

    // Header cut short.
    s = (sbuf_t){0};
    put_code(&s, "aevt");
    assert_rejects(s.b, s.len, "truncated header");

    // A parameter that claims more data than the stream holds.
    s = (sbuf_t){0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_code(&s, "----");
    put_code(&s, "TEXT");
    put32(&s, 9999);
    assert_rejects(s.b, s.len, "length past the end");

    // A parameter header cut in half.
    s = (sbuf_t){0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_code(&s, "----");
    put_code(&s, "TE");
    assert_rejects(s.b, s.len, "truncated parameter header");

    // Two meta terminators.
    s = (sbuf_t){0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_code(&s, ";;;;");
    assert_rejects(s.b, s.len, "double terminator");

    // A list whose item count exceeds its data.
    sbuf_t items = {0};
    put32(&items, 50);
    put32(&items, 0);
    s = (sbuf_t){0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);
    assert_rejects(s.b, s.len, "list count past the end");

    // An illegal factoring prefix size.
    items = (sbuf_t){0};
    put32(&items, 1);
    put32(&items, 6);
    s = (sbuf_t){0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", items.b, items.len);
    assert_rejects(s.b, s.len, "prefix size 6");
}

// Truncating a well-formed stream at every offset must never crash and must
// never yield a half-built value.
TEST(test_truncation_fuzz) {
    sbuf_t inner = {0};
    put32(&inner, 1);
    put32(&inner, 0);
    put_code(&inner, "TEXT");
    put32(&inner, 5);
    put_bytes(&inner, "inner", 5);
    put_pad(&inner);

    sbuf_t s = {0};
    put_header(&s);
    put_param(&s, "timo", "long", "\0\0\0\1", 4);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", inner.b, inner.len);
    put_param(&s, "errn", "long", "\0\0\0\0", 4);

    // A few cut points are themselves valid streams (the header alone with a
    // terminator, say).  The property that must hold everywhere is that the
    // result is either a clean map or a reported error — never a crash and
    // never a half-built value.
    int rejected = 0;
    for (int cut = 0; cut < s.len; cut++) {
        value_t ev = aevt_decode("aevt", "test", s.b, cut);
        if (val_is_error(&ev))
            rejected++;
        else if (ev.kind != V_MAP)
            fprintf(stderr, "[FAIL] truncation to %d bytes yielded kind %d\n", cut, (int)ev.kind);
        ASSERT_TRUE(val_is_error(&ev) || ev.kind == V_MAP);
        value_free(&ev);
    }
    ASSERT_TRUE(rejected > s.len / 2); // most cuts land mid-descriptor
    value_t whole = aevt_decode("aevt", "test", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&whole));
    value_free(&whole);
}

// Every single-byte mutation is either accepted or reported, never fatal.
TEST(test_mutation_fuzz) {
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "list", "\0\0\0\1\0\0\0\0TEXT\0\0\0\2hi", 22);

    int errors = 0;
    for (int i = 0; i < s.len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t saved = s.b[i];
            s.b[i] ^= (uint8_t)(1 << bit);
            value_t ev = aevt_decode("aevt", "test", s.b, s.len);
            if (val_is_error(&ev))
                errors++;
            value_free(&ev);
            s.b[i] = saved;
        }
    }
    // Some mutations are legal streams; the point is that none crashed and
    // that the obviously broken ones were caught.
    ASSERT_TRUE(errors > 0);
}

// ============================================================================
// Text form
// ============================================================================

TEST(test_text_parse_minimal) {
    char err[192] = "";
    value_t ev = aevt_parse_text("aevt/oapp", err, sizeof(err));
    ASSERT_TRUE(!val_is_error(&ev));
    char c[5], i[5];
    ASSERT_TRUE(aevt_event_codes(&ev, c, i));
    ASSERT_TRUE(strcmp(c, "aevt") == 0);
    ASSERT_TRUE(strcmp(i, "oapp") == 0);
    value_free(&ev);
}

TEST(test_text_parse_scalars) {
    char err[192] = "";
    value_t ev = aevt_parse_text("core/setd{'----':\"hello\", num:42, neg:-7, hx:0x10, "
                                 "yes:true, no:false, t:type(docu), e:enum(yes ), n:null()}",
                                 err, sizeof(err));
    if (val_is_error(&ev))
        fprintf(stderr, "parse failed: %s\n", err);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(val_as_str(leaf_data(&ev, "----")), "hello") == 0);
    ASSERT_EQ_INT((int)val_as_i64(leaf_data(&ev, "num "), NULL), 42);
    ASSERT_EQ_INT((int)val_as_i64(leaf_data(&ev, "neg "), NULL), -7);
    ASSERT_EQ_INT((int)val_as_i64(leaf_data(&ev, "hx  "), NULL), 16);
    ASSERT_TRUE(strcmp(leaf_type(&ev, "yes "), "true") == 0);
    ASSERT_TRUE(strcmp(leaf_type(&ev, "no  "), "fals") == 0);
    ASSERT_TRUE(strcmp(val_as_str(leaf_data(&ev, "t   ")), "docu") == 0);
    ASSERT_TRUE(strcmp(leaf_type(&ev, "e   "), "enum") == 0);
    ASSERT_TRUE(strcmp(leaf_type(&ev, "n   "), "null") == 0);
    value_free(&ev);
}

// The flagship query of the proposal's test plan, parsed and encoded.
TEST(test_text_object_specifier) {
    char err[192] = "";
    value_t ev = aevt_parse_text("core/getd{'----':obj{form:enum(prop), want:type(prop), "
                                 "seld:type(pnam), from:null()}}",
                                 err, sizeof(err));
    if (val_is_error(&ev))
        fprintf(stderr, "parse failed: %s\n", err);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "----"), "obj ") == 0);
    const value_t *spec = leaf_data(&ev, "----");
    ASSERT_EQ_INT((int)spec->map.len, 4);

    uint8_t out[512];
    int n = aevt_encode(&ev, out, (int)sizeof(out), err, sizeof(err));
    ASSERT_TRUE(n > 0);
    // Decoding our own bytes must give the same specifier back.
    value_t again = aevt_decode("core", "getd", out, n);
    ASSERT_TRUE(!val_is_error(&again));
    const value_t *spec2 = leaf_data(&again, "----");
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(value_map_get(spec2, "seld"), "data")), "pnam") == 0);
    value_free(&again);
    value_free(&ev);
}

TEST(test_text_list_and_record) {
    char err[192] = "";
    value_t ev = aevt_parse_text("aevt/odoc{'----':[\"a\", \"b\"], meta:rec{k1:1, k2:\"two\"}}", err, sizeof(err));
    ASSERT_TRUE(!val_is_error(&ev));
    const value_t *list = leaf_data(&ev, "----");
    ASSERT_EQ_INT((int)list->list.len, 2);
    ASSERT_TRUE(strcmp(leaf_type(&ev, "meta"), "reco") == 0);
    ASSERT_EQ_INT((int)leaf_data(&ev, "meta")->map.len, 2);
    value_free(&ev);
}

TEST(test_text_hex_and_fss) {
    char err[192] = "";
    value_t ev =
        aevt_parse_text("aevt/odoc{'----':hex('alis', \"DEADBEEF\"), fs:fss(-1, 2, \"ReadMe\")}", err, sizeof(err));
    if (val_is_error(&ev))
        fprintf(stderr, "parse failed: %s\n", err);
    ASSERT_TRUE(!val_is_error(&ev));
    ASSERT_TRUE(strcmp(leaf_type(&ev, "----"), "alis") == 0);
    ASSERT_TRUE(strcmp(val_as_str(value_map_get(leaf(&ev, "----"), "hex")), "DEADBEEF") == 0);
    // The FSSpec is volume ref, parent id, then a Pascal name (§6.2).
    const char *fss = val_as_str(value_map_get(leaf(&ev, "fs  "), "hex"));
    ASSERT_TRUE(fss != NULL);
    ASSERT_TRUE(strncmp(fss, "FFFF0000000206526561644D65", 26) == 0);
    value_free(&ev);
}

// Text -> map -> text must be stable, and the second parse must agree.
TEST(test_text_round_trip) {
    static const char *cases[] = {
        "aevt/oapp{}",
        "aevt/quit{}",
        "core/getd{'----':obj{form:enum(prop), want:type(prop), seld:type(pnam), from:null()}}",
        "aevt/odoc{'----':[hex('alis', \"00FF\")]}",
        "misc/dosc{'----':\"tell me\", flag:true, n:-3}",
        "aevt/test{'----':rec{a:1, b:[\"x\", \"y\"]}}",
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        char err[192] = "";
        value_t ev = aevt_parse_text(cases[i], err, sizeof(err));
        if (val_is_error(&ev))
            fprintf(stderr, "case %d parse failed: %s\n", i, err);
        ASSERT_TRUE(!val_is_error(&ev));
        char *text = aevt_render_text(&ev);
        ASSERT_TRUE(text != NULL);

        value_t again = aevt_parse_text(text, err, sizeof(err));
        if (val_is_error(&again))
            fprintf(stderr, "case %d reparse of '%s' failed: %s\n", i, text, err);
        ASSERT_TRUE(!val_is_error(&again));
        char *text2 = aevt_render_text(&again);
        ASSERT_TRUE(text2 != NULL);
        if (strcmp(text, text2) != 0)
            fprintf(stderr, "case %d unstable: '%s' vs '%s'\n", i, text, text2);
        ASSERT_TRUE(strcmp(text, text2) == 0);

        // And the bytes must agree too.
        uint8_t a[1024], b[1024];
        int na = aevt_encode(&ev, a, (int)sizeof(a), err, sizeof(err));
        int nb = aevt_encode(&again, b, (int)sizeof(b), err, sizeof(err));
        ASSERT_TRUE(na > 0);
        ASSERT_EQ_INT(na, nb);
        ASSERT_TRUE(memcmp(a, b, (size_t)na) == 0);

        free(text);
        free(text2);
        value_free(&ev);
        value_free(&again);
    }
}

// Wire -> map -> text -> map -> wire, the full circuit.
TEST(test_wire_text_wire) {
    sbuf_t s = {0};
    put_header(&s);
    put_code(&s, ";;;;");
    put_param(&s, "----", "TEXT", "document", 8);
    uint8_t n[4] = {0, 0, 0, 7};
    put_param(&s, "indx", "long", n, 4);

    value_t ev = aevt_decode("core", "getd", s.b, s.len);
    ASSERT_TRUE(!val_is_error(&ev));
    char *text = aevt_render_text(&ev);
    ASSERT_TRUE(text != NULL);

    char err[192] = "";
    value_t back = aevt_parse_text(text, err, sizeof(err));
    ASSERT_TRUE(!val_is_error(&back));
    uint8_t out[512];
    int len = aevt_encode(&back, out, (int)sizeof(out), err, sizeof(err));
    ASSERT_EQ_INT(len, s.len);
    ASSERT_TRUE(memcmp(out, s.b, (size_t)len) == 0);

    free(text);
    value_free(&back);
    value_free(&ev);
}

TEST(test_text_syntax_errors_reported) {
    static const char *bad[] = {
        "",
        "aevt",
        "aevt/",
        "aevt/oapp{",
        "aevt/oapp{'----'}",
        "aevt/oapp{'----':}",
        "aevt/oapp{'----':\"unterminated}",
        "aevt/oapp{'----':[1, }",
        "aevt/oapp{} trailing",
        "aevt/oapp{'----':hex(alis)}",
    };
    for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        char err[192] = "";
        value_t ev = aevt_parse_text(bad[i], err, sizeof(err));
        if (!val_is_error(&ev))
            fprintf(stderr, "[FAIL] accepted bad text: '%s'\n", bad[i]);
        ASSERT_TRUE(val_is_error(&ev));
        ASSERT_TRUE(err[0] != '\0'); // the reason must be reportable
        value_free(&ev);
    }
}

// The encoder must refuse rather than truncate when the buffer is too small.
TEST(test_encode_respects_buffer) {
    char err[192] = "";
    value_t ev = aevt_parse_text("aevt/odoc{'----':\"a longer document name\"}", err, sizeof(err));
    ASSERT_TRUE(!val_is_error(&ev));
    uint8_t tiny[16];
    ASSERT_EQ_INT(aevt_encode(&ev, tiny, (int)sizeof(tiny), err, sizeof(err)), -1);
    ASSERT_TRUE(err[0] != '\0');
    value_free(&ev);
}

int main(void) {
    RUN(test_decode_empty_event);
    RUN(test_decode_zero_length_stream);
    RUN(test_decode_scalars);
    RUN(test_decode_meta_section);
    RUN(test_decode_unfactored_list);
    RUN(test_decode_type_factored_list);
    RUN(test_decode_fully_factored_list);
    RUN(test_decode_packed_list);
    RUN(test_decode_object_specifier);
    RUN(test_unknown_descriptor_round_trips);
    RUN(test_wrong_length_scalar_kept_opaque);
    RUN(test_decode_reply_errn);
    RUN(test_malformed_streams_rejected);
    RUN(test_truncation_fuzz);
    RUN(test_mutation_fuzz);
    RUN(test_text_parse_minimal);
    RUN(test_text_parse_scalars);
    RUN(test_text_object_specifier);
    RUN(test_text_list_and_record);
    RUN(test_text_hex_and_fss);
    RUN(test_text_round_trip);
    RUN(test_wire_text_wire);
    RUN(test_text_syntax_errors_reported);
    RUN(test_encode_respects_buffer);
    printf("aevt: all tests passed\n");
    return 0;
}
