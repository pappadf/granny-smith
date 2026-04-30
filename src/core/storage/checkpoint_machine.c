// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_machine.c
// Implementation of per-machine checkpoint directory bookkeeping.  See
// local/gs-docs/notes/proposal-checkpoint-storage-isolation.md.

#include "checkpoint_machine.h"

#include "build_id.h"
#include "common.h"
#include "image.h"
#include "log.h"
#include "system_config.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

LOG_USE_CATEGORY_NAME("checkpoint")

static char *g_machine_id = NULL;
static char *g_machine_created = NULL;
static char *g_machine_dir = NULL;
static char *g_machine_root = NULL; // defaults to "/opfs/checkpoints"

static const char *machine_root(void) {
    return g_machine_root ? g_machine_root : "/opfs/checkpoints";
}

static char *str_dup_local(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out)
        return NULL;
    memcpy(out, s, n);
    return out;
}

static char *str_printf_local(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (needed < 0) {
        va_end(ap);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)needed + 1);
    if (!buf) {
        va_end(ap);
        return NULL;
    }
    vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static int mkdir_p(const char *path) {
    if (!path || !*path)
        return -1;
    char *tmp = str_dup_local(path);
    if (!tmp)
        return -1;
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    int rc = (mkdir(tmp, 0777) != 0 && errno != EEXIST) ? -1 : 0;
    free(tmp);
    return rc;
}

// Recursively delete a directory tree.  Best effort.
static void rm_tree(const char *path) {
    if (!path)
        return;
    DIR *dir = opendir(path);
    if (!dir) {
        // Maybe a regular file.
        unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char *child = str_printf_local("%s/%s", path, name);
        if (!child)
            continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rm_tree(child);
        else
            unlink(child);
        free(child);
    }
    closedir(dir);
    rmdir(path);
}

void checkpoint_machine_set_root(const char *root) {
    free(g_machine_root);
    g_machine_root = root ? str_dup_local(root) : NULL;
    // Recompute machine dir if id+created already set.
    if (g_machine_id && g_machine_created) {
        free(g_machine_dir);
        g_machine_dir = str_printf_local("%s/%s-%s", machine_root(), g_machine_id, g_machine_created);
    }
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
    g_machine_id = str_dup_local(machine_id);
    g_machine_created = str_dup_local(created);
    if (!g_machine_id || !g_machine_created)
        return -1;
    // Ensure parent + machine dir exist.
    if (mkdir_p(machine_root()) != 0) {
        LOG(1, "checkpoint_machine_set: cannot create root %s", machine_root());
        return -1;
    }
    g_machine_dir = str_printf_local("%s/%s-%s", machine_root(), machine_id, created);
    if (!g_machine_dir)
        return -1;
    if (mkdir_p(g_machine_dir) != 0) {
        LOG(1, "checkpoint_machine_set: cannot create machine dir %s", g_machine_dir);
        return -1;
    }
    return 0;
}

const char *checkpoint_machine_dir(void) {
    return g_machine_dir;
}

int checkpoint_machine_set_dir(const char *dir) {
    if (!dir || !*dir)
        return -1;
    free(g_machine_dir);
    g_machine_dir = str_dup_local(dir);
    if (!g_machine_dir)
        return -1;
    return mkdir_p(g_machine_dir);
}

const char *checkpoint_machine_id(void) {
    return g_machine_id;
}

const char *checkpoint_machine_created(void) {
    return g_machine_created;
}

