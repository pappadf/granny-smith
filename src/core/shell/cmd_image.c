// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_image.c
// `image` command: inspect disk image contents without SCSI-attaching.
// Phase 1 implements `partmap` and `probe` against APM (and sniffs
// ISO 9660 in `probe`).  `list` and `unmount` return a "not implemented
// yet" error; they become real in Phase 2 when the auto-mount cache
// lands.

#include "cmd_types.h"
#include "image.h"
#include "image_apm.h"
#include "image_vfs.h"
#include "vfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ISO 9660 volume descriptor lives at sector 16 (offset 0x8000); the
// standard identifier is "CD001".
#define ISO9660_OFFSET       0x8000
#define ISO9660_VD_ID_OFFSET 1 // byte after type byte
#define ISO9660_ID_LEN       5

// Human-readable label for an fs_kind.
static const char *fs_kind_label(enum apm_fs_kind k) {
    switch (k) {
    case APM_FS_HFS:
        return "HFS";
    case APM_FS_UFS:
        return "UFS";
    case APM_FS_PARTITION_MAP:
        return "map";
    case APM_FS_DRIVER:
        return "drvr";
    case APM_FS_FREE:
        return "free";
    case APM_FS_PATCHES:
        return "patch";
    default:
        return "--";
    }
}

// Sniff ISO 9660: read the 512-byte block covering offset 0x8000 and
// check for "CD001" at offset 1.  disk_read_data requires block-aligned
// reads, and 0x8000 is already block-aligned, so a single 512-byte read
// is the minimum we can do.  Silently swallows read errors since a short
// image simply can't be ISO 9660.
static bool probe_iso9660(image_t *img) {
    if (disk_size(img) < ISO9660_OFFSET + 512)
        return false;
    uint8_t buf[512];
    if (disk_read_data(img, ISO9660_OFFSET, buf, sizeof(buf)) != sizeof(buf))
        return false;
    return memcmp(buf + ISO9660_VD_ID_OFFSET, "CD001", ISO9660_ID_LEN) == 0;
}

// Render a parsed APM table in human-readable form.
static void render_partmap_text(struct cmd_context *ctx, image_t *img, const apm_table_t *table) {
    cmd_printf(ctx, "format: APM (512B blocks, %zu total)\n", disk_size(img) / 512);
    cmd_printf(ctx, "  #  Name                             Type                        Start        Size  FS\n");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        cmd_printf(ctx, "  %-2u %-32s %-24s %10llu  %10llu  %s\n", (unsigned)p->index,
                   p->name[0] ? p->name : "(unnamed)", p->type[0] ? p->type : "(unknown)",
                   (unsigned long long)p->start_block, (unsigned long long)p->size_blocks, fs_kind_label(p->fs_kind));
    }
}

// Render a parsed APM table as JSON (array of partition objects).
static void render_partmap_json(struct cmd_context *ctx, const apm_table_t *table) {
    cmd_printf(ctx, "[");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        cmd_printf(ctx,
                   "%s{\"index\":%u,\"name\":\"%s\",\"type\":\"%s\","
                   "\"start\":%llu,\"size\":%llu,\"fs\":\"%s\"}",
                   i ? "," : "", (unsigned)p->index, p->name, p->type, (unsigned long long)p->start_block,
                   (unsigned long long)p->size_blocks, fs_kind_label(p->fs_kind));
    }
    cmd_printf(ctx, "]\n");
}

// `image partmap <path> [--json]` — parse and print the partition map.
static void cmd_image_partmap(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: image partmap <path> [--json]");
        return;
    }
    const char *path = ctx->args[0].as_str;
    const char *fmt = ctx->args[1].present ? ctx->args[1].as_str : NULL;
    bool json = fmt && strcmp(fmt, "--json") == 0;

    image_t *img = image_open(path, false);
    if (!img) {
        cmd_err(res, "cannot open image '%s'", path);
        return;
    }

    const char *errmsg = NULL;
    apm_table_t *table = image_apm_parse(img, &errmsg);
    if (!table) {
        image_close(img);
        cmd_err(res, "not an APM image: %s", errmsg ? errmsg : "unknown error");
        return;
    }

    if (json)
        render_partmap_json(ctx, table);
    else
        render_partmap_text(ctx, img, table);

    image_apm_free(table);
    image_close(img);
    cmd_ok(res);
}

