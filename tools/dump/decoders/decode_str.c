// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_str.c
// `STR ` (single Pascal string), `STR#` (counted list of Pascal strings),
// and `TEXT` (raw text, no length prefix).  All transcode MacRoman to
// UTF-8 in their output.

#include "decoders.h"

#include "macroman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int re_decode_str_single(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    char s[1024];
    s[0] = '\0';
    if (len > 0)
        re_pstring_to_utf8(bytes, len, 0, s, sizeof(s));
    fprintf(json_fp, "{\"text\":");
    re_json_write_string(json_fp, s);
    fprintf(json_fp, "}\n");
    if (txt_fp)
        fprintf(txt_fp, "%s\n", s);
    return 0;
}

int re_decode_strlist(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 2) {
        fprintf(json_fp, "{\"error\":\"STR# too short\"}\n");
        return -1;
    }
    uint16_t n = (uint16_t)((bytes[0] << 8) | bytes[1]);
    size_t off = 2;
    fprintf(json_fp, "{\"count\":%u,\"items\":[", n);
    for (uint16_t i = 0; i < n; i++) {
        if (off >= len)
            break;
        char s[1024];
        size_t consumed = re_pstring_to_utf8(bytes, len, off, s, sizeof(s));
        if (consumed == 0)
            break;
        if (i > 0)
            fputc(',', json_fp);
        re_json_write_string(json_fp, s);
        if (txt_fp)
            fprintf(txt_fp, "%s\n", s);
        off += consumed;
    }
    fprintf(json_fp, "]}\n");
    return 0;
}

int re_decode_text(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    // TEXT has no length prefix — the whole buffer is the text.
    // Allocate enough UTF-8 storage for the worst case (3 bytes per byte).
    char *utf8 = (char *)malloc(len * 3 + 1);
    if (!utf8) {
        fprintf(json_fp, "{\"error\":\"oom\"}\n");
        return -1;
    }
    macroman_to_utf8(bytes, len, utf8, len * 3 + 1);
    fprintf(json_fp, "{\"length\":%zu,\"text\":", len);
    re_json_write_string(json_fp, utf8);
    fprintf(json_fp, "}\n");
    if (txt_fp)
        fputs(utf8, txt_fp);
    free(utf8);
    return 0;
}
