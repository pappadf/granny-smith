// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_vers.c
// `vers` resource: short + long version strings + BCD-encoded version
// number + region.  Layout (per Inside Macintosh: More Macintosh Toolbox):
//   +0 major BCD (u8) — high nybble.low nybble = 1.0 etc.
//   +1 minor BCD (u8)
//   +2 release stage (u8) — 0x20=dev, 0x40=alpha, 0x60=beta, 0x80=final
//   +3 prerelease build (u8)
//   +4 region (u16 BE) — 0=USA, 1=France, etc.
//   +6 short version (pstring, MacRoman)
//   +N long version (pstring, MacRoman)
//
// We dump both pstrings as UTF-8 plus the structured fields as JSON.

#include "decoders.h"

#include <stdio.h>

int re_decode_vers(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 7) {
        fprintf(json_fp, "{\"error\":\"vers too short: %zu bytes\"}\n", len);
        return -1;
    }
    uint8_t major_bcd = bytes[0];
    uint8_t minor_bcd = bytes[1];
    uint8_t stage = bytes[2];
    uint8_t prerel = bytes[3];
    uint16_t region = (uint16_t)((bytes[4] << 8) | bytes[5]);

    char short_str[256];
    size_t off = 6;
    size_t consumed = re_pstring_to_utf8(bytes, len, off, short_str, sizeof(short_str));
    off += consumed;
    char long_str[256];
    long_str[0] = '\0';
    if (off < len)
        re_pstring_to_utf8(bytes, len, off, long_str, sizeof(long_str));

    // BCD decode: major/10*10 + major%10 for two BCD nybbles.
    int major = ((major_bcd >> 4) * 10) + (major_bcd & 0x0F);
    int minor = (minor_bcd >> 4) & 0x0F;
    int bug = minor_bcd & 0x0F;
    const char *stage_str = "unknown";
    switch (stage) {
    case 0x20:
        stage_str = "dev";
        break;
    case 0x40:
        stage_str = "alpha";
        break;
    case 0x60:
        stage_str = "beta";
        break;
    case 0x80:
        stage_str = "final";
        break;
    }

    fprintf(json_fp, "{\"major\":%d,\"minor\":%d,\"bug\":%d,\"stage\":\"%s\",\"prerelease\":%u,\"region\":%u,", major,
            minor, bug, stage_str, prerel, region);
    fprintf(json_fp, "\"short\":");
    re_json_write_string(json_fp, short_str);
    fprintf(json_fp, ",\"long\":");
    re_json_write_string(json_fp, long_str);
    fprintf(json_fp, "}\n");

    if (txt_fp) {
        fprintf(txt_fp, "%d.%d.%d %s (region %u)\n", major, minor, bug, stage_str, region);
        if (short_str[0])
            fprintf(txt_fp, "short: %s\n", short_str);
        if (long_str[0])
            fprintf(txt_fp, "long:  %s\n", long_str);
    }
    return 0;
}