// `image probe <path>` — print detected format without descent.
static void cmd_image_probe(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: image probe <path>");
        return;
    }
    const char *path = ctx->args[0].as_str;

    image_t *img = image_open(path, false);
    if (!img) {
        cmd_err(res, "cannot open image '%s'", path);
        return;
    }

    size_t size = disk_size(img);

    // APM first — matches the proposal's probe order (APM wins over
    // ISO 9660 because Apple install CDs carry both).
    uint8_t block1[512];
    bool apm = false;
    if (size >= 1024 && disk_read_data(img, 512, block1, sizeof(block1)) == sizeof(block1)) {
        apm = image_apm_probe_magic(block1);
    }

    bool iso = probe_iso9660(img);

    // HFS sniff at volume offset 1024 (for bare floppies / HDs without APM).
    bool hfs = false;
    if (!apm && size >= 1024 + 512) {
        uint8_t mdb[512];
        if (disk_read_data(img, 1024, mdb, sizeof(mdb)) == sizeof(mdb))
            hfs = (mdb[0] == 0x42 && mdb[1] == 0x44);
    }

    if (apm && iso)
        cmd_printf(ctx, "format: APM + ISO 9660 hybrid (%zu bytes)\n", size);
    else if (apm)
        cmd_printf(ctx, "format: APM (%zu bytes)\n", size);
    else if (iso)
        cmd_printf(ctx, "format: ISO 9660 (%zu bytes)\n", size);
    else if (hfs)
        cmd_printf(ctx, "format: HFS (bare, %zu bytes)\n", size);
    else
        cmd_printf(ctx, "format: unrecognised / raw (%zu bytes)\n", size);

    image_close(img);
    cmd_ok(res);
}

// Callback context for `image list` rendering.
struct list_state {
    struct cmd_context *ctx;
    bool json;
    bool first;
    bool header_printed;
};

static void list_row(const char *path, const char *fmt, uint32_t n_parts, uint32_t refs, bool conflicted, void *user) {
    struct list_state *st = user;
    if (st->json) {
        cmd_printf(st->ctx,
                   "%s{\"path\":\"%s\",\"format\":\"%s\",\"partitions\":%u,"
                   "\"refs\":%u,\"conflicted\":%s}",
                   st->first ? "" : ",", path, fmt, n_parts, refs, conflicted ? "true" : "false");
        st->first = false;
        return;
    }
    if (!st->header_printed) {
        cmd_printf(st->ctx, "PATH                                        FMT  PARTS  REFS  STATUS\n");
        st->header_printed = true;
    }
    cmd_printf(st->ctx, "%-44s %-3s %5u %5u  %s\n", path, fmt, n_parts, refs, conflicted ? "busy" : "ok");
    st->first = false;
}

// `image list [--json]` — enumerate currently-cached auto-mounts.
static void cmd_image_list(struct cmd_context *ctx, struct cmd_result *res) {
    const char *fmt = ctx->args[0].present ? ctx->args[0].as_str : NULL;
    bool json = fmt && strcmp(fmt, "--json") == 0;
    struct list_state state = {
        .ctx = ctx,
        .json = json,
        .first = true,
        .header_printed = false,
    };
    if (json)
        cmd_printf(ctx, "[");
    image_vfs_list(list_row, &state);
    if (json) {
        cmd_printf(ctx, "]\n");
    } else if (!state.header_printed) {
        cmd_printf(ctx, "(no cached image mounts)\n");
    }
    cmd_ok(res);
}

// `image unmount <path>` — force-close a cached auto-mount.
static void cmd_image_unmount(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: image unmount <path>");
        return;
    }
    // Canonicalise to match the cache's absolute-path keys.  We use the
    // host-backend's normaliser; `tail` ignored.
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    const char *path = ctx->args[0].as_str;
    if (vfs_resolve(path, resolved, sizeof(resolved), &be, &bctx, &tail) == 0)
        path = resolved;
    int rc = image_vfs_unmount(path);
    if (rc == 0) {
        cmd_printf(ctx, "unmounted %s\n", path);
        cmd_ok(res);
    } else if (rc == -ENOENT) {
        cmd_printf(ctx, "image unmount: not currently mounted: %s\n", path);
        cmd_ok(res);
    } else if (rc == -EBUSY) {
        cmd_printf(ctx, "image unmount: %s has live handles; marked conflicted\n", path);
        cmd_ok(res);
    } else {
        cmd_err(res, "image unmount: %s: %s", path, strerror(-rc));
    }
}

// Dispatcher for the `image` command.  Matches the cmd_hd_handler pattern
// in system.c: check ctx->subcmd string and route to a helper.
void cmd_image_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *sub = ctx->subcmd;
    if (!sub) {
        cmd_err(res, "usage: image <partmap|probe|list|unmount> [args...]");
        return;
    }
    if (strcmp(sub, "partmap") == 0) {
        cmd_image_partmap(ctx, res);
        return;
    }
    if (strcmp(sub, "probe") == 0) {
        cmd_image_probe(ctx, res);
        return;
    }
    if (strcmp(sub, "list") == 0) {
        cmd_image_list(ctx, res);
        return;
    }
    if (strcmp(sub, "unmount") == 0) {
        cmd_image_unmount(ctx, res);
        return;
    }
    cmd_err(res, "unknown subcommand: image %s", sub);
}
