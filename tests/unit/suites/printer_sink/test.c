// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// printer_safe_title and printer_file_name (printer_sink.h): the names a
// print job's PDF and capture get, the same on headless and in the browser.
// The rule is app/web2's documentName (platenRing.ts), which names the
// LaserWriter's PDF in the browser.

#include "printer_sink.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// One title and the safe form it reduces to.
typedef struct {
    const char *title;
    const char *safe;
} title_vector_t;

static const title_vector_t k_titles[] = {
    {"Print",                "Print"       },
    {"Macintosh HD",         "Macintosh_HD"},
    // A run of characters outside [A-Za-z0-9.-] is one '_', not one per byte
    // (the browser's ImageWriter used to write "My__Doc")
    {"My  Doc",              "My_Doc"      },
    {"a/b\\c:d",             "a_b_c_d"     },
    {"  leading",            "leading"     },
    {"trailing  ",           "trailing"    },
    {"under_score",          "under_score" },
    {"r\xC3\xA9sum\xC3\xA9", "r_sum"       },
    {"v1.2-final",           "v1.2-final"  },
    {"",                     "untitled"    },
    {"!!!",                  "untitled"    },
};

TEST(safe_titles_collapse_runs) {
    for (size_t i = 0; i < sizeof k_titles / sizeof k_titles[0]; i++) {
        char out[PRINTER_TITLE_MAX + 1];
        printer_safe_title(out, sizeof out, k_titles[i].title);
        if (strcmp(out, k_titles[i].safe) != 0)
            fprintf(stderr, "       '%s' -> '%s', expected '%s'\n", k_titles[i].title, out, k_titles[i].safe);
        ASSERT_EQ_INT(0, strcmp(out, k_titles[i].safe));
    }
}

TEST(safe_titles_are_bounded) {
    char longer[200];
    memset(longer, 'x', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    char out[PRINTER_TITLE_MAX + 1];
    ASSERT_EQ_INT(PRINTER_TITLE_MAX, (int)printer_safe_title(out, sizeof out, longer));
    ASSERT_EQ_INT(PRINTER_TITLE_MAX, (int)strlen(out));
    char small[4];
    printer_safe_title(small, sizeof small, "abcdef");
    ASSERT_EQ_INT(0, strcmp(small, "abc"));
}

TEST(file_names) {
    char name[128];
    // The ImageWriter's PDF and capture carry the printer's slug
    printer_file_name(name, sizeof name, "imagewriter2", 1, "My  Doc", "pdf");
    ASSERT_EQ_INT(0, strcmp(name, "imagewriter2-00001-My_Doc.pdf"));
    printer_file_name(name, sizeof name, "imagewriter2", 12, NULL, "iw");
    ASSERT_EQ_INT(0, strcmp(name, "imagewriter2-00012.iw"));
    // The LaserWriter's carry none
    printer_file_name(name, sizeof name, NULL, 4, "Macintosh HD", "pdf");
    ASSERT_EQ_INT(0, strcmp(name, "00004-Macintosh_HD.pdf"));
    printer_file_name(name, sizeof name, NULL, 4, NULL, "ps");
    ASSERT_EQ_INT(0, strcmp(name, "00004.ps"));
    printer_file_name(name, sizeof name, "", 7, "", "pdf");
    ASSERT_EQ_INT(0, strcmp(name, "00007-untitled.pdf"));
}

int main(void) {
    RUN(safe_titles_collapse_runs);
    RUN(safe_titles_are_bounded);
    RUN(file_names);
    fprintf(stderr, "All printer_sink tests passed.\n");
    return 0;
}
