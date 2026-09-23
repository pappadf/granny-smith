// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_util.c
// Shared filesystem, string and JSON helpers.  See storage_util.h.

#include "storage_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#endif

char *gs_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, s, n);
    return copy;
}

char *gs_str_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *out = NULL;
    if (n >= 0 && (out = malloc((size_t)n + 1)) != NULL)
        vsnprintf(out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

static int mkdir_one(const char *dir) {
#if defined(_WIN32)
    int rc = _mkdir(dir);
#else
    int rc = mkdir(dir, 0777);
#endif
    return (rc == 0 || errno == EEXIST) ? 0 : -errno;
}

int gs_mkdir_p(const char *dir) {
    if (!dir || !*dir)
        return -EINVAL;
    char *tmp = gs_strdup(dir);
    if (!tmp)
        return -ENOMEM;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    int rc = 0;
    for (char *p = tmp + 1; *p && rc == 0; p++) {
        if (*p == '/') {
            *p = '\0';
            rc = mkdir_one(tmp);
            *p = '/';
        }
    }
    if (rc == 0)
        rc = mkdir_one(tmp);
    free(tmp);
    return rc;
}

int gs_mkdir_parents(const char *path) {
    if (!path || !*path)
        return -EINVAL;
    char *tmp = gs_strdup(path);
    if (!tmp)
        return -ENOMEM;
    char *slash = strrchr(tmp, '/');
    int rc = 0;
    if (slash && slash != tmp) {
        *slash = '\0';
        rc = gs_mkdir_p(tmp);
    }
    free(tmp);
    return rc;
}

int gs_rm_tree(const char *path) {
    if (!path || !*path)
        return -EINVAL;
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -errno;
    // Anything but a real directory -- a file, a symlink even to a directory
    // -- is removed itself; nothing is followed.
    if (!S_ISDIR(st.st_mode))
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -errno;

    DIR *dir = opendir(path);
    if (!dir)
        return -errno;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        // On the heap, so a deep tree costs a small frame per level.
        char *child = gs_str_printf("%s/%s", path, e->d_name);
        if (child) {
            (void)gs_rm_tree(child); // best effort; rmdir below reports what is left
            free(child);
        }
    }
    closedir(dir);
    return (rmdir(path) == 0 || errno == ENOENT) ? 0 : -errno;
}

int gs_read_file(const char *path, size_t cap, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -errno;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        int e = errno;
        fclose(f);
        return -e;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > cap) {
        fclose(f);
        return -EFBIG;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = malloc(sz ? sz : 1);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != sz) {
        free(buf);
        return -EIO;
    }
    *out = buf;
    *out_len = sz;
    return 0;
}

int gs_json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        char buf[8];
        size_t need = 2;
        buf[0] = '\\';
        switch (*p) {
        case '"':
        case '\\':
            buf[1] = (char)*p;
            break;
        case '\b':
            buf[1] = 'b';
            break;
        case '\f':
            buf[1] = 'f';
            break;
        case '\n':
            buf[1] = 'n';
            break;
        case '\r':
            buf[1] = 'r';
            break;
        case '\t':
            buf[1] = 't';
            break;
        default:
            if (*p < 0x20) {
                need = (size_t)snprintf(buf, sizeof(buf), "\\u%04x", *p);
            } else {
                buf[0] = (char)*p;
                need = 1;
            }
        }
        if (o >= cap || need >= cap - o)
            return -EINVAL;
        memcpy(dst + o, buf, need);
        o += need;
    }
    if (o >= cap)
        return -EINVAL;
    dst[o] = '\0';
    return (int)o;
}

char *gs_json_escape_dup(const char *src) {
    size_t cap = (src ? strlen(src) : 0) * 6 + 1; // every byte as \u00XX at worst
    char *out = malloc(cap);
    if (out && gs_json_escape(src, out, cap) < 0) {
        free(out);
        out = NULL;
    }
    return out;
}
