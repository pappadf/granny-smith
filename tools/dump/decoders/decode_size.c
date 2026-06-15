// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decode_size.c
// SIZE resource — MultiFinder application size flags.
//
// Layout:
//   +0   flags    (u16 BE) — see bit definitions below
//   +2   pref_mem (u32 BE) — preferred memory size, bytes
//   +6   min_mem  (u32 BE) — minimum memory size, bytes

#include "decoders.h"

#include <stdio.h>

// Bit definitions per Inside Macintosh: MultiFinder.
static const struct {
    uint16_t bit;
    const char *name;
} size_bits[] = {
    {0x8000, "save_screen"           },
    {0x4000, "accepts_suspend_events"},
    {0x2000, "disable_option"        },
    {0x1000, "cannot_background"     },
    {0x0800, "needs_multifinder"     },
    {0x0400, "32_bit_compatible"     },
    {0x0200, "high_level_events"     },
    {0x0100, "local_appletalk"       },
    {0x0080, "remote_appletalk"      },
    {0x0040, "stationery_aware"      },
    {0x0020, "use_text_edit_services"},
    {0x0010, "notification_aware"    },
};
static const size_t size_bits_count = sizeof(size_bits) / sizeof(size_bits[0]);

int re_decode_size(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp) {
    if (len < 10) {
        fprintf(json_fp, "{\"error\":\"SIZE too short\"}\n");
        return -1;
    }
    uint16_t flags = (uint16_t)((bytes[0] << 8) | bytes[1]);
    uint32_t pref = ((uint32_t)bytes[2] << 24) | ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 8) | bytes[5];
    uint32_t mini = ((uint32_t)bytes[6] << 24) | ((uint32_t)bytes[7] << 16) | ((uint32_t)bytes[8] << 8) | bytes[9];
    fprintf(json_fp, "{\"flags\":\"0x%04x\",\"preferred_size\":%u,\"minimum_size\":%u,\"flag_names\":[", flags, pref,
            mini);
    bool first = true;
    for (size_t i = 0; i < size_bits_count; i++) {
        if (flags & size_bits[i].bit) {
            if (!first)
                fputc(',', json_fp);
            first = false;
            fprintf(json_fp, "\"%s\"", size_bits[i].name);
        }
    }
    fprintf(json_fp, "]}\n");
    if (txt_fp)
        fprintf(txt_fp, "flags=0x%04x pref=%u min=%u\n", flags, pref, mini);
    return 0;
}
