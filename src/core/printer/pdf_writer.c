// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pdf_writer.c
// Minimal PDF 1.4 writer: see pdf_writer.h.
//
// Object layout: 1 Catalog, 2 Pages, 3 Info; then three objects per page in
// the order the pages are added -- the image XObject, the content stream
// and the Page.  The page objects are written to the buffer as each page is
// added (their offsets recorded), the Catalog, Pages and Info at finish,
// followed by the cross-reference table and the trailer.

#include "pdf_writer.h"

#include "deflate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The three fixed objects
#define OBJ_CATALOG 1
#define OBJ_PAGES   2
#define OBJ_INFO    3
#define OBJ_FIRST   4

// A growable byte buffer
typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool failed; // an allocation failed; everything after is dropped
} pdf_buf_t;

struct pdf_writer {
    pdf_buf_t out; // the document so far
    size_t *offsets; // offsets[n] = byte offset of object n (0 = unused)
    uint32_t n_objects, cap_objects; // highest object number + 1, capacity
    uint32_t *page_objs; // the Page object of each page
    uint32_t n_pages, cap_pages;
    char *title;
    char *producer;
};

// Make room for `more` bytes in `b`.
static bool buf_reserve(pdf_buf_t *b, size_t more) {
    if (b->failed)
        return false;
    if (b->len + more <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

// Append `n` raw bytes.
static void buf_put(pdf_buf_t *b, const void *data, size_t n) {
    if (!buf_reserve(b, n))
        return;
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

// Append formatted text.
static void buf_printf(pdf_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void buf_printf(pdf_buf_t *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(tmp)) {
        b->failed = true; // never happens for the fixed formats used here
        return;
    }
    buf_put(b, tmp, (size_t)n);
}

// A number of points as the shortest fixed-point text (2 decimals at most):
// deterministic, and free of exponent notation.
static void fmt_pt(char *out, size_t cap, double v) {
    long long hundredths = (long long)(v * 100.0 + (v < 0 ? -0.5 : 0.5));
    long long whole = hundredths / 100;
    int frac = (int)(hundredths % 100);
    if (frac < 0)
        frac = -frac;
    if (frac == 0)
        snprintf(out, cap, "%lld", whole);
    else if (frac % 10 == 0)
        snprintf(out, cap, "%lld.%d", whole, frac / 10);
    else
        snprintf(out, cap, "%lld.%02d", whole, frac);
}

// Append `s` as a PDF literal string, escaping what must be escaped.
static void buf_put_string(pdf_buf_t *b, const char *s) {
    buf_put(b, "(", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') {
            char esc[2] = {'\\', (char)*p};
            buf_put(b, esc, 2);
        } else if (*p < 0x20 || *p >= 0x7F) {
            buf_printf(b, "\\%03o", *p);
        } else {
            buf_put(b, p, 1);
        }
    }
    buf_put(b, ")", 1);
}

// Allocate the next object number.
static uint32_t new_object(pdf_writer_t *w) {
    if (w->n_objects >= w->cap_objects) {
        uint32_t cap = w->cap_objects ? w->cap_objects * 2 : 32;
        size_t *o = realloc(w->offsets, cap * sizeof(*o));
        if (!o) {
            w->out.failed = true;
            return 0;
        }
        memset(o + w->cap_objects, 0, (cap - w->cap_objects) * sizeof(*o));
        w->offsets = o;
        w->cap_objects = cap;
    }
    return w->n_objects++;
}

// Start object `n` at the current offset.
static void begin_object(pdf_writer_t *w, uint32_t n) {
    if (n < w->cap_objects)
        w->offsets[n] = w->out.len;
    buf_printf(&w->out, "%u 0 obj\n", (unsigned)n);
}

// Duplicate a string, "" for NULL.
static char *dup_str(const char *s) {
    s = s ? s : "";
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

pdf_writer_t *pdf_writer_new(const char *title, const char *producer) {
    pdf_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->title = dup_str(title);
    w->producer = dup_str(producer);
    if (!w->title || !w->producer) {
        pdf_writer_free(w);
        return NULL;
    }
    // Objects 0..3 are reserved: 0 is the free-list head, 1..3 the fixed ones
    for (int i = 0; i < OBJ_FIRST; i++)
        new_object(w);
    // Header, then a comment with high bytes so tools treat the file as binary
    buf_printf(&w->out, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
    if (w->out.failed) {
        pdf_writer_free(w);
        return NULL;
    }
    return w;
}

// Write one image page: the image XObject, its content stream and the Page.
// `dict` is the image dictionary's kind-specific part; `data`/`len` the
// uncompressed samples.
static bool add_image_page(pdf_writer_t *w, double width_pt, double height_pt, uint32_t px_w, uint32_t px_h,
                           const char *dict, const uint8_t *data, size_t len, bool mask) {
    if (!w || w->out.failed)
        return false;
    // Compress the samples; stored uncompressed if the encoder fails
    size_t cap = deflate_bound(len);
    uint8_t *z = malloc(cap ? cap : 1);
    long zlen = z ? deflate_zlib(NULL, data, len, z, cap, 6) : -1;

    uint32_t img = new_object(w);
    uint32_t content = new_object(w);
    uint32_t page = new_object(w);
    if (w->out.failed) {
        free(z);
        return false;
    }

    begin_object(w, img);
    buf_printf(&w->out, "<< /Type /XObject /Subtype /Image /Width %u /Height %u ", (unsigned)px_w, (unsigned)px_h);
    buf_printf(&w->out, "%s", dict);
    if (zlen >= 0)
        buf_printf(&w->out, " /Filter /FlateDecode /Length %ld >>\nstream\n", zlen);
    else
        buf_printf(&w->out, " /Length %zu >>\nstream\n", len);
    if (zlen >= 0)
        buf_put(&w->out, z, (size_t)zlen);
    else
        buf_put(&w->out, data, len);
    buf_printf(&w->out, "\nendstream\nendobj\n");
    free(z);

    // The image scaled to fill the page; a stencil mask paints in black
    char ws[32], hs[32];
    fmt_pt(ws, sizeof(ws), width_pt);
    fmt_pt(hs, sizeof(hs), height_pt);
    char cs[160];
    int cs_len = snprintf(cs, sizeof(cs), "q %s%s 0 0 %s 0 0 cm /Im0 Do Q\n", mask ? "0 g " : "", ws, hs);
    begin_object(w, content);
    buf_printf(&w->out, "<< /Length %d >>\nstream\n%s", cs_len, cs);
    buf_printf(&w->out, "endstream\nendobj\n");

    begin_object(w, page);
    buf_printf(&w->out, "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] ", OBJ_PAGES, ws, hs);
    buf_printf(&w->out, "/Resources << /XObject << /Im0 %u 0 R >> >> /Contents %u 0 R >>\nendobj\n", (unsigned)img,
               (unsigned)content);

    if (w->n_pages >= w->cap_pages) {
        uint32_t c = w->cap_pages ? w->cap_pages * 2 : 8;
        uint32_t *p = realloc(w->page_objs, c * sizeof(*p));
        if (!p) {
            w->out.failed = true;
            return false;
        }
        w->page_objs = p;
        w->cap_pages = c;
    }
    w->page_objs[w->n_pages++] = page;
    return !w->out.failed;
}

bool pdf_writer_add_mono_page(pdf_writer_t *w, double width_pt, double height_pt, uint32_t px_w, uint32_t px_h,
                              const uint8_t *bits, size_t stride) {
    size_t row = (px_w + 7) / 8;
    // Repack when the caller's rows carry padding beyond the PDF's
    const uint8_t *data = bits;
    uint8_t *packed = NULL;
    if (stride != row) {
        packed = malloc(row * px_h + 1);
        if (!packed)
            return false;
        for (uint32_t y = 0; y < px_h; y++)
            memcpy(packed + y * row, bits + y * stride, row);
        data = packed;
    }
    // A set bit is ink: Decode [1 0] makes 1 the painted value of the mask
    bool ok = add_image_page(w, width_pt, height_pt, px_w, px_h, "/ImageMask true /BitsPerComponent 1 /Decode [1 0]",
                             data, row * px_h, true);
    free(packed);
    return ok;
}

bool pdf_writer_add_indexed4_page(pdf_writer_t *w, double width_pt, double height_pt, uint32_t px_w, uint32_t px_h,
                                  const uint8_t *nibbles, size_t stride, const uint8_t palette_rgb[16][3]) {
    size_t row = (px_w + 1) / 2;
    const uint8_t *data = nibbles;
    uint8_t *packed = NULL;
    if (stride != row) {
        packed = malloc(row * px_h + 1);
        if (!packed)
            return false;
        for (uint32_t y = 0; y < px_h; y++)
            memcpy(packed + y * row, nibbles + y * stride, row);
        data = packed;
    }
    // The palette as a hex string inside the colour space array
    char dict[256];
    int n = snprintf(dict, sizeof(dict), "/ColorSpace [/Indexed /DeviceRGB 15 <");
    for (int i = 0; i < 16; i++)
        n += snprintf(dict + n, sizeof(dict) - (size_t)n, "%02X%02X%02X", palette_rgb[i][0], palette_rgb[i][1],
                      palette_rgb[i][2]);
    snprintf(dict + n, sizeof(dict) - (size_t)n, ">] /BitsPerComponent 4");
    bool ok = add_image_page(w, width_pt, height_pt, px_w, px_h, dict, data, row * px_h, false);
    free(packed);
    return ok;
}

void pdf_writer_set_title(pdf_writer_t *w, const char *title) {
    if (!w)
        return;
    char *t = dup_str(title);
    if (!t)
        return;
    free(w->title);
    w->title = t;
}

// Serialised layout, big-endian: magic, object count, page count, body
// length, title length, producer length; then the offsets (u32 each), the
// page objects (u32 each), the body, the title and the producer.
#define PDF_SER_MAGIC 0x50444657u // "PDFW"

static void put32(pdf_buf_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_put(b, x, 4);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool pdf_writer_serialise(const pdf_writer_t *w, uint8_t **out, size_t *out_len) {
    if (!w || w->out.failed)
        return false;
    pdf_buf_t b = {0};
    size_t tl = strlen(w->title), pl = strlen(w->producer);
    put32(&b, PDF_SER_MAGIC);
    put32(&b, w->n_objects);
    put32(&b, w->n_pages);
    put32(&b, (uint32_t)w->out.len);
    put32(&b, (uint32_t)tl);
    put32(&b, (uint32_t)pl);
    for (uint32_t i = 0; i < w->n_objects; i++)
        put32(&b, (uint32_t)w->offsets[i]);
    for (uint32_t i = 0; i < w->n_pages; i++)
        put32(&b, w->page_objs[i]);
    buf_put(&b, w->out.p, w->out.len);
    buf_put(&b, w->title, tl);
    buf_put(&b, w->producer, pl);
    if (b.failed) {
        free(b.p);
        return false;
    }
    *out = b.p;
    *out_len = b.len;
    return true;
}

pdf_writer_t *pdf_writer_deserialise(const uint8_t *data, size_t len) {
    if (!data || len < 24 || get32(data) != PDF_SER_MAGIC)
        return NULL;
    uint32_t nobj = get32(data + 4), npages = get32(data + 8), body = get32(data + 12);
    uint32_t tl = get32(data + 16), pl = get32(data + 20);
    // Every count is bounded by what the buffer can hold
    uint64_t need = 24ull + 4ull * nobj + 4ull * npages + body + tl + pl;
    if (need != len || nobj < OBJ_FIRST || nobj > (1u << 24) || npages > nobj)
        return NULL;
    pdf_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    const uint8_t *p = data + 24;
    w->offsets = calloc(nobj, sizeof(*w->offsets));
    w->page_objs = calloc(npages ? npages : 1, sizeof(*w->page_objs));
    w->title = malloc(tl + 1);
    w->producer = malloc(pl + 1);
    if (!w->offsets || !w->page_objs || !w->title || !w->producer) {
        pdf_writer_free(w);
        return NULL;
    }
    w->n_objects = w->cap_objects = nobj;
    w->n_pages = npages;
    w->cap_pages = npages ? npages : 1;
    for (uint32_t i = 0; i < nobj; i++, p += 4)
        w->offsets[i] = get32(p);
    for (uint32_t i = 0; i < npages; i++, p += 4)
        w->page_objs[i] = get32(p);
    buf_put(&w->out, p, body);
    p += body;
    memcpy(w->title, p, tl);
    w->title[tl] = '\0';
    p += tl;
    memcpy(w->producer, p, pl);
    w->producer[pl] = '\0';
    if (w->out.failed) {
        pdf_writer_free(w);
        return NULL;
    }
    return w;
}

uint32_t pdf_writer_pages(const pdf_writer_t *w) {
    return w ? w->n_pages : 0;
}

bool pdf_writer_finish(pdf_writer_t *w, uint8_t **out, size_t *out_len) {
    if (!w || !out || !out_len || w->out.failed)
        return false;

    begin_object(w, OBJ_CATALOG);
    buf_printf(&w->out, "<< /Type /Catalog /Pages %d 0 R >>\nendobj\n", OBJ_PAGES);

    begin_object(w, OBJ_PAGES);
    buf_printf(&w->out, "<< /Type /Pages /Count %u /Kids [", (unsigned)w->n_pages);
    for (uint32_t i = 0; i < w->n_pages; i++)
        buf_printf(&w->out, "%s%u 0 R", i ? " " : "", (unsigned)w->page_objs[i]);
    buf_printf(&w->out, "] >>\nendobj\n");

    begin_object(w, OBJ_INFO);
    buf_printf(&w->out, "<< /Title ");
    buf_put_string(&w->out, w->title);
    buf_printf(&w->out, " /Producer ");
    buf_put_string(&w->out, w->producer);
    buf_printf(&w->out, " >>\nendobj\n");

    // Cross-reference table: one 20-byte entry per object
    size_t xref = w->out.len;
    buf_printf(&w->out, "xref\n0 %u\n", (unsigned)w->n_objects);
    buf_printf(&w->out, "0000000000 65535 f\r\n");
    for (uint32_t i = 1; i < w->n_objects; i++)
        buf_printf(&w->out, "%010zu 00000 n\r\n", w->offsets[i]);
    buf_printf(&w->out, "trailer\n<< /Size %u /Root %d 0 R /Info %d 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
               (unsigned)w->n_objects, OBJ_CATALOG, OBJ_INFO, xref);
    if (w->out.failed)
        return false;

    // Hand the buffer over
    *out = w->out.p;
    *out_len = w->out.len;
    w->out.p = NULL;
    w->out.len = w->out.cap = 0;
    w->out.failed = true; // the writer is spent
    return true;
}

void pdf_writer_free(pdf_writer_t *w) {
    if (!w)
        return;
    free(w->out.p);
    free(w->offsets);
    free(w->page_objs);
    free(w->title);
    free(w->producer);
    free(w);
}
