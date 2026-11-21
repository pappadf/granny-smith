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
    int16_t t = RD_BE16S(p);
    int16_t l = RD_BE16S(p + 2);
    int16_t b = RD_BE16S(p + 4);
    int16_t r = RD_BE16S(p + 6);
    fprintf(fp, "[%d,%d,%d,%d]", t, l, b, r);
}

int re_decode_dlog(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 22) {
        fprintf(json_fp, "{\"error\":\"DLOG too short\"}\n");
        return -1;
    }
    int16_t proc_id = RD_BE16S(bytes + 8);
    int16_t visible = RD_BE16S(bytes + 10);
    int16_t go_away = RD_BE16S(bytes + 12);
    int32_t ref_con = (int32_t)RD_BE32(bytes + 14);
    int16_t ditl_id = RD_BE16S(bytes + 18);
    char title[256];
    re_pstring_to_utf8(bytes, len, 20, title, sizeof(title));

    fprintf(json_fp, "{\"bounds\":");
    write_rect(json_fp, bytes);
    fprintf(json_fp, ",\"proc_id\":%d,\"visible\":%s,\"go_away\":%s,\"ref_con\":%d,\"ditl_id\":%d,\"title\":", proc_id,
            visible ? "true" : "false", go_away ? "true" : "false", ref_con, ditl_id);
    json_fputs(json_fp, title);
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
    int16_t ditl_id = RD_BE16S(bytes + 8);
    uint16_t stages = RD_BE16(bytes + 10);
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
    case 16:
        return "edit_text";
    case 32:
        return "icon";
    case 64:
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
    int16_t count_m1 = RD_BE16S(bytes);
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
        // Control, icon and picture items carry a 2-byte resource ID, not text.
        uint8_t kind = type_byte & 0x7F;
        bool has_res_id = (kind == 7 || kind == 32 || kind == 64) && plen == 2 && off + 3 <= len;
        int16_t res_id = has_res_id ? RD_BE16S(&bytes[off + 1]) : 0;
        if (has_res_id)
            snprintf(text, sizeof(text), "id %d", res_id);
        else if (plen > 0 && off + 1 + plen <= len)
            re_pstring_to_utf8(bytes, len, off, text, sizeof(text));
        // The length byte, then the data padded to an even length.
        off += 1 + (size_t)plen + (plen & 1);

        if (!first)
            fputc(',', json_fp);
        first = false;
        bool enabled = (type_byte & 0x80) == 0;
        fprintf(json_fp, "{\"index\":%d,\"kind\":\"%s\",\"enabled\":%s,\"bounds\":", i + 1, ditl_kind_name(type_byte),
                enabled ? "true" : "false");
        write_rect(json_fp, rect_p);
        if (has_res_id) {
            fprintf(json_fp, ",\"res_id\":%d}", res_id);
        } else {
            fprintf(json_fp, ",\"text\":");
            json_fputs(json_fp, text);
            fprintf(json_fp, "}");
        }
        if (txt_fp)
            fprintf(txt_fp, "  %2d: %-12s %s\n", i + 1, ditl_kind_name(type_byte), text);
    }
    fprintf(json_fp, "]}\n");
    return 0;
}
