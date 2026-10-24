// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the dump tool's DITL decoder (tools/dump/decoders/
// decode_dialog.c).  Builds an item list by hand and checks that every
// item is found -- each item's data is a length byte plus the data padded
// to an even length, so a text of even length takes no pad byte -- and
// that icon and picture items report their resource ID.

#include "decoders.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Append one item: 4 reserved bytes, a rect, the type byte, then the data.
static size_t put_item(uint8_t *p, uint8_t type, const uint8_t *data, uint8_t len) {
    memset(p, 0, 12);
    p[5] = 10; // top = 10
    p[12] = type;
    p[13] = len;
    memcpy(p + 14, data, len);
    size_t n = 14 + len;
    if (len & 1)
        p[n++] = 0;
    return n;
}

// Run the decoder; returns its text summary (malloc'd) and the JSON in *json.
static char *decode(const uint8_t *bytes, size_t len, char **json) {
    char *jbuf = NULL, *tbuf = NULL;
    size_t jlen = 0, tlen = 0;
    FILE *jf = open_memstream(&jbuf, &jlen);
    FILE *tf = open_memstream(&tbuf, &tlen);
    re_decode_ditl(bytes, len, jf, tf);
    fclose(jf);
    fclose(tf);
    *json = jbuf;
    return tbuf;
}

// Even- and odd-length texts in a row, then an icon and a picture.
TEST(test_ditl_item_padding_and_ids) {
    uint8_t buf[256];
    size_t off = 2;
    buf[0] = 0;
    buf[1] = 4; // five items
    off += put_item(buf + off, 0x04, (const uint8_t *)"Finder", 6); // button, even
    off += put_item(buf + off, 0x04, (const uint8_t *)"Restart", 7); // button, odd
    off += put_item(buf + off, 0x10, (const uint8_t *)"W", 1); // edit text
    off += put_item(buf + off, 0xA0, (const uint8_t *)"\x07\xDA", 2); // icon 2010, disabled
    off += put_item(buf + off, 0xC0, (const uint8_t *)"\x7D\x00", 2); // picture 32000, disabled
    char *json = NULL;
    char *txt = decode(buf, off, &json);
    ASSERT_TRUE(strstr(txt, "DITL: 5 items") != NULL);
    ASSERT_TRUE(strstr(txt, "1: button       Finder\n") != NULL);
    ASSERT_TRUE(strstr(txt, "2: button       Restart\n") != NULL);
    ASSERT_TRUE(strstr(txt, "3: edit_text    W\n") != NULL);
    ASSERT_TRUE(strstr(txt, "4: icon         id 2010\n") != NULL);
    ASSERT_TRUE(strstr(txt, "5: pict         id 32000\n") != NULL);
    ASSERT_TRUE(strstr(json, "\"kind\":\"icon\",\"enabled\":false") != NULL);
    ASSERT_TRUE(strstr(json, "\"res_id\":2010}") != NULL);
    ASSERT_TRUE(strstr(json, "\"text\":\"Restart\"") != NULL);
    free(txt);
    free(json);
}

int main(void) {
    RUN(test_ditl_item_padding_and_ids);
    return 0;
}
