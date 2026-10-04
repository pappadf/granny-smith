// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pdf_writer.h
// A minimal PDF writer: one image per page, filling the page.  Pages are
// compressed as they are added, so a caller can free each page's raster
// before building the next; the document is serialised at the end.  The
// output carries no dates or random identifiers, so the same pages always
// give the same bytes.  Nothing here knows about printers.

#ifndef PDF_WRITER_H
#define PDF_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pdf_writer pdf_writer_t;

// A new, empty document.  `title` and `producer` go into the Info
// dictionary (either may be NULL or "").  NULL when memory runs out.
pdf_writer_t *pdf_writer_new(const char *title, const char *producer);

// A page `width_pt` x `height_pt` points whose content is a 1-bit image of
// px_w x px_h pixels, rows top to bottom, `stride` bytes apart, the most
// significant bit leftmost.  A set bit is ink: the image is painted as a
// stencil mask in black.
bool pdf_writer_add_mono_page(pdf_writer_t *w, double width_pt, double height_pt, uint32_t px_w, uint32_t px_h,
                              const uint8_t *bits, size_t stride);

// The same with a 4-bit indexed image: two pixels per byte, the high
// nibble leftmost, each an index into `palette_rgb`.
bool pdf_writer_add_indexed4_page(pdf_writer_t *w, double width_pt, double height_pt, uint32_t px_w, uint32_t px_h,
                                  const uint8_t *nibbles, size_t stride, const uint8_t palette_rgb[16][3]);

// Replace the title given to pdf_writer_new.
void pdf_writer_set_title(pdf_writer_t *w, const char *title);

// The writer's state as one buffer (a document in progress, for a
// checkpoint).  *out is malloc'd.  False when memory runs out.
bool pdf_writer_serialise(const pdf_writer_t *w, uint8_t **out, size_t *out_len);

// A writer rebuilt from pdf_writer_serialise's output; NULL when the data
// is malformed.
pdf_writer_t *pdf_writer_deserialise(const uint8_t *data, size_t len);

// Pages added so far.
uint32_t pdf_writer_pages(const pdf_writer_t *w);

// Serialise the document.  *out is malloc'd and owned by the caller.
// False when memory runs out.
bool pdf_writer_finish(pdf_writer_t *w, uint8_t **out, size_t *out_len);

// Free the writer (after finish, or to abandon the document).
void pdf_writer_free(pdf_writer_t *w);

#endif // PDF_WRITER_H
