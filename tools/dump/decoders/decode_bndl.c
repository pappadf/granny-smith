// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_bndl.c
// BNDL (Bundle) and FREF (File Reference) resources.
//
// BNDL layout (Inside Macintosh: Toolbox Essentials):
//   +0   signature (OSType, 4 bytes) — usually creator code
//   +4   res_id  (i16 BE) — id of the FREF/ICN# pair (typically 0)
//   +6   count - 1 (i16 BE) — number of mapping array elements
//   then mapping entries:
//     +0  local type (OSType, 4 bytes)
//     +4  count - 1 (i16 BE) — number of (local_id, on_disk_id) pairs
//     +6  pairs of i16 BE local + i16 BE on_disk
//
// FREF layout:
//   +0   file_type (OSType)
//   +4   icon_local_id (i16 BE) — index into BNDL's ICN# mapping
//   +6   name (pstring)

#include "decoders.h"

#include <stdio.h>
#include <string.h>

static void write_ostype(FILE *fp, const uint8_t *p) {
    bool ascii = true;
    for (int i = 0; i < 4; i++)
        if (p[i] < 0x20 || p[i] > 0x7E)
            ascii = false;
    if (ascii)
        fprintf(fp, "\"%c%c%c%c\"", p[0], p[1], p[2], p[3]);
    else
        fprintf(fp, "\"0x%02x%02x%02x%02x\"", p[0], p[1], p[2], p[3]);
}

int re_decode_bndl(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 8) {
        fprintf(json_fp, "{\"error\":\"BNDL too short\"}\n");
        return -1;
    }
    fprintf(json_fp, "{\"signature\":");
    write_ostype(json_fp, bytes);
    int16_t res_id = (int16_t)((bytes[4] << 8) | bytes[5]);
    int16_t group_count_m1 = (int16_t)((bytes[6] << 8) | bytes[7]);
    int n_groups = (int)group_count_m1 + 1;
    fprintf(json_fp, ",\"res_id\":%d,\"groups\":[", res_id);
    if (txt_fp) {
        fprintf(txt_fp, "BNDL res_id=%d  signature=%c%c%c%c\n", res_id, bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    size_t off = 8;
    bool gfirst = true;
    for (int g = 0; g < n_groups && off + 6 <= len; g++) {
        const uint8_t *type = bytes + off;
        off += 4;
        int16_t cm1 = (int16_t)((bytes[off] << 8) | bytes[off + 1]);
        off += 2;
        int n_pairs = (int)cm1 + 1;
        if (!gfirst)
            fputc(',', json_fp);
        gfirst = false;
        fprintf(json_fp, "{\"type\":");
        write_ostype(json_fp, type);
        fprintf(json_fp, ",\"mappings\":[");
        bool pfirst = true;
        for (int p = 0; p < n_pairs && off + 4 <= len; p++) {
            int16_t local = (int16_t)((bytes[off] << 8) | bytes[off + 1]);
            int16_t on_disk = (int16_t)((bytes[off + 2] << 8) | bytes[off + 3]);
            off += 4;
            if (!pfirst)
                fputc(',', json_fp);
            pfirst = false;
            fprintf(json_fp, "{\"local_id\":%d,\"resource_id\":%d}", local, on_disk);
        }
        fprintf(json_fp, "]}");
    }
    fprintf(json_fp, "]}\n");
    return 0;
}

int re_decode_fref(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 7) {
        fprintf(json_fp, "{\"error\":\"FREF too short\"}\n");
        return -1;
    }
    int16_t icon_id = (int16_t)((bytes[4] << 8) | bytes[5]);
    char name[256];
    re_pstring_to_utf8(bytes, len, 6, name, sizeof(name));
    fprintf(json_fp, "{\"file_type\":");
    write_ostype(json_fp, bytes);
    fprintf(json_fp, ",\"icon_local_id\":%d,\"name\":", icon_id);
    re_json_write_string(json_fp, name);
    fprintf(json_fp, "}\n");
    if (txt_fp)
        fprintf(txt_fp, "FREF: type=%c%c%c%c icon_local=%d name=%s\n", bytes[0], bytes[1], bytes[2], bytes[3], icon_id,
                name);
    return 0;
}
