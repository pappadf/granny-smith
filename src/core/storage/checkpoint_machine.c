// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_machine.c
// Implementation of per-machine checkpoint directory bookkeeping.

#include "checkpoint_machine.h"

#include "build_id.h"
#include "common.h"
#include "image.h"
#include "log.h"
#include "storage_util.h"
#include "system_config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// One category for the whole checkpoint path.  This file used "checkpoint"
// while checkpoint.c uses "ckpt" and system.c reached for "ckpt" inline, so
// `debug.log checkpoint 2` turned up a third of the subsystem and the other
// two thirds stayed silent.
LOG_USE_CATEGORY_NAME("ckpt")

static char *g_machine_id = NULL;
static char *g_machine_created = NULL;
static char *g_machine_dir = NULL;
static char *g_machine_root = NULL; // defaults to "/opfs/checkpoints"

static const char *machine_root(void) {
    return g_machine_root ? g_machine_root : "/opfs/checkpoints";
}

void checkpoint_machine_set_root(const char *root) {
    free(g_machine_root);
    g_machine_root = root ? gs_strdup(root) : NULL;
    // Recompute machine dir if id+created already set.
    if (g_machine_id && g_machine_created) {
        free(g_machine_dir);
        g_machine_dir = gs_str_printf("%s/%s-%s", machine_root(), g_machine_id, g_machine_created);
    }
}

// Undo a partial checkpoint_machine_set so a retry is possible, putting
// back the directory that was in place before it (a verbatim
// checkpoint_machine_set_dir, or none).
static void checkpoint_machine_forget_identity(char *prev_dir) {
    free(g_machine_id);
    free(g_machine_created);
    free(g_machine_dir);
    g_machine_id = NULL;
    g_machine_created = NULL;
    g_machine_dir = prev_dir;
}

int checkpoint_machine_set(const char *machine_id, const char *created) {
    if (!machine_id || !*machine_id || !created || !*created)
        return -1;
    if (g_machine_id) {
        // Already set — second call is a programming error.  Tolerate
        // exact-match repeat (idempotent for tests that re-init).
        if (strcmp(g_machine_id, machine_id) == 0 && strcmp(g_machine_created, created) == 0)
            return 0;
        LOG(1, "checkpoint_machine_set: refusing to rotate id mid-process (had %s-%s, asked %s-%s)", g_machine_id,
            g_machine_created, machine_id, created);
        return -1;
    }
    // Every failure below rolls the identity back.
    //
    // It used to assign the globals FIRST and then return -1 from four later
    // points with them populated and g_machine_dir unset -- after which the
    // "at most once per process" guard above rejected every subsequent call.
    // So a transient OPFS mkdir failure left the process with no machine
    // directory and no way to establish one: quick checkpoints and image
    // deltas disabled for the session, behind a single level-1 log line, and
    // the documented recovery is a page reload.
    //
    // A directory set verbatim (checkpoint_machine_set_dir, headless
    // --checkpoint-dir) is replaced by the identity's own; it was overwritten
    // without being freed.  Keep it until the new one exists.
    char *prev_dir = g_machine_dir;
    g_machine_dir = NULL;
    g_machine_id = gs_strdup(machine_id);
    g_machine_created = gs_strdup(created);
    if (!g_machine_id || !g_machine_created) {
        checkpoint_machine_forget_identity(prev_dir);
        return -1;
    }
    // Ensure parent + machine dir exist.
    if (gs_mkdir_p(machine_root()) != 0) {
        LOG(1, "checkpoint_machine_set: cannot create root %s", machine_root());
        checkpoint_machine_forget_identity(prev_dir);
        return -1;
    }
    g_machine_dir = gs_str_printf("%s/%s-%s", machine_root(), machine_id, created);
    if (!g_machine_dir) {
        checkpoint_machine_forget_identity(prev_dir);
        return -1;
    }
    if (gs_mkdir_p(g_machine_dir) != 0) {
        LOG(1, "checkpoint_machine_set: cannot create machine dir %s", g_machine_dir);
        checkpoint_machine_forget_identity(prev_dir);
        return -1;
    }
    free(prev_dir);
    return 0;
}

