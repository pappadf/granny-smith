// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The checkpoint manifest's image list.  See Makefile.

#include "checkpoint_machine.h"
#include "image.h"
#include "system.h"
#include "system_internal.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// checkpoint_machine.c reads the machine's images through system.c's
// accessors; this suite stands in for system.c with a list of its own.
static config_t g_cfg; // zeroed: no machine profile, so no model/RAM fields
static image_t *g_images[8];
static int g_n_images;
config_t *global_emulator = NULL;

image_t *config_get_image(const config_t *cfg, int index) {
    return cfg && index >= 0 && index < g_n_images ? g_images[index] : NULL;
}
int config_get_n_images(const config_t *cfg) {
    return cfg ? g_n_images : 0;
}

const char *build_id_get(void) {
    return "test-build";
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// The image list is well-formed however many images there are and whichever
// slots are empty: the first entry after an empty slot 0 used to be written
// with a leading comma ("[,"), and the hand-grown buffer overran on a
// failed realloc.  Enough long paths are attached here to need several
// growths of the old 256-byte buffer.
TEST(test_image_list_is_well_formed) {
    char root[64] = "/tmp/cp_manifest_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    checkpoint_machine_set_root(root);
    ASSERT_EQ_INT(0, checkpoint_machine_set("m1", "20260923"));
    // The machine directory exists now: moving the root under it is refused.
    ASSERT_EQ_INT(-1, checkpoint_machine_set_root("/elsewhere"));

    static image_t imgs[6];
    static char names[6][300];
    g_images[0] = NULL; // an empty slot first
    for (int i = 1; i <= 5; i++) {
        memset(names[i], 'a' + i, 280);
        names[i][0] = '/';
        names[i][280] = '\0';
        imgs[i].filename = names[i];
        imgs[i].raw_size = 512u * (unsigned)i;
        g_images[i] = &imgs[i];
    }
    g_n_images = 6;

    ASSERT_EQ_INT(0, checkpoint_machine_write_manifest(&g_cfg));
    char path[128];
    snprintf(path, sizeof(path), "%s/manifest.json", checkpoint_machine_dir());
    const char *text = read_text(path);
    ASSERT_TRUE(strstr(text, "\"images\": [\n    { \"index\": 1,") != NULL);
    ASSERT_TRUE(strstr(text, "[,") == NULL);
    ASSERT_TRUE(strstr(text, "\"index\": 5,") != NULL);
    ASSERT_TRUE(strstr(text, "}\n  ]\n}\n") != NULL);

    // An empty list closes cleanly too.
    g_n_images = 0;
    ASSERT_EQ_INT(0, checkpoint_machine_write_manifest(&g_cfg));
    text = read_text(path);
    ASSERT_TRUE(strstr(text, "\"images\": []\n}\n") != NULL);

    remove(path);
    rmdir(checkpoint_machine_dir());
    rmdir(root);
}

int main(void) {
    RUN(test_image_list_is_well_formed);
    fprintf(stderr, "All checkpoint_manifest tests passed\n");
    return 0;
}
