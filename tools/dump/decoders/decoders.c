// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decoders.c
// Common helpers + 4-CC dispatcher for per-resource decoders.  Each
// individual decoder lives in its own decode_*.c file beside this one;
// the dispatch table here is the single place that knows the type
// universe.

#include "decoders.h"

#include "macroman.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void re_json_write_string(FILE *fp, const char *s) {
    fputc('"', fp);
    if (!s) {
        fputc('"', fp);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', fp);
            fputc(c, fp);
        } else if (c == '\n') {
            fputs("\\n", fp);
        } else if (c == '\r') {
            fputs("\\r", fp);
        } else if (c == '\t') {
            fputs("\\t", fp);
        } else if (c < 0x20) {
            fprintf(fp, "\\u%04x", c);
        } else {
            fputc(c, fp);
        }
    }
    fputc('"', fp);
}

size_t re_pstring_to_utf8(const uint8_t *bytes, size_t len, size_t off, char *out, size_t cap) {
    if (!bytes || !out || cap == 0)
        return 0;
    out[0] = '\0';
    if (off >= len)
        return 0;
    uint8_t plen = bytes[off];
    if (off + 1 + plen > len)
        return 0;
    macroman_to_utf8(bytes + off + 1, plen, out, cap);
    return 1 + (size_t)plen;
}

// Dispatcher: each row is a 4-CC + (decoder fn, name) pair.  Linear scan
// is fine — the table is ~12 entries and dispatch happens once per
// resource (not per byte).
typedef struct {
    const uint8_t cc[4];
    const char *name;
    re_decoder_fn fn;
} decoder_entry_t;

static const decoder_entry_t g_decoders[] = {
    {{'v', 'e', 'r', 's'}, "vers", re_decode_vers      },
    {{'S', 'T', 'R', ' '}, "STR",  re_decode_str_single},
    {{'S', 'T', 'R', '#'}, "STR#", re_decode_strlist   },
    {{'T', 'E', 'X', 'T'}, "TEXT", re_decode_text      },
    {{'M', 'E', 'N', 'U'}, "MENU", re_decode_menu      },
    {{'M', 'B', 'A', 'R'}, "MBAR", re_decode_mbar      },
    {{'D', 'L', 'O', 'G'}, "DLOG", re_decode_dlog      },
    {{'A', 'L', 'R', 'T'}, "ALRT", re_decode_alrt      },
    {{'D', 'I', 'T', 'L'}, "DITL", re_decode_ditl      },
    {{'S', 'I', 'Z', 'E'}, "SIZE", re_decode_size      },
    {{'B', 'N', 'D', 'L'}, "BNDL", re_decode_bndl      },
    {{'F', 'R', 'E', 'F'}, "FREF", re_decode_fref      },
};
static const size_t g_decoders_count = sizeof(g_decoders) / sizeof(g_decoders[0]);

static const decoder_entry_t *find_entry(const uint8_t type[4]) {
    for (size_t i = 0; i < g_decoders_count; i++) {
        if (memcmp(g_decoders[i].cc, type, 4) == 0)
            return &g_decoders[i];
    }
    return NULL;
}

const char *re_decode_dispatch_name(const uint8_t type[4]) {
    const decoder_entry_t *e = find_entry(type);
    return e ? e->name : NULL;
}

int re_decode_dispatch(const uint8_t type[4], const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    const decoder_entry_t *e = find_entry(type);
    if (!e)
        return 0;
    int rc = e->fn(bytes, len, json_fp, txt_fp);
    return (rc < 0) ? rc : 1;
}