const char *checkpoint_machine_dir(void) {
    return g_machine_dir;
}

int checkpoint_machine_set_dir(const char *dir) {
    if (!dir || !*dir)
        return -1;
    free(g_machine_dir);
    g_machine_dir = gs_strdup(dir);
    if (!g_machine_dir)
        return -1;
    return gs_mkdir_p(g_machine_dir);
}

const char *checkpoint_machine_id(void) {
    return g_machine_id;
}

const char *checkpoint_machine_created(void) {
    return g_machine_created;
}

// A machine directory is `<16 hex>-<ISO-ish stamp>`, the shape
// checkpoint_machine_set builds.  Anything else in the root belongs to
// someone else.
static bool is_machine_dir_name(const char *name) {
    // 16 hex digits
    size_t i = 0;
    for (; i < 16; i++)
        if (!isxdigit((unsigned char)name[i]))
            return false;
    if (name[i++] != '-')
        return false;
    // 8 digits, 'T', 6 digits, 'Z'
    for (size_t k = 0; k < 8; k++, i++)
        if (!isdigit((unsigned char)name[i]))
            return false;
    if (name[i++] != 'T')
        return false;
    for (size_t k = 0; k < 6; k++, i++)
        if (!isdigit((unsigned char)name[i]))
            return false;
    if (name[i++] != 'Z')
        return false;
    return name[i] == '\0';
}

int checkpoint_machine_sweep_others(void) {
    if (!g_machine_dir)
        return -1;
    // `want` is built from id+created; if the caller used _set_dir only
    // those are NULL and the sweep has nothing meaningful to compare
    // against.  Skip rather than dereference NULL via snprintf.
    if (!g_machine_id || !g_machine_created)
        return 0;

    const char *root = machine_root();
    DIR *dir = opendir(root);
    if (!dir) {
        // Nothing to sweep — root may not exist yet.
        return 0;
    }
    char want[512];
    int wn = snprintf(want, sizeof(want), "%s-%s", g_machine_id, g_machine_created);
    if (wn < 0 || (size_t)wn >= sizeof(want)) {
        // Truncated comparison key would mis-match against current dir name
        // and sweep our own state.  Bail out rather than risk that.
        closedir(dir);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (strcmp(name, want) == 0)
            continue; // current machine dir; keep
        // Only entries that LOOK like a machine directory are swept.
        //
        // This used to rm_tree or unlink every entry whose name was not
        // exactly `want`, including plain files at the top level.  The root is
        // configurable (checkpoint_machine_set_root, a headless
        // --checkpoint-dir, a shared test directory), so pointing it at a
        // directory containing anything else lost all of it, silently, behind
        // one level-2 log line per entry.  The code already recognised the
        // danger -- it bails above rather than risk sweeping its own dir on a
        // truncated key -- and this extends that caution to everything else.
        if (!is_machine_dir_name(name)) {
            LOG(2, "checkpoint_machine: leaving unrecognised entry %s alone", name);
            continue;
        }
        char *child = gs_str_printf("%s/%s", root, name);
        if (!child)
            continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            LOG(2, "checkpoint_machine: sweeping orphan dir %s", child);
            (void)gs_rm_tree(child);
        } else {
            unlink(child);
        }
        free(child);
    }
    closedir(dir);

    // Tmp-file cleanup inside our own dir: any *.tmp left over from a
    // crashed write is never valid state.
    DIR *me = opendir(g_machine_dir);
    if (me) {
        struct dirent *me_entry;
        while ((me_entry = readdir(me)) != NULL) {
            const char *name = me_entry->d_name;
            if (!name)
                continue;
            size_t nlen = strlen(name);
            if (nlen >= 4 && strcmp(name + nlen - 4, ".tmp") == 0) {
                char *p = gs_str_printf("%s/%s", g_machine_dir, name);
                if (p) {
                    unlink(p);
                    free(p);
                }
            }
        }
        closedir(me);
    }
    return 0;
}

