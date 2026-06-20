// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_dialog.c
// DLOG (dialog template), ALRT (alert template), and DITL (item list).
// All three follow Inside Macintosh: Toolbox Essentials layouts.
//
// DLOG:
//   +0   bounds         (Rect: 4×i16 BE = top,left,bottom,right)
//   +8   proc_id        (i16 BE)
//   +10  visible        (i16 BE — non-zero = visible)
//   +12  go_away        (i16 BE)
//   +14  ref_con        (i32 BE)
//   +18  ditl_id        (i16 BE)
//   +20  title          (pstring)
//
// ALRT:
//   +0   bounds         (Rect)
//   +8   ditl_id        (i16 BE)
//   +10  stages         (i16 BE) — packed sound/draw/font stage data
//
// DITL:
//   +0   item_count - 1 (i16 BE)
//   then per item:
//     +0  reserved (u32, must be 0)
//     +4  bounds   (Rect)
//     +12 item_type(u8) — bit 7 = enable, low bits = kind
//     +13 data (variable, depending on kind)

#include "decoders.h"

#include <stdio.h>

static void write_rect(FILE *fp, const uint8_t *p) {
    int16_t t = (int16_t)((p[0] << 8) | p[1]);
    int16_t l = (int16_t)((p[2] << 8) | p[3]);
    int16_t b = (int16_t)((p[4] << 8) | p[5]);
    int16_t r = (int16_t)((p[6] << 8) | p[7]);
    fprintf(fp, "[%d,%d,%d,%d]", t, l, b, r);
}

int re_decode_dlog(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 22) {
        fprintf(json_fp, "{\"error\":\"DLOG too short\"}\n");
        return -1;
    }
    int16_t proc_id = (int16_t)((bytes[8] << 8) | bytes[9]);
    int16_t visible = (int16_t)((bytes[10] << 8) | bytes[11]);
    int16_t go_away = (int16_t)((bytes[12] << 8) | bytes[13]);
    int32_t ref_con =
        (int32_t)(((uint32_t)bytes[14] << 24) | ((uint32_t)bytes[15] << 16) | ((uint32_t)bytes[16] << 8) | bytes[17]);
    int16_t ditl_id = (int16_t)((bytes[18] << 8) | bytes[19]);
    char title[256];
    re_pstring_to_utf8(bytes, len, 20, title, sizeof(title));

    fprintf(json_fp, "{\"bounds\":");
    write_rect(json_fp, bytes);
    fprintf(json_fp, ",\"proc_id\":%d,\"visible\":%s,\"go_away\":%s,\"ref_con\":%d,\"ditl_id\":%d,\"title\":", proc_id,
            visible ? "true" : "false", go_away ? "true" : "false", ref_con, ditl_id);
    re_json_write_string(json_fp, title);
    fprintf(json_fp, ",\"refs\":[{\"type\":\"DITL\",\"id\":%d}]}\n", ditl_id);

    if (txt_fp)
        fprintf(txt_fp, "DLOG: title=%s ditl=%d visible=%d\n", title, ditl_id, visible);
    return 0;
}

int re_decode_alrt(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 12) {
        fprintf(json_fp, "{\"error\":\"ALRT too short\"}\n");
        return -1;
    }
    int16_t ditl_id = (int16_t)((bytes[8] << 8) | bytes[9]);
    uint16_t stages = (uint16_t)((bytes[10] << 8) | bytes[11]);
    fprintf(json_fp, "{\"bounds\":");
    write_rect(json_fp, bytes);
    fprintf(json_fp, ",\"ditl_id\":%d,\"stages\":\"0x%04x\",\"refs\":[{\"type\":\"DITL\",\"id\":%d}]}\n", ditl_id,
            stages, ditl_id);
    if (txt_fp)
        fprintf(txt_fp, "ALRT: ditl=%d stages=0x%04x\n", ditl_id, stages);
    return 0;
}

static const char *ditl_kind_name(uint8_t kind) {
    // Low 7 bits of item_type byte. Reference: Inside Macintosh: Toolbox
    // Essentials, "Item Type Constants".
    switch (kind & 0x7F) {
    case 0:
        return "user";
    case 1:
        return "help";
    case 4:
        return "button";
    case 5:
        return "checkbox";
    case 6:
        return "radio";
    case 7:
        return "control";
    case 8:
        return "static_text";
    case 9:
        return "edit_text";
    case 16:
        return "icon";
    case 32:
        return "pict";
    default:
        return "unknown";
    }
}

int re_decode_ditl(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 2) {
        fprintf(json_fp, "{\"error\":\"DITL too short\"}\n");
        return -1;
    }
    int16_t count_m1 = (int16_t)((bytes[0] << 8) | bytes[1]);
    int n_items = (int)count_m1 + 1;
    fprintf(json_fp, "{\"count\":%d,\"items\":[", n_items);
    if (txt_fp)
        fprintf(txt_fp, "DITL: %d items\n", n_items);
    size_t off = 2;
    bool first = true;
    for (int i = 0; i < n_items && off < len; i++) {
        if (off + 13 > len)
            break;
        // Skip 4-byte reserved.
        const uint8_t *rect_p = bytes + off + 4;
        uint8_t type_byte = bytes[off + 12];
        off += 13;
        uint8_t plen = (off < len) ? bytes[off] : 0;
        char text[256] = {0};
        if (plen > 0 && off + 1 + plen <= len)
            re_pstring_to_utf8(bytes, len, off, text, sizeof(text));
        size_t data_take = 1 + plen;
        if (data_take & 1)
            data_take++; // pad to even
        off += data_take;

        if (!first)
            fputc(',', json_fp);
        first = false;
        bool enabled = (type_byte & 0x80) == 0;
        fprintf(json_fp, "{\"index\":%d,\"kind\":\"%s\",\"enabled\":%s,\"bounds\":", i + 1, ditl_kind_name(type_byte),
                enabled ? "true" : "false");
        write_rect(json_fp, rect_p);
        fprintf(json_fp, ",\"text\":");
        re_json_write_string(json_fp, text);
        fprintf(json_fp, "}");
        if (txt_fp)
            fprintf(txt_fp, "  %2d: %-12s %s\n", i + 1, ditl_kind_name(type_byte), text);
    }
    fprintf(json_fp, "]}\n");
    return 0;
}
