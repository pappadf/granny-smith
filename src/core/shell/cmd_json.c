// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_json.c
// JSON serializer for cmd_result. Produces a JSON object for the JS bridge.

#include "cmd_json.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Write a JSON-escaped string into buf. Returns number of bytes written.
static int json_quote_string(char *buf, int buflen, const char *str) {
    if (!str) {
        return snprintf(buf, buflen, "\"\"");
    }

    int off = 0;
    if (off < buflen)
        buf[off++] = '"';

    for (const char *p = str; *p && off < buflen - 2; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':
            if (off + 2 < buflen) {
                buf[off++] = '\\';
                buf[off++] = '"';
            }
            break;
        case '\\':
            if (off + 2 < buflen) {
                buf[off++] = '\\';
                buf[off++] = '\\';
            }
            break;
        case '\n':
            if (off + 2 < buflen) {
                buf[off++] = '\\';
                buf[off++] = 'n';
            }
            break;
        case '\r':
            if (off + 2 < buflen) {
                buf[off++] = '\\';
                buf[off++] = 'r';
            }
            break;
        case '\t':
            if (off + 2 < buflen) {
                buf[off++] = '\\';
                buf[off++] = 't';
            }
            break;
        default:
            if (c < 0x20) {
                // Control character: \u00XX
                if (off + 6 < buflen)
                    off += snprintf(buf + off, buflen - off, "\\u%04x", c);
            } else {
                buf[off++] = c;
            }
            break;
        }
    }

    if (off < buflen)
        buf[off++] = '"';
    if (off < buflen)
        buf[off] = '\0';
    return off;
}

// Serialize cmd_result to JSON into buf
void cmd_result_to_json(const struct cmd_result *res, char *buf, int buflen) {
    if (!res || !buf || buflen <= 0)
        return;

    int off = 0;
    off += snprintf(buf + off, buflen - off, "{\"status\":");

    if (res->type == RES_ERR) {
        off += snprintf(buf + off, buflen - off, "\"error\",\"error\":");
        off += json_quote_string(buf + off, buflen - off, res->as_str);
    } else {
        off += snprintf(buf + off, buflen - off, "\"ok\"");

        // Typed value
        switch (res->type) {
        case RES_INT:
            off += snprintf(buf + off, buflen - off, ",\"type\":\"int\",\"value\":%" PRId64, res->as_int);
            break;
        case RES_STR:
            off += snprintf(buf + off, buflen - off, ",\"type\":\"str\",\"value\":");
            off += json_quote_string(buf + off, buflen - off, res->as_str);
            break;
        case RES_BOOL:
            off +=
                snprintf(buf + off, buflen - off, ",\"type\":\"bool\",\"value\":%s", res->as_bool ? "true" : "false");
            break;
        case RES_OK:
            off += snprintf(buf + off, buflen - off, ",\"type\":\"ok\"");
            break;
        default:
            break;
        }
    }

    // Captured output (always included for programmatic calls)
    if (res->output && res->output_len > 0) {
        off += snprintf(buf + off, buflen - off, ",\"output\":");
        off += json_quote_string(buf + off, buflen - off, res->output);
    }
    if (res->error_output && res->error_len > 0) {
        off += snprintf(buf + off, buflen - off, ",\"stderr\":");
        off += json_quote_string(buf + off, buflen - off, res->error_output);
    }

    snprintf(buf + off, buflen - off, "}");
}