int checkpoint_machine_write_manifest(void) {
    if (!g_machine_dir)
        return -1;
    // Defer to a JSON build inline.  Keep the schema shallow and stable.
    char *path = gs_str_printf("%s/manifest.json", g_machine_dir);
    if (!path)
        return -1;

    // Compose a manifest reflecting the build + currently-attached images.
    // We avoid pulling JSON dependencies; the schema is small enough to write
    // by hand.
    char *body = NULL;
    char *id_esc = gs_json_escape_dup(g_machine_id);
    char *created_esc = gs_json_escape_dup(g_machine_created);
    char *build_esc = gs_json_escape_dup(get_build_id());
    if (!id_esc || !created_esc || !build_esc) {
        free(id_esc);
        free(created_esc);
        free(build_esc);
        free(path);
        return -1;
    }
    char *prefix = gs_str_printf(
        "{\n  \"schema\": 1,\n  \"machine_id\": \"%s\",\n  \"created\": \"%s\",\n  \"build\": { \"id\": \"%s\" },\n",
        id_esc, created_esc, build_esc);
    free(id_esc);
    free(created_esc);
    free(build_esc);
    if (!prefix) {
        free(path);
        return -1;
    }

    const char *model_id = "";
    uint32_t ram_bytes = 0;
    if (global_emulator && global_emulator->machine && global_emulator->machine->id) {
        model_id = global_emulator->machine->id;
        ram_bytes = global_emulator->ram_size;
    }
    char *model_esc = gs_json_escape_dup(model_id);
    if (!model_esc) {
        free(prefix);
        free(path);
        return -1;
    }
    char *machine = gs_str_printf("  \"machine\": { \"model\": \"%s\", \"ram_bytes\": %u },\n", model_esc, ram_bytes);
    free(model_esc);
    if (!machine) {
        free(prefix);
        free(path);
        return -1;
    }

    // Image list, built by appending: at most MAX_IMAGES entries, so the
    // copying costs nothing, and there is no capacity arithmetic to get
    // wrong -- the hand-grown buffer this replaces overran on a failed
    // realloc.  A failure writes no manifest rather than
    // a truncated one.
    char *img_buf = gs_strdup("  \"images\": [");
    bool first = true;
    int n = global_emulator ? global_emulator->n_images : 0;
    for (int i = 0; i < n && img_buf; i++) {
        image_t *img = global_emulator->images[i];
        if (!img)
            continue;
        char *base_esc = gs_json_escape_dup(img->filename ? img->filename : "");
        char *inst_esc = gs_json_escape_dup((img->writable && img->instance_path) ? img->instance_path : "");
        char *grown = NULL;
        if (base_esc && inst_esc)
            grown = gs_str_printf(
                "%s%s\n    { \"index\": %d, \"base_path\": \"%s\", \"size\": %zu, \"instance_path\": \"%s\" }", img_buf,
                first ? "" : ",", i, base_esc, img->raw_size, inst_esc);
        free(base_esc);
        free(inst_esc);
        free(img_buf);
        img_buf = grown;
        first = false;
    }
    if (img_buf) {
        char *closed = gs_str_printf("%s%s]\n", img_buf, first ? "" : "\n  ");
        free(img_buf);
        img_buf = closed;
    }
    if (!img_buf) {
        free(prefix);
        free(machine);
        free(path);
        return -1;
    }

    body = gs_str_printf("%s%s%s}\n", prefix, machine, img_buf);
    free(prefix);
    free(machine);
    free(img_buf);
    if (!body) {
        free(path);
        return -1;
    }

    int rc = gs_write_atomic(path, body, strlen(body));
    free(body);
    free(path);
    if (rc != 0)
        LOG(1, "checkpoint_machine: failed to write manifest");
    return rc;
}
