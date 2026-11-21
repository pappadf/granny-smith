// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_util.c
// Shared filesystem, string and JSON helpers.  See storage_util.h.

#include "storage_util.h"

#include "json_escape.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
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

char *str_printf(const char *fmt, ...) {
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

int mkdir_p(const char *dir) {
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

int mkdir_parents(const char *path) {
    if (!path || !*path)
        return -EINVAL;
    char *tmp = gs_strdup(path);
    if (!tmp)
        return -ENOMEM;
    char *slash = strrchr(tmp, '/');
    int rc = 0;
    if (slash && slash != tmp) {
        *slash = '\0';
        rc = mkdir_p(tmp);
    }
    free(tmp);
    return rc;
}

int rm_tree(const char *path) {
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
        char *child = str_printf("%s/%s", path, e->d_name);
        if (child) {
            (void)rm_tree(child); // best effort; rmdir below reports what is left
            free(child);
        }
    }
    closedir(dir);
    return (rmdir(path) == 0 || errno == ENOENT) ? 0 : -errno;
}

FILE *file_replace_open(file_replace_t *a, const char *path, const char *tmp_suffix) {
    memset(a, 0, sizeof(*a));
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    a->path = gs_strdup(path);
    a->tmp = str_printf("%s%s", path, tmp_suffix ? tmp_suffix : ".tmp");
    if (!a->path || !a->tmp) {
        free(a->path);
        free(a->tmp);
        memset(a, 0, sizeof(*a));
        errno = ENOMEM;
        return NULL;
    }
    a->f = fopen(a->tmp, "wb");
    if (!a->f) {
        int e = errno;
        free(a->path);
        free(a->tmp);
        memset(a, 0, sizeof(*a));
        errno = e;
    }
    return a->f;
}

int file_replace_commit(file_replace_t *a, bool ok) {
    if (!a->f)
        return -EINVAL;
    int rc = 0;
    if (ferror(a->f))
        ok = false;
    if (fclose(a->f) != 0 && ok) {
        rc = errno ? -errno : -EIO;
        ok = false;
    }
    if (ok && rename(a->tmp, a->path) != 0)
        rc = errno ? -errno : -EIO;
    else if (!ok && rc == 0)
        rc = -EIO;
    if (rc != 0)
        remove(a->tmp);
    free(a->path);
    free(a->tmp);
    memset(a, 0, sizeof(*a));
    return rc;
}

int file_write_atomic(const char *path, const void *data, size_t len) {
    file_replace_t a;
    FILE *f = file_replace_open(&a, path, NULL);
    if (!f)
        return errno ? -errno : -EIO;
    return file_replace_commit(&a, len == 0 || fwrite(data, 1, len, f) == len);
}

int open_regular_file(const char *path, uint64_t *size, int64_t *mtime) {
    if (!path)
        return -EINVAL;
    // Non-blocking: a FIFO with no writer would otherwise block in open()
    // before the type check below could refuse it.  O_CLOEXEC: don't leak
    // the fd across a hypothetical future fork/exec.
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -errno;
    struct stat st;
    int rc = 0;
    if (fstat(fd, &st) != 0)
        rc = -errno;
    else if (S_ISDIR(st.st_mode))
        rc = -EISDIR;
    else if (!S_ISREG(st.st_mode))
        rc = -EINVAL;
    // A regular file reads the same either way; clear the flag so every
    // later read is the ordinary blocking kind.
    else if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0)
        rc = -errno;
    if (rc != 0) {
        close(fd);
        return rc;
    }
    if (size)
        *size = (uint64_t)st.st_size;
    if (mtime)
        *mtime = (int64_t)st.st_mtime;
    return fd;
}

// read_file and read_text_file: the whole of `path` into a buffer `pad`
// bytes longer than the file (zeroed), the size first checked against `cap`.
static int read_file_padded(const char *path, size_t cap, size_t pad, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    uint64_t size = 0;
    int fd = open_regular_file(path, &size, NULL);
    if (fd < 0)
        return fd;
    if (size > cap) {
        close(fd);
        return -EFBIG;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        int e = errno;
        close(fd);
        return -e;
    }
    size_t sz = (size_t)size;
    uint8_t *buf = malloc(sz + pad ? sz + pad : 1);
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
    memset(buf + sz, 0, pad);
    *out = buf;
    *out_len = sz;
    return 0;
}

int read_file(const char *path, size_t cap, uint8_t **out, size_t *out_len) {
    return read_file_padded(path, cap, 0, out, out_len);
}

int read_text_file(const char *path, size_t cap, char **out, size_t *out_len) {
    size_t len = 0;
    int rc = read_file_padded(path, cap, 1, (uint8_t **)out, &len);
    if (out_len)
        *out_len = len;
    return rc;
}

void random_bytes(void *out, size_t n) {
    uint8_t *b = (uint8_t *)out;
    size_t got = 0;
#if !defined(_WIN32)
    // getentropy hands out at most 256 bytes a call
    while (got < n) {
        size_t k = n - got > 256 ? 256 : n - got;
        if (getentropy(b + got, k) != 0)
            break;
        got += k;
    }
    if (got < n) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            while (got < n) {
                ssize_t r = read(fd, b + got, n - got);
                if (r <= 0)
                    break;
                got += (size_t)r;
            }
            close(fd);
        }
    }
#endif
    if (got == n)
        return;
    // No entropy source: pid, time and a per-process counter, stirred with
    // xorshift, so ids minted in one process still differ
    static uint64_t ctr;
    uint64_t x =
        ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL) ^ (++ctr * 0x9E3779B97F4A7C15ull) ^ (uint64_t)(uintptr_t)out;
    for (; got < n; got++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        b[got] = (uint8_t)(x >> 24);
    }
}

bool path_has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && strcasecmp(path + n - e, ext) == 0;
}

bool path_has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), e = strlen(suffix);
    return n >= e && strcmp(s + n - e, suffix) == 0;
}

bool path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/')
        dlen--; // keep a lone "/" (the root) intact
    bool has_sep = dlen > 0 && dir[dlen - 1] == '/';
    while (leaf[0] == '/')
        leaf++;
    int n = snprintf(out, cap, "%.*s%s%s", (int)dlen, dir, has_sep ? "" : "/", leaf);
    return n >= 0 && (size_t)n < cap;
}

int json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        char esc[JSON_ESCAPE_MAX];
        size_t need = json_escape_char(*p, esc);
        if (!need) {
            esc[0] = (char)*p;
            need = 1;
        }
        if (o >= cap || need >= cap - o)
            return -EINVAL;
        memcpy(dst + o, esc, need);
        o += need;
    }
    if (o >= cap)
        return -EINVAL;
    dst[o] = '\0';
    return (int)o;
}

char *json_escape_dup(const char *src) {
    size_t cap = (src ? strlen(src) : 0) * JSON_ESCAPE_MAX + 1; // every byte as \u00XX at worst
    char *out = malloc(cap);
    if (out && json_escape(src, out, cap) < 0) {
        free(out);
        out = NULL;
    }
    return out;
}

void json_fputs(FILE *fp, const char *s) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[JSON_ESCAPE_MAX];
        size_t n = json_escape_char(*p, esc);
        if (n)
            fwrite(esc, 1, n, fp);
        else
            fputc(*p, fp);
    }
    fputc('"', fp);
}