int checkpoint_machine_sweep_others(void) {
    if (!g_machine_dir)
        return -1;

    const char *root = machine_root();
    DIR *dir = opendir(root);
    if (!dir) {
        // Nothing to sweep — root may not exist yet.
        return 0;
    }
    char want[512];
    snprintf(want, sizeof(want), "%s-%s", g_machine_id, g_machine_created);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (strcmp(name, want) == 0)
            continue; // current machine dir; keep
        char *child = str_printf_local("%s/%s", root, name);
        if (!child)
            continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            LOG(2, "checkpoint_machine: sweeping orphan dir %s", child);
            rm_tree(child);
        } else {
            // Stray top-level files (e.g. legacy <n>.checkpoint from older
            // builds): drop them too.
            unlink(child);
        }
        free(child);
    }
    closedir(dir);

    // Tmp-file cleanup inside our own dir: any *.tmp left over from a
    // crashed write is never valid state.
    DIR *me = opendir(g_machine_dir);
    if (me) {
        while ((entry = readdir(me)) != NULL) {
            const char *name = entry->d_name;
            if (!name)
                continue;
            size_t nlen = strlen(name);
            if (nlen >= 4 && strcmp(name + nlen - 4, ".tmp") == 0) {
                char *p = str_printf_local("%s/%s", g_machine_dir, name);
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

// Writes a sibling "<path>.tmp" then renames to <path>.  Returns 0 on success.
static int write_atomic(const char *path, const char *body) {
    if (!path || !body)
        return -1;
    char *tmp = str_printf_local("%s.tmp", path);
    if (!tmp)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return -1;
    }
    size_t len = strlen(body);
    int rc = (fwrite(body, 1, len, f) == len) ? 0 : -1;
    fclose(f);
    if (rc != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int checkpoint_machine_write_manifest(void) {
    if (!g_machine_dir)
        return -1;
    // Defer to a JSON build inline.  Keep the schema shallow and stable.
    char *path = str_printf_local("%s/manifest.json", g_machine_dir);
    if (!path)
        return -1;

    // Compose a manifest reflecting the build + currently-attached images.
    // We avoid pulling JSON dependencies; the schema is small enough to write
    // by hand.
    char *body = NULL;
    char *prefix = str_printf_local(
        "{\n  \"schema\": 1,\n  \"machine_id\": \"%s\",\n  \"created\": \"%s\",\n  \"build\": { \"id\": \"%s\" },\n",
        g_machine_id, g_machine_created, get_build_id());
    if (!prefix) {
        free(path);
        return -1;
    }

    const char *model_id = "";
    uint32_t ram_bytes = 0;
    if (global_emulator && global_emulator->machine && global_emulator->machine->model_id) {
        model_id = global_emulator->machine->model_id;
        ram_bytes = global_emulator->ram_size;
    }
    char *machine = str_printf_local("  \"machine\": { \"model\": \"%s\", \"ram_bytes\": %u },\n", model_id, ram_bytes);
    if (!machine) {
        free(prefix);
        free(path);
        return -1;
    }

    // Image list
    size_t img_buf_cap = 256;
    char *img_buf = (char *)malloc(img_buf_cap);
    if (!img_buf) {
        free(prefix);
        free(machine);
        free(path);
        return -1;
    }
    img_buf[0] = '\0';
    size_t img_len = 0;
    int n = global_emulator ? global_emulator->n_images : 0;
    img_len += (size_t)snprintf(img_buf + img_len, img_buf_cap - img_len, "  \"images\": [");
    for (int i = 0; i < n; i++) {
        image_t *img = global_emulator->images[i];
        if (!img)
            continue;
        const char *base = img->filename ? img->filename : "";
        const char *inst = (img->writable && img->instance_path) ? img->instance_path : "";
        // Re-grow if needed.
        size_t need = strlen(base) + strlen(inst) + 128;
        if (img_len + need >= img_buf_cap) {
            img_buf_cap = (img_len + need) * 2;
            char *grown = (char *)realloc(img_buf, img_buf_cap);
            if (!grown)
                break;
            img_buf = grown;
        }
        img_len += (size_t)snprintf(
            img_buf + img_len, img_buf_cap - img_len,
            "%s\n    { \"index\": %d, \"base_path\": \"%s\", \"size\": %zu, \"instance_path\": \"%s\" }",
            i == 0 ? "" : ",", i, base, img->raw_size, inst);
    }
    if (img_len + 32 >= img_buf_cap) {
        img_buf_cap += 32;
        char *grown = (char *)realloc(img_buf, img_buf_cap);
        if (grown)
            img_buf = grown;
    }
    img_len += (size_t)snprintf(img_buf + img_len, img_buf_cap - img_len, "%s]\n", n > 0 ? "\n  " : "");

    body = str_printf_local("%s%s%s}\n", prefix, machine, img_buf);
    free(prefix);
    free(machine);
    free(img_buf);
    if (!body) {
        free(path);
        return -1;
    }

    int rc = write_atomic(path, body);
    free(body);
    free(path);
    if (rc != 0)
        LOG(1, "checkpoint_machine: failed to write manifest");
    return rc;
}
