// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_applog.c
// The append log shared by the AFP server's persistent stores (see
// afp_applog.h).

#include "afp_applog.h"
#include "common.h"
#include "crc32.h"

#include "log.h"
#include "storage_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

LOG_USE_CATEGORY_NAME("afp");

struct afp_applog {
    char path[PATH_MAX];
    uint32_t magic;
    FILE *f; // append handle; NULL when the log is not being written
    size_t records; // records in the file
    afp_applog_dump_fn dump;
    void *ctx;
    FILE *emit_to; // the compaction's file, while the store dumps into it
    size_t emitted;
};

static bool record_write(FILE *f, uint8_t op, const void *payload, uint16_t len) {
    uint8_t head[3];
    head[0] = op;
    WR_BE16(head + 1, len);
    uint8_t tail[4];
    WR_BE32(tail, gs_crc32(gs_crc32(0, head, sizeof(head)), payload, len));
    return fwrite(head, 1, sizeof(head), f) == sizeof(head) && (len == 0 || fwrite(payload, 1, len, f) == len) &&
           fwrite(tail, 1, sizeof(tail), f) == sizeof(tail);
}

// Replay the file into the store; false when it is missing or not this
// store's format.
static bool applog_replay(afp_applog_t *log, afp_applog_replay_fn replay) {
    FILE *f = fopen(log->path, "r+b");
    if (!f)
        return false;
    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || RD_BE32(magic) != log->magic) {
        fclose(f);
        LOG(1, "AFP: '%s' is not a log this server reads -- starting it afresh", log->path);
        return false;
    }
    uint8_t *payload = (uint8_t *)malloc(UINT16_MAX);
    if (!payload) {
        fclose(f);
        return false;
    }
    off_t good = (off_t)sizeof(magic);
    for (;;) {
        uint8_t head[3], tail[4];
        if (fread(head, 1, sizeof(head), f) != sizeof(head))
            break;
        uint16_t len = RD_BE16(head + 1);
        if (fread(payload, 1, len, f) != len || fread(tail, 1, sizeof(tail), f) != sizeof(tail))
            break;
        if (gs_crc32(gs_crc32(0, head, sizeof(head)), payload, len) != RD_BE32(tail))
            break;
        replay(log->ctx, head[0], payload, len);
        log->records++;
        good = ftello(f);
    }
    free(payload);
    // Whatever follows the last good record is a torn write: cut it off, or
    // every record appended from now on would be lost behind it (N-16).
    if (fseeko(f, 0, SEEK_END) == 0 && ftello(f) > good) {
        LOG(1, "AFP: '%s' ends in a torn record -- dropping %lld bytes", log->path, (long long)(ftello(f) - good));
        fflush(f);
        if (ftruncate(fileno(f), good) != 0)
            LOG(1, "AFP: cannot cut '%s' (%s)", log->path, strerror(errno));
    }
    fclose(f);
    return true;
}

afp_applog_t *afp_applog_open(const char *path, uint32_t magic, afp_applog_replay_fn replay, afp_applog_dump_fn dump,
                              void *ctx, bool *fresh) {
    afp_applog_t *log = (afp_applog_t *)calloc(1, sizeof(*log));
    if (!log)
        return NULL;
    snprintf(log->path, sizeof(log->path), "%s", path);
    log->magic = magic;
    log->dump = dump;
    log->ctx = ctx;
    bool loaded = applog_replay(log, replay);
    if (fresh)
        *fresh = !loaded;
    if (!loaded) {
        log->records = 0;
        FILE *f = fopen(path, "wb");
        uint8_t m[4];
        WR_BE32(m, magic);
        if (!f || fwrite(m, 1, sizeof(m), f) != sizeof(m))
            LOG(1, "AFP: cannot create '%s' (%s) -- it will not persist", path, strerror(errno));
        if (f)
            fclose(f);
    }
    log->f = fopen(path, "ab");
    if (!log->f)
        LOG(1, "AFP: cannot append to '%s' (%s) -- it will not persist", path, strerror(errno));
    return log;
}

bool afp_applog_emit(afp_applog_t *log, uint8_t op, const void *payload, uint16_t len) {
    if (!log || !log->emit_to || !record_write(log->emit_to, op, payload, len))
        return false;
    log->emitted++;
    return true;
}

// Rewrite the log from the store's live state, atomically (gs_atomic_open):
// a crash leaves the old log or the new one whole.
static void applog_compact(afp_applog_t *log) {
    gs_atomic_t out;
    FILE *f = gs_atomic_open(&out, log->path, NULL);
    if (!f)
        return;
    uint8_t m[4];
    WR_BE32(m, log->magic);
    log->emit_to = f;
    log->emitted = 0;
    bool ok = fwrite(m, 1, sizeof(m), f) == sizeof(m) && log->dump(log->ctx, log);
    log->emit_to = NULL;
    fclose(log->f); // the append handle goes before the file is replaced
    int rc = gs_atomic_commit(&out, ok);
    if (rc == 0)
        log->records = log->emitted;
    else
        LOG(1, "AFP: cannot compact '%s' (%s)", log->path, strerror(-rc));
    log->f = fopen(log->path, "ab");
}

static bool applog_due(const afp_applog_t *log, size_t live) {
    return log->f && log->dump && log->records > AFP_APPLOG_COMPACT_FACTOR * (live + 1);
}

bool afp_applog_append(afp_applog_t *log, uint8_t op, const void *payload, uint16_t len, size_t live) {
    if (!log || !log->f)
        return false;
    if (!record_write(log->f, op, payload, len) || fflush(log->f) != 0) {
        LOG(1, "AFP: write to '%s' failed (%s) -- it no longer persists", log->path, strerror(errno));
        fclose(log->f);
        log->f = NULL;
        return false;
    }
    log->records++;
    if (applog_due(log, live))
        applog_compact(log);
    return true;
}

size_t afp_applog_records(const afp_applog_t *log) {
    return log ? log->records : 0;
}

void afp_applog_close(afp_applog_t *log, size_t live) {
    if (!log)
        return;
    if (applog_due(log, live))
        applog_compact(log);
    if (log->f)
        fclose(log->f);
    free(log);
}
