// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_menu.c
// MENU (menu definition) and MBAR (array of menu IDs).
//
// MENU layout (Inside Macintosh: Menus):
//   +0   menu_id          (i16 BE)
//   +2   width            (i16 BE) — runtime cached, usually 0 on disk
//   +4   height           (i16 BE) — runtime cached, usually 0 on disk
//   +6   menu_proc        (i16 BE) — MDEF id (0 = standard)
//   +8   reserved (filler)
//   +10  enable_flags     (u32 BE) — per-item enable bits
//   +14  title            (pstring, MacRoman)
//   then per-item:
//     +0 text             (pstring)
//     +1 icon             (u8)
//     +2 cmd_key          (u8) — ASCII or 0
//     +3 mark             (u8)
//     +4 style            (u8)
//   Item list ends with a zero length byte.
//
// MBAR layout: u16 BE count, then count u16 BE menu IDs.

#include "decoders.h"

#include <stdio.h>

int re_decode_menu(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 14) {
        fprintf(json_fp, "{\"error\":\"MENU too short\"}\n");
        return -1;
    }
    int16_t menu_id = (int16_t)((bytes[0] << 8) | bytes[1]);
    int16_t menu_proc = (int16_t)((bytes[6] << 8) | bytes[7]);
    uint32_t enable_flags =
        ((uint32_t)bytes[10] << 24) | ((uint32_t)bytes[11] << 16) | ((uint32_t)bytes[12] << 8) | bytes[13];

    char title[256];
    size_t off = 14;
    size_t consumed = re_pstring_to_utf8(bytes, len, off, title, sizeof(title));
    off += consumed;

    fprintf(json_fp, "{\"menu_id\":%d,\"menu_proc\":%d,\"enable_flags\":\"0x%08x\",\"title\":", menu_id, menu_proc,
            enable_flags);
    re_json_write_string(json_fp, title);
    fprintf(json_fp, ",\"items\":[");
    if (txt_fp)
        fprintf(txt_fp, "MENU %d: %s\n", menu_id, title);

    bool first = true;
    while (off < len) {
        uint8_t plen = bytes[off];
        if (plen == 0)
            break; // terminator
        char text[256];
        size_t took = re_pstring_to_utf8(bytes, len, off, text, sizeof(text));
        if (took == 0)
            break;
        off += took;
        if (off + 4 > len)
            break;
        uint8_t icon = bytes[off + 0];
        uint8_t cmd = bytes[off + 1];
        uint8_t mark = bytes[off + 2];
        uint8_t style = bytes[off + 3];
        off += 4;
        if (!first)
            fputc(',', json_fp);
        first = false;
        fprintf(json_fp, "{\"text\":");
        re_json_write_string(json_fp, text);
        fprintf(json_fp, ",\"icon\":%u,\"cmd_key\":%u,\"mark\":%u,\"style\":\"0x%02x\"}", icon, cmd, mark, style);
        if (txt_fp) {
            fprintf(txt_fp, "  %-30s", text);
            if (cmd >= 0x20 && cmd < 0x7F)
                fprintf(txt_fp, " (cmd-%c)", cmd);
            fputc('\n', txt_fp);
        }
    }
    fprintf(json_fp, "]}\n");
    return 0;
}

int re_decode_mbar(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 2) {
        fprintf(json_fp, "{\"error\":\"MBAR too short\"}\n");
        return -1;
    }
    uint16_t n = (uint16_t)((bytes[0] << 8) | bytes[1]);
    fprintf(json_fp, "{\"count\":%u,\"menus\":[", n);
    for (uint16_t i = 0; i < n; i++) {
        if (2 + i * 2 + 2 > len)
            break;
        int16_t mid = (int16_t)((bytes[2 + i * 2] << 8) | bytes[2 + i * 2 + 1]);
        if (i > 0)
            fputc(',', json_fp);
        fprintf(json_fp, "{\"type\":\"MENU\",\"id\":%d}", mid);
        if (txt_fp)
            fprintf(txt_fp, "MENU %d\n", mid);
    }
    fprintf(json_fp, "]}\n");
    return 0;
}
