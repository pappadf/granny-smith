// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_classes.c
// Stub class registry for the object-model rollout. M2 stood up
// cpu/memory/scheduler/machine/shell/storage with read-only
// attributes; M3 extends:
//   - More CPU registers (ccr, sp, usp, ssp, msp, vbr).
//   - cpu.fpu child class with fp0..fp7, fpcr, fpsr, fpiar (when FPU
//     is present on the active machine).
//   - `mac` root child auto-populated from mac_globals_data.c (471
//     entries, V_UINT/V_BYTES per size column).
//   - `shell.alias` child object with add / remove / list methods.
//   - Built-in alias registration at install time:
//       cpu / fpu register short forms ($pc, $d0, $fpcr, …)
//       all 471 mac globals ($MBState, $Ticks, $ROMBase, …)
//
// Real per-peripheral classes (cpu setters, scc, scsi, …) land in
// M3+ per the milestone plan.

#include "gs_classes.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "addr_format.h"
#include "alias.h"
#include "appletalk.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "debug_mac.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "fpu.h"
#include "image.h"
#include "machine.h"
#include "memory.h"
#include "object.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "sound.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"
#include "via.h"

// instance_data on these stubs is config_t*. The lifetime is bounded
// by gs_classes_install / gs_classes_uninstall — same scope as the
// owning emulator instance.

static cpu_t *cpu_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->cpu : NULL;
}

// === CPU class ==============================================================

static value_t attr_cpu_pc(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_pc(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_sr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(2, cpu_get_sr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_ccr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(1, cpu_get_sr(cpu) & 0xFFu);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_ssp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_ssp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_usp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_usp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_msp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_msp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_vbr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_vbr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_sp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_an(cpu, 7));
    v.flags |= VAL_HEX;
    return v;
}

#define CPU_DREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_d##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_dn(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }
#define CPU_AREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_a##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_an(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

// clang-format off — these macros expand to function definitions
// without a trailing `;`, which clang-format mis-parses as expressions.
CPU_DREG_GETTER(0)
CPU_DREG_GETTER(1)
CPU_DREG_GETTER(2)
CPU_DREG_GETTER(3)
CPU_DREG_GETTER(4)
CPU_DREG_GETTER(5)
CPU_DREG_GETTER(6)
CPU_DREG_GETTER(7)
CPU_AREG_GETTER(0)
CPU_AREG_GETTER(1)
CPU_AREG_GETTER(2)
CPU_AREG_GETTER(3)
CPU_AREG_GETTER(4)
CPU_AREG_GETTER(5)
CPU_AREG_GETTER(6)
CPU_AREG_GETTER(7)
// clang-format on

#define ATTR_RO_HEX(name_, get_)                                                                                       \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .flags = VAL_RO | VAL_HEX, .attr = {.type = V_UINT, .get = get_, .set = NULL }  \
    }

static const member_t cpu_members[] = {
    ATTR_RO_HEX("pc", attr_cpu_pc),   ATTR_RO_HEX("sr", attr_cpu_sr),   ATTR_RO_HEX("ccr", attr_cpu_ccr),
    ATTR_RO_HEX("ssp", attr_cpu_ssp), ATTR_RO_HEX("usp", attr_cpu_usp), ATTR_RO_HEX("msp", attr_cpu_msp),
    ATTR_RO_HEX("vbr", attr_cpu_vbr), ATTR_RO_HEX("sp", attr_cpu_sp),   ATTR_RO_HEX("d0", attr_cpu_d0),
    ATTR_RO_HEX("d1", attr_cpu_d1),   ATTR_RO_HEX("d2", attr_cpu_d2),   ATTR_RO_HEX("d3", attr_cpu_d3),
    ATTR_RO_HEX("d4", attr_cpu_d4),   ATTR_RO_HEX("d5", attr_cpu_d5),   ATTR_RO_HEX("d6", attr_cpu_d6),
    ATTR_RO_HEX("d7", attr_cpu_d7),   ATTR_RO_HEX("a0", attr_cpu_a0),   ATTR_RO_HEX("a1", attr_cpu_a1),
    ATTR_RO_HEX("a2", attr_cpu_a2),   ATTR_RO_HEX("a3", attr_cpu_a3),   ATTR_RO_HEX("a4", attr_cpu_a4),
    ATTR_RO_HEX("a5", attr_cpu_a5),   ATTR_RO_HEX("a6", attr_cpu_a6),   ATTR_RO_HEX("a7", attr_cpu_a7),
};

static const class_desc_t cpu_class = {
    .name = "cpu",
    .members = cpu_members,
    .n_members = sizeof(cpu_members) / sizeof(cpu_members[0]),
};

// === CPU.fpu child class ====================================================
//
// The fpu instance_data is config_t*; we walk to cfg->cpu->fpu
// internally because the FPU pointer lives on the cpu_t (and `fpu`
// is exposed as a struct field, not a public getter).

static fpu_state_t *fpu_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->cpu)
        return NULL;
    return (fpu_state_t *)cfg->cpu->fpu;
}

static value_t attr_fpu_fpcr(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpcr);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_fpu_fpsr(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpsr);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_fpu_fpiar(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpiar);
    v.flags |= VAL_HEX;
    return v;
}

// FP0..FP7: 80-bit extended precision. Expose the raw register bytes
// as V_BYTES (10 bytes) so the formatter can hex-dump it and tests
// can compare bit-for-bit. Conversion to a host double is lossy and
// belongs in a future helper; M3 keeps the raw payload visible.
static value_t attr_fpu_fpN(struct object *self, const member_t *m) {
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    int n = (int)(uintptr_t)m->attr.user_data;
    if (n < 0 || n > 7)
        return val_err("invalid fp register index %d", n);
    return val_bytes(&fpu->fp[n], sizeof(fpu->fp[n]));
}

#define FP_REG(idx)                                                                                                    \
    {                                                                                                                  \
        .kind = M_ATTR, .name = "fp" #idx, .flags = VAL_RO, .attr = {                                                  \
            .type = V_BYTES,                                                                                           \
            .get = attr_fpu_fpN,                                                                                       \
            .set = NULL,                                                                                               \
            .user_data = (const void *)(uintptr_t)idx                                                                  \
        }                                                                                                              \
    }

static const member_t fpu_members[] = {
    FP_REG(0),
    FP_REG(1),
    FP_REG(2),
    FP_REG(3),
    FP_REG(4),
    FP_REG(5),
    FP_REG(6),
    FP_REG(7),
    {.kind = M_ATTR,
         .name = "fpcr",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpcr, .set = NULL} },
    {.kind = M_ATTR,
         .name = "fpsr",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpsr, .set = NULL} },
    {.kind = M_ATTR,
         .name = "fpiar",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpiar, .set = NULL}},
};

static const class_desc_t fpu_class = {
    .name = "fpu",
    .members = fpu_members,
    .n_members = sizeof(fpu_members) / sizeof(fpu_members[0]),
};

// === Memory class ===========================================================

static value_t attr_mem_ram_size(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? cfg->ram_size : 0u);
}

static value_t attr_mem_rom_size(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, (cfg && cfg->machine) ? cfg->machine->rom_size : 0u);
}

// === memory.peek child class ================================================
//
// Three methods (b/w/l) that read sized values from guest memory at a
// caller-supplied address. Used by ${...} interpolation in logpoint
// messages (proposal §5.3) and any expression that needs a peek.

static value_t method_mem_peek_b(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.peek.b: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.b: addr must be numeric");
    value_t v = val_uint(1, memory_read_uint8((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_w(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.peek.w: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.w: addr must be numeric");
    value_t v = val_uint(2, memory_read_uint16((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_l(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.peek.l: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.l: addr must be numeric");
    value_t v = val_uint(4, memory_read_uint32((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}

static const arg_decl_t mem_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .flags = VAL_HEX, .doc = "guest memory address"},
};

static const member_t mem_peek_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .doc = "Read 1 byte at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_b}},
    {.kind = M_METHOD,
     .name = "w",
     .doc = "Read 2 bytes (big-endian word) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_w}},
    {.kind = M_METHOD,
     .name = "l",
     .doc = "Read 4 bytes (big-endian long) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_l}},
};

static const class_desc_t mem_peek_class = {
    .name = "peek",
    .members = mem_peek_members,
    .n_members = sizeof(mem_peek_members) / sizeof(mem_peek_members[0]),
};

// === memory.read_cstring ====================================================
//
// Read a NUL-terminated string from guest memory at addr, escaping
// non-printable bytes. Used to migrate the legacy `$str.<src>`
// vocabulary onto the unified ${...} interpolator.

static value_t method_mem_read_cstring(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.read_cstring: expected addr");
    bool ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.read_cstring: addr must be numeric");
    uint32_t addr = (uint32_t)addr_u;
    int max_chars = 96;
    if (argc >= 2) {
        bool ok2 = false;
        int64_t mc = val_as_i64(&argv[1], &ok2);
        if (ok2 && mc > 0 && mc <= 4096)
            max_chars = (int)mc;
    }
    char buf[8192];
    size_t out = 0;
    if (out < sizeof(buf))
        buf[out++] = '"';
    for (int i = 0; i < max_chars && out + 4 < sizeof(buf); i++) {
        uint8_t b = memory_read_uint8(addr + (uint32_t)i);
        if (b == 0)
            break;
        if (b >= 0x20 && b <= 0x7E) {
            buf[out++] = (char)b;
        } else {
            int n = snprintf(buf + out, sizeof(buf) - out, "\\x%02X", b);
            if (n < 0)
                break;
            out += (size_t)n;
        }
    }
    if (out + 1 < sizeof(buf))
        buf[out++] = '"';
    buf[out] = '\0';
    return val_str(buf);
}

static const arg_decl_t mem_read_cstring_args[] = {
    {.name = "addr",      .kind = V_UINT, .flags = VAL_HEX,          .doc = "guest memory address"          },
    {.name = "max_chars", .kind = V_INT,  .flags = OBJ_ARG_OPTIONAL, .doc = "max chars to read (default 96)"},
};

static const member_t memory_members[] = {
    {.kind = M_ATTR,
     .name = "ram_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_ram_size, .set = NULL}                                         },
    {.kind = M_ATTR,
     .name = "rom_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_rom_size, .set = NULL}                                         },
    {.kind = M_METHOD,
     .name = "read_cstring",
     .doc = "Read a quoted, escape-encoded C string at addr",
     .method = {.args = mem_read_cstring_args, .nargs = 2, .result = V_STRING, .fn = method_mem_read_cstring}},
};

static const class_desc_t memory_class = {
    .name = "memory",
    .members = memory_members,
    .n_members = sizeof(memory_members) / sizeof(memory_members[0]),
};

// === Machine class ==========================================================

static value_t attr_machine_model_id(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_id) ? cfg->machine->model_id : "");
}

static value_t attr_machine_model_name(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_name) ? cfg->machine->model_name : "");
}

static value_t attr_machine_cpu_clock(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, (cfg && cfg->machine) ? cfg->machine->cpu_clock_hz : 0u);
}

static const member_t machine_members[] = {
    {.kind = M_ATTR,
     .name = "model_id",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_id, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "model_name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "cpu_clock_hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_cpu_clock, .set = NULL}   },
};

static const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
};

// === Mac low-memory globals (auto-populated) ================================
//
// Reads the existing mac_global_vars[] table from
// src/core/debug/mac_globals_data.c at install time, allocates a
// matching member_t[] array, and points each member's user_data at
// the table row. One shared getter dispatches by row.

extern struct {
    const char *name;
    uint32_t address;
    int size;
    const char *description;
} mac_global_vars[];
extern const size_t mac_global_vars_count;

// Generic getter for any mac global. user_data is the entry index
// into mac_global_vars[]. Size column 1/2/4 → V_UINT; otherwise the
// full buffer is read as V_BYTES (proposal §5.5 strict-improvement
// rule — old code truncated 8/10/16-byte buffers to a u32).
static value_t attr_mac_global(struct object *self, const member_t *m) {
    (void)self;
    if (!m || !m->attr.user_data)
        return val_err("mac member missing user_data");
    size_t idx = (size_t)(uintptr_t)m->attr.user_data;
    if (idx >= mac_global_vars_count)
        return val_err("mac index %zu out of range", idx);
    uint32_t addr = mac_global_vars[idx].address;
    int sz = mac_global_vars[idx].size;
    switch (sz) {
    case 1: {
        value_t v = val_uint(1, memory_read_uint8(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    case 2: {
        value_t v = val_uint(2, memory_read_uint16(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    case 4: {
        value_t v = val_uint(4, memory_read_uint32(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    default: {
        // Read `sz` bytes via memory_read_uint8 — this respects
        // overlay / ROM mapping just like the existing $Symbol path.
        if (sz <= 0 || sz > 256)
            return val_err("unexpected mac entry size %d", sz);
        uint8_t buf[256];
        for (int i = 0; i < sz; i++)
            buf[i] = memory_read_uint8(addr + (uint32_t)i);
        return val_bytes(buf, (size_t)sz);
    }
    }
}

static member_t *g_mac_members = NULL; // heap-allocated 471-entry table
static class_desc_t g_mac_class = {0};

static int build_mac_class(void) {
    if (g_mac_members)
        return 0;
    size_t n = mac_global_vars_count;
    g_mac_members = (member_t *)calloc(n, sizeof(member_t));
    if (!g_mac_members)
        return -1;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        const char *nm = mac_global_vars[i].name;
        if (!nm)
            continue;
        // Dedupe by name. mac_globals_data.c carries a handful of
        // historical duplicates (e.g. TimeSCSIDB at $B24 and $DA6);
        // the legacy resolver matched on first-found, so we mirror
        // that behavior — first entry wins, later ones drop.
        bool dup = false;
        for (size_t j = 0; j < out; j++) {
            if (g_mac_members[j].name && strcmp(g_mac_members[j].name, nm) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        g_mac_members[out].kind = M_ATTR;
        g_mac_members[out].name = nm;
        g_mac_members[out].doc = mac_global_vars[i].description;
        g_mac_members[out].flags = VAL_RO | VAL_HEX | VAL_VOLATILE;
        // Size 1/2/4 → V_UINT; everything else → V_BYTES. Matches the
        // proposal §5.5 mapping table.
        int sz = mac_global_vars[i].size;
        g_mac_members[out].attr.type = (sz == 1 || sz == 2 || sz == 4) ? V_UINT : V_BYTES;
        g_mac_members[out].attr.get = attr_mac_global;
        g_mac_members[out].attr.set = NULL;
        g_mac_members[out].attr.user_data = (const void *)(uintptr_t)i;
        out++;
    }
    g_mac_class.name = "mac";
    g_mac_class.members = g_mac_members;
    g_mac_class.n_members = out;
    return 0;
}

static void free_mac_class(void) {
    free(g_mac_members);
    g_mac_members = NULL;
    g_mac_class.members = NULL;
    g_mac_class.n_members = 0;
}

// === shell.alias child class ================================================
//
// Method shells: callable via node_call once gs_eval grows method
// dispatch in M4. M3 wires them through C-side helpers used by tests.

static value_t method_alias_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("shell.alias.add: expected 2 args (name, path)");
    if (argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("shell.alias.add: name and path must be strings");
    char err[160];
    if (alias_add_user(argv[0].s, argv[1].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

static value_t method_alias_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("shell.alias.remove: expected name (string)");
    char err[160];
    if (alias_remove_user(argv[0].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

// shell.alias.list builds a V_LIST of V_STRING entries: each "name=path".
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} list_acc_t;

static bool list_acc_collect(const char *name, const char *path, alias_kind_t kind, void *ud) {
    list_acc_t *acc = (list_acc_t *)ud;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s%s", name, path, kind == ALIAS_BUILTIN ? " (built-in)" : "");
    if (acc->len + 1 > acc->cap) {
        size_t cap = acc->cap ? acc->cap * 2 : 32;
        value_t *t = (value_t *)realloc(acc->items, cap * sizeof(value_t));
        if (!t)
            return false;
        acc->items = t;
        acc->cap = cap;
    }
    acc->items[acc->len++] = val_str(buf);
    return true;
}

static value_t method_alias_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    list_acc_t acc = {0};
    alias_each(list_acc_collect, &acc);
    return val_list(acc.items, acc.len);
}

static const arg_decl_t alias_add_args[] = {
    {.name = "name", .kind = V_STRING, .flags = 0, .doc = "alias identifier (no $)"          },
    {.name = "path", .kind = V_STRING, .flags = 0, .doc = "object path the alias substitutes"},
};
static const arg_decl_t alias_remove_args[] = {
    {.name = "name", .kind = V_STRING, .flags = 0, .doc = "alias identifier (no $)"},
};

static const member_t shell_alias_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Register a user alias",
     .flags = 0,
     .method = {.args = alias_add_args, .nargs = 2, .result = V_NONE, .fn = method_alias_add}      },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove a user alias",
     .flags = 0,
     .method = {.args = alias_remove_args, .nargs = 1, .result = V_NONE, .fn = method_alias_remove}},
    {.kind = M_METHOD,
     .name = "list",
     .doc = "List aliases as 'name=path' strings",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = method_alias_list}               },
};

static const class_desc_t shell_alias_class = {
    .name = "alias",
    .members = shell_alias_members,
    .n_members = sizeof(shell_alias_members) / sizeof(shell_alias_members[0]),
};

// === Scheduler / Shell / Storage stubs ======================================

static const class_desc_t scheduler_class_desc = {.name = "scheduler", .members = NULL, .n_members = 0};
static const class_desc_t shell_class_desc = {.name = "shell", .members = NULL, .n_members = 0};
static const class_desc_t storage_class_desc = {.name = "storage", .members = NULL, .n_members = 0};

// === lp synthetic class (logpoint fire context) =============================
//
// Per-fire context is tracked in a static struct that debug.c flips
// on/off around expr_interpolate_string. While `active` is true, the
// lp class getters read from the struct; outside that window they
// return V_ERROR (proposal §5.3 — "lp.value resolves to V_ERROR
// outside that context").

static struct {
    bool active;
    uint32_t addr;
    uint32_t value;
    unsigned size;
} g_lp;

void gs_lp_context_begin(uint32_t addr, uint32_t value, unsigned size) {
    g_lp.active = true;
    g_lp.addr = addr;
    g_lp.value = value;
    g_lp.size = size;
}

void gs_lp_context_end(void) {
    g_lp.active = false;
}

static value_t attr_lp_value(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!g_lp.active)
        return val_err("lp.value: only valid inside a logpoint message");
    int w = (g_lp.size == 1) ? 1 : (g_lp.size == 2) ? 2 : 4;
    uint64_t mask = (g_lp.size >= 4) ? 0xFFFFFFFFu : ((1u << (g_lp.size * 8)) - 1u);
    value_t v = val_uint((uint8_t)w, g_lp.value & mask);
    v.flags |= VAL_HEX;
    return v;
}
static value_t attr_lp_addr(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!g_lp.active)
        return val_err("lp.addr: only valid inside a logpoint message");
    value_t v = val_uint(4, g_lp.addr);
    v.flags |= VAL_HEX;
    return v;
}
static value_t attr_lp_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!g_lp.active)
        return val_err("lp.size: only valid inside a logpoint message");
    return val_uint(1, g_lp.size);
}
static value_t attr_lp_pc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!g_lp.active)
        return val_err("lp.pc: only valid inside a logpoint message");
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->cpu)
        return val_err("lp.pc: cpu not initialised");
    value_t v = val_uint(4, cpu_get_pc(cfg->cpu));
    v.flags |= VAL_HEX;
    return v;
}
static value_t attr_lp_instruction_pc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!g_lp.active)
        return val_err("lp.instruction_pc: only valid inside a logpoint message");
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->cpu)
        return val_err("lp.instruction_pc: cpu not initialised");
    value_t v = val_uint(4, cfg->cpu->instruction_pc);
    v.flags |= VAL_HEX;
    return v;
}

static const member_t lp_members[] = {
    {.kind = M_ATTR,
     .name = "value",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = attr_lp_value, .set = NULL}                                                          },
    {.kind = M_ATTR,
     .name = "addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = attr_lp_addr, .set = NULL}                                                           },
    {.kind = M_ATTR, .name = "size", .flags = VAL_RO,           .attr = {.type = V_UINT, .get = attr_lp_size, .set = NULL}},
    {.kind = M_ATTR, .name = "pc",   .flags = VAL_RO | VAL_HEX, .attr = {.type = V_UINT, .get = attr_lp_pc, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "instruction_pc",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = attr_lp_instruction_pc, .set = NULL}                                                 },
};

static const class_desc_t lp_class_desc = {
    .name = "lp",
    .members = lp_members,
    .n_members = sizeof(lp_members) / sizeof(lp_members[0]),
};

// === math object ============================================================
//
// Minimal numerics needed to write predicates inside `$(...)`. See
// proposal-shell-expressions.md §6 — close/abs/min/max are the
// concrete asks; everything else (sin/cos/log/...) defers until a
// real test wants it.

static double v_to_double(const value_t *v, bool *ok) {
    return val_as_f64(v, ok);
}

static value_t method_math_close(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 3)
        return val_err("math.close: expected (a, b, eps)");
    bool ok = true;
    double a = v_to_double(&argv[0], &ok);
    if (!ok)
        return val_err("math.close: a is not numeric");
    double b = v_to_double(&argv[1], &ok);
    if (!ok)
        return val_err("math.close: b is not numeric");
    double eps = v_to_double(&argv[2], &ok);
    if (!ok)
        return val_err("math.close: eps is not numeric");
    double d = a - b;
    if (d < 0)
        d = -d;
    return val_bool(d <= eps);
}

static value_t method_math_abs(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("math.abs: expected (x)");
    if (argv[0].kind == V_INT)
        return val_int(argv[0].i < 0 ? -argv[0].i : argv[0].i);
    if (argv[0].kind == V_UINT)
        return val_uint(0, argv[0].u);
    bool ok = true;
    double x = v_to_double(&argv[0], &ok);
    if (!ok)
        return val_err("math.abs: not numeric");
    return val_float(x < 0 ? -x : x);
}

static value_t method_math_min(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("math.min: expected (a, b)");
    bool ok = true;
    double a = v_to_double(&argv[0], &ok);
    if (!ok)
        return val_err("math.min: a is not numeric");
    double b = v_to_double(&argv[1], &ok);
    if (!ok)
        return val_err("math.min: b is not numeric");
    // Preserve int-likeness when both inputs are ints.
    if (argv[0].kind == V_INT && argv[1].kind == V_INT)
        return val_int(argv[0].i < argv[1].i ? argv[0].i : argv[1].i);
    if (argv[0].kind == V_UINT && argv[1].kind == V_UINT)
        return val_uint(0, argv[0].u < argv[1].u ? argv[0].u : argv[1].u);
    return val_float(a < b ? a : b);
}

static value_t method_math_max(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("math.max: expected (a, b)");
    bool ok = true;
    double a = v_to_double(&argv[0], &ok);
    if (!ok)
        return val_err("math.max: a is not numeric");
    double b = v_to_double(&argv[1], &ok);
    if (!ok)
        return val_err("math.max: b is not numeric");
    if (argv[0].kind == V_INT && argv[1].kind == V_INT)
        return val_int(argv[0].i > argv[1].i ? argv[0].i : argv[1].i);
    if (argv[0].kind == V_UINT && argv[1].kind == V_UINT)
        return val_uint(0, argv[0].u > argv[1].u ? argv[0].u : argv[1].u);
    return val_float(a > b ? a : b);
}

static const arg_decl_t math_close_args[] = {
    {.name = "a",   .kind = V_FLOAT, .doc = "first value" },
    {.name = "b",   .kind = V_FLOAT, .doc = "second value"},
    {.name = "eps", .kind = V_FLOAT, .doc = "tolerance"   },
};
static const arg_decl_t math_unary_args[] = {
    {.name = "x", .kind = V_FLOAT, .doc = "input"},
};
static const arg_decl_t math_binary_args[] = {
    {.name = "a", .kind = V_FLOAT, .doc = "first" },
    {.name = "b", .kind = V_FLOAT, .doc = "second"},
};

static const member_t math_members[] = {
    {.kind = M_METHOD,
     .name = "close",
     .doc = "True if |a - b| <= eps",
     .method = {.args = math_close_args, .nargs = 3, .result = V_BOOL, .fn = method_math_close}},
    {.kind = M_METHOD,
     .name = "abs",
     .doc = "Absolute value",
     .method = {.args = math_unary_args, .nargs = 1, .result = V_FLOAT, .fn = method_math_abs} },
    {.kind = M_METHOD,
     .name = "min",
     .doc = "Smaller of two numerics",
     .method = {.args = math_binary_args, .nargs = 2, .result = V_FLOAT, .fn = method_math_min}},
    {.kind = M_METHOD,
     .name = "max",
     .doc = "Larger of two numerics",
     .method = {.args = math_binary_args, .nargs = 2, .result = V_FLOAT, .fn = method_math_max}},
};

static const class_desc_t math_class_desc = {
    .name = "math",
    .members = math_members,
    .n_members = sizeof(math_members) / sizeof(math_members[0]),
};

// === M6 — debugger object tree ==============================================
//
// `debugger.breakpoints` / `debugger.logpoints` are indexed children
// with sparse stable indices. Each entry is its own object_t whose
// instance_data is the underlying breakpoint_t / logpoint_t. The entry
// classes carry the per-entry attributes (addr, condition, hit_count,
// …) and a `remove()` method.
//
// `debugger.watches` is a placeholder: the legacy shell `watch` command
// is currently a synonym for `run-until` (one-shot, no persistent
// storage) so there are no entries to enumerate. Exposing the empty
// collection keeps the path resolvable for forward-compat without
// inventing new state for M6.

// --- breakpoint entry class -------------------------------------------------

static breakpoint_t *bp_from(struct object *self) {
    return (breakpoint_t *)object_data(self);
}

static value_t bp_attr_addr(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    value_t v = val_uint(4, breakpoint_get_addr(bp));
    v.flags |= VAL_HEX;
    return v;
}

static value_t bp_attr_space(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    static const char *const names[] = {"logical", "physical"};
    int idx = breakpoint_get_space(bp);
    return val_enum(idx, names, 2);
}

static value_t bp_attr_condition(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    const char *c = breakpoint_get_condition(bp);
    return val_str(c ? c : "");
}

static value_t bp_attr_condition_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp) {
        value_free(&in);
        return val_err("breakpoint detached");
    }
    if (in.kind != V_STRING && in.kind != V_NONE) {
        value_free(&in);
        return val_err("condition must be a string");
    }
    breakpoint_set_condition(bp, (in.kind == V_STRING) ? in.s : NULL);
    value_free(&in);
    return val_none();
}

static value_t bp_attr_hit_count(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    return val_uint(4, breakpoint_get_hit_count(bp));
}

static value_t bp_attr_id(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    return val_int(breakpoint_get_id(bp));
}

static value_t bp_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint already removed");
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("debugger not initialised");
    int id = breakpoint_get_id(bp);
    if (!debug_remove_breakpoint(debug, id))
        return val_err("breakpoint #%d not found", id);
    return val_none();
}

static const member_t bp_entry_members[] = {
    {.kind = M_ATTR,
     .name = "addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = bp_attr_addr, .set = NULL}},
    {.kind = M_ATTR, .name = "space", .flags = VAL_RO, .attr = {.type = V_ENUM, .get = bp_attr_space, .set = NULL}},
    {.kind = M_ATTR,
     .name = "condition",
     .flags = 0,
     .attr = {.type = V_STRING, .get = bp_attr_condition, .set = bp_attr_condition_set}},
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = bp_attr_hit_count, .set = NULL}},
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .attr = {.type = V_INT, .get = bp_attr_id, .set = NULL}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this breakpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_remove}},
};

static const class_desc_t breakpoint_entry_class = {
    .name = "breakpoint",
    .members = bp_entry_members,
    .n_members = sizeof(bp_entry_members) / sizeof(bp_entry_members[0]),
};

struct object *gs_classes_make_breakpoint_object(struct breakpoint *bp) {
    if (!bp)
        return NULL;
    return object_new(&breakpoint_entry_class, bp, NULL);
}

// --- logpoint entry class ---------------------------------------------------

static logpoint_t *lp_from(struct object *self) {
    return (logpoint_t *)object_data(self);
}

static value_t lpe_attr_addr(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    value_t v = val_uint(4, logpoint_get_addr(lp));
    v.flags |= VAL_HEX;
    return v;
}
static value_t lpe_attr_end_addr(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    value_t v = val_uint(4, logpoint_get_end_addr(lp));
    v.flags |= VAL_HEX;
    return v;
}
static value_t lpe_attr_kind(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    static const char *const names[] = {"pc", "write", "read", "rw"};
    int idx = logpoint_get_kind(lp);
    if (idx < 0 || idx > 3)
        idx = 0;
    return val_enum(idx, names, 4);
}
static value_t lpe_attr_level(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_int(logpoint_get_level(lp));
}
static value_t lpe_attr_category(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    const char *n = logpoint_get_category_name(lp);
    return val_str(n ? n : "");
}
static value_t lpe_attr_message(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    const char *s = logpoint_get_message(lp);
    return val_str(s ? s : "");
}
static value_t lpe_attr_hit_count(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_uint(4, logpoint_get_hit_count(lp));
}
static value_t lpe_attr_id(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_int(logpoint_get_id(lp));
}
static value_t lpe_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint already removed");
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("debugger not initialised");
    int id = logpoint_get_id(lp);
    if (!debug_remove_logpoint(debug, id))
        return val_err("logpoint #%d not found", id);
    return val_none();
}

static const member_t lp_entry_members[] = {
    {.kind = M_ATTR,
     .name = "addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = lpe_attr_addr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "end_addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = lpe_attr_end_addr, .set = NULL}},
    {.kind = M_ATTR, .name = "kind", .flags = VAL_RO, .attr = {.type = V_ENUM, .get = lpe_attr_kind, .set = NULL}},
    {.kind = M_ATTR, .name = "level", .flags = VAL_RO, .attr = {.type = V_INT, .get = lpe_attr_level, .set = NULL}},
    {.kind = M_ATTR,
     .name = "category",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = lpe_attr_category, .set = NULL}},
    {.kind = M_ATTR,
     .name = "message",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = lpe_attr_message, .set = NULL}},
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = lpe_attr_hit_count, .set = NULL}},
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .attr = {.type = V_INT, .get = lpe_attr_id, .set = NULL}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this logpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lpe_method_remove}},
};

static const class_desc_t logpoint_entry_class = {
    .name = "logpoint",
    .members = lp_entry_members,
    .n_members = sizeof(lp_entry_members) / sizeof(lp_entry_members[0]),
};

struct object *gs_classes_make_logpoint_object(struct logpoint *lp) {
    if (!lp)
        return NULL;
    return object_new(&logpoint_entry_class, lp, NULL);
}

// --- collection objects -----------------------------------------------------
//
// `debugger.breakpoints` is a real object_t* attached to `debugger` at
// install time. Its instance_data is config_t* so the same debug_from()
// helper recovers debug_t* whether you're holding the debugger node or
// one of its children. The collection class declares:
//   - method members (add, clear) for the legacy mutation API;
//   - one indexed M_CHILD member that exposes per-entry objects, so
//     `debugger.breakpoints.0` and `[0]` both resolve via the integer-
//     segment rule in node_child (object.c).

static debug_t *debug_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->debugger : NULL;
}

// Forward-declared because the indexed-child member descriptors below
// need it but the entry classes are already defined above.
static struct object *bp_entries_get(struct object *self, int index);
static int bp_entries_count(struct object *self);
static int bp_entries_next(struct object *self, int prev_index);
static struct object *lp_entries_get(struct object *self, int index);
static int lp_entries_count(struct object *self);
static int lp_entries_next(struct object *self, int prev_index);

static struct object *bp_entries_get(struct object *self, int index) {
    breakpoint_t *bp = debug_breakpoint_by_id(debug_from(self), index);
    return bp ? breakpoint_get_entry_object(bp) : NULL;
}
static int bp_entries_count(struct object *self) {
    return debug_breakpoint_count(debug_from(self));
}
static int bp_entries_next(struct object *self, int prev_index) {
    return debug_breakpoint_next_id(debug_from(self), prev_index);
}

static struct object *lp_entries_get(struct object *self, int index) {
    logpoint_t *lp = debug_logpoint_by_id(debug_from(self), index);
    return lp ? logpoint_get_entry_object(lp) : NULL;
}
static int lp_entries_count(struct object *self) {
    return debug_logpoint_count(debug_from(self));
}
static int lp_entries_next(struct object *self, int prev_index) {
    return debug_logpoint_next_id(debug_from(self), prev_index);
}

static value_t bp_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    if (argc < 1)
        return val_err("breakpoints.add: expected (addr, [condition])");
    bool ok = true;
    uint64_t addr = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("breakpoints.add: addr is not numeric");
    breakpoint_t *bp = set_breakpoint(debug, (uint32_t)addr, ADDR_LOGICAL);
    if (!bp)
        return val_err("breakpoints.add: allocation failed");
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s)
        breakpoint_set_condition(bp, argv[1].s);
    return val_obj(breakpoint_get_entry_object(bp));
}

static value_t bp_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    int id;
    while ((id = debug_breakpoint_next_id(debug, -1)) >= 0)
        debug_remove_breakpoint(debug, id);
    return val_none();
}

static value_t lp_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    int id;
    while ((id = debug_logpoint_next_id(debug, -1)) >= 0)
        debug_remove_logpoint(debug, id);
    return val_none();
}

static const arg_decl_t bp_add_args[] = {
    {.name = "addr",      .kind = V_UINT,   .flags = VAL_HEX,          .doc = "logical address"          },
    {.name = "condition", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "optional condition string"},
};

static const member_t bp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Add a logical-space breakpoint",
     .method = {.args = bp_add_args, .nargs = 2, .result = V_OBJECT, .fn = bp_method_add}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every breakpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_clear}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &breakpoint_entry_class,
               .indexed = true,
               .get = bp_entries_get,
               .count = bp_entries_count,
               .next = bp_entries_next,
               .lookup = NULL}},
};

static const class_desc_t bp_collection_class = {
    .name = "breakpoints",
    .members = bp_collection_members,
    .n_members = sizeof(bp_collection_members) / sizeof(bp_collection_members[0]),
};

static const member_t lp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every logpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lp_method_clear}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &logpoint_entry_class,
               .indexed = true,
               .get = lp_entries_get,
               .count = lp_entries_count,
               .next = lp_entries_next,
               .lookup = NULL}},
};

static const class_desc_t lp_collection_class = {
    .name = "logpoints",
    .members = lp_collection_members,
    .n_members = sizeof(lp_collection_members) / sizeof(lp_collection_members[0]),
};

// Watches: empty placeholder. The legacy `watch` shell command is a
// synonym for `run-until` (one-shot, no persistent watch storage), so
// there is nothing to enumerate. The collection exists for path
// resolvability and forward-compat with the proposal's §5.3 listing.
static const class_desc_t watches_collection_class = {
    .name = "watches",
    .members = NULL,
    .n_members = 0,
};

// `debugger` itself: namespace-only. breakpoints / logpoints / watches
// are attached as named children at install time.
static const class_desc_t debugger_class = {
    .name = "debugger",
    .members = NULL,
    .n_members = 0,
};

// === M7a — SCC peripheral class =============================================
//
// Replaces the bespoke `scc loopback` command (proposal §5.4). The
// `scc` root object has a writable `loopback` attribute, an immutable
// view of the BRG source clocks, a `reset()` method, and per-channel
// children `a` / `b` (proposal goal: "scc class with loopback, reset,
// channel children a/b").
//
// instance_data is config_t* throughout; we recover scc_t* via cfg->scc.
// Channel objects identify themselves through user_data on each member
// — channel index is encoded as the member's user_data pointer (0 = A,
// 1 = B), so a single getter per attribute can serve both children
// (mirrors the trick the auto-populated `mac` class uses).

static scc_t *scc_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->scc : NULL;
}

// `loopback` is the writable head attribute — get/set wrap the C API.
static value_t scc_attr_loopback_get(struct object *self, const member_t *m) {
    (void)m;
    scc_t *scc = scc_from(self);
    return val_bool(scc ? scc_get_external_loopback(scc) : false);
}

static value_t scc_attr_loopback_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    scc_t *scc = scc_from(self);
    if (!scc) {
        value_free(&in);
        return val_err("scc not available");
    }
    bool b = val_as_bool(&in);
    value_free(&in);
    scc_set_external_loopback(scc, b);
    return val_none();
}

static value_t scc_attr_pclk_hz(struct object *self, const member_t *m) {
    (void)m;
    scc_t *scc = scc_from(self);
    return val_uint(4, scc ? scc_get_pclk_hz(scc) : 0);
}

static value_t scc_attr_rtxc_hz(struct object *self, const member_t *m) {
    (void)m;
    scc_t *scc = scc_from(self);
    return val_uint(4, scc ? scc_get_rtxc_hz(scc) : 0);
}

static value_t scc_method_reset(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    scc_t *scc = scc_from(self);
    if (!scc)
        return val_err("scc not available");
    scc_reset(scc);
    return val_none();
}

// --- Channel attributes -----------------------------------------------------
//
// One getter per logical attribute, dispatched by `m->attr.user_data`
// holding the channel index (cast through uintptr_t). Three small
// channel attrs exposed for now — the proposal lists "loopback,
// reset, channel children a/b" without enumerating channel members,
// so we expose the three things existing code already encapsulates
// (`scc_channel_*`). Heavier per-channel views (BRG, baud, sync mode)
// can land later once a real consumer needs them.

static unsigned channel_index_from_member(const member_t *m) {
    return (unsigned)(uintptr_t)m->attr.user_data;
}

static value_t scc_ch_attr_index(struct object *self, const member_t *m) {
    (void)self;
    return val_int((int)channel_index_from_member(m));
}

static value_t scc_ch_attr_dcd(struct object *self, const member_t *m) {
    scc_t *scc = scc_from(self);
    return val_bool(scc_channel_dcd(scc, channel_index_from_member(m)));
}

static value_t scc_ch_attr_tx_empty(struct object *self, const member_t *m) {
    scc_t *scc = scc_from(self);
    return val_bool(scc_channel_tx_empty(scc, channel_index_from_member(m)));
}

static value_t scc_ch_attr_rx_pending(struct object *self, const member_t *m) {
    scc_t *scc = scc_from(self);
    return val_uint(4, scc_channel_rx_pending(scc, channel_index_from_member(m)));
}

// Two member tables — one per channel — because the user_data slot
// has to encode the channel index statically. (Trying to share a
// single table across both channels would need per-instance member
// data, which the substrate intentionally avoids.)

static const member_t scc_ch_a_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scc_ch_attr_index, .set = NULL, .user_data = (void *)(uintptr_t)0}      },
    {.kind = M_ATTR,
     .name = "dcd",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scc_ch_attr_dcd, .set = NULL, .user_data = (void *)(uintptr_t)0}       },
    {.kind = M_ATTR,
     .name = "tx_empty",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scc_ch_attr_tx_empty, .set = NULL, .user_data = (void *)(uintptr_t)0}  },
    {.kind = M_ATTR,
     .name = "rx_pending",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scc_ch_attr_rx_pending, .set = NULL, .user_data = (void *)(uintptr_t)0}},
};

static const member_t scc_ch_b_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scc_ch_attr_index, .set = NULL, .user_data = (void *)(uintptr_t)1}      },
    {.kind = M_ATTR,
     .name = "dcd",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scc_ch_attr_dcd, .set = NULL, .user_data = (void *)(uintptr_t)1}       },
    {.kind = M_ATTR,
     .name = "tx_empty",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scc_ch_attr_tx_empty, .set = NULL, .user_data = (void *)(uintptr_t)1}  },
    {.kind = M_ATTR,
     .name = "rx_pending",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scc_ch_attr_rx_pending, .set = NULL, .user_data = (void *)(uintptr_t)1}},
};

static const class_desc_t scc_channel_a_class = {
    .name = "scc_channel",
    .members = scc_ch_a_members,
    .n_members = sizeof(scc_ch_a_members) / sizeof(scc_ch_a_members[0]),
};
static const class_desc_t scc_channel_b_class = {
    .name = "scc_channel",
    .members = scc_ch_b_members,
    .n_members = sizeof(scc_ch_b_members) / sizeof(scc_ch_b_members[0]),
};

static const member_t scc_members[] = {
    {.kind = M_ATTR,
     .name = "loopback",
     .doc = "External loopback (port A TX → port B RX, port B TX → port A RX)",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = scc_attr_loopback_get, .set = scc_attr_loopback_set}},
    {.kind = M_ATTR,
     .name = "pclk_hz",
     .doc = "PCLK source frequency (Hz)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scc_attr_pclk_hz, .set = NULL}                      },
    {.kind = M_ATTR,
     .name = "rtxc_hz",
     .doc = "RTxC source frequency (Hz)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scc_attr_rtxc_hz, .set = NULL}                      },
    {.kind = M_METHOD,
     .name = "reset",
     .doc = "Reset the SCC (both channels)",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = scc_method_reset}      },
};

static const class_desc_t scc_class = {
    .name = "scc",
    .members = scc_members,
    .n_members = sizeof(scc_members) / sizeof(scc_members[0]),
};

// === M7b — RTC peripheral class =============================================
//
// `rtc.time` is the writable head (Mac-epoch seconds, 1904); it
// replaces `set-time` as the canonical entry point — the legacy
// command remains and `rtc.time = N` is the new equivalent. PRAM is
// exposed two ways: a read-only V_BYTES snapshot of all 256 bytes
// (`rtc.pram`) and per-byte read/write methods (`pram_read(addr)`,
// `pram_write(addr, value)`). Per-byte writes honor the write-protect
// bit the same way the chip-level command stream does — bypassing it
// from the shell would let test scripts mask kernel bugs.

static rtc_t *rtc_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->rtc : NULL;
}

static value_t rtc_attr_time_get(struct object *self, const member_t *m) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    return val_uint(4, rtc ? rtc_get_seconds(rtc) : 0);
}

static value_t rtc_attr_time_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    if (!rtc) {
        value_free(&in);
        return val_err("rtc not available");
    }
    uint32_t mac_seconds = 0;
    bool resolved = false;
    if (in.kind == V_STRING && in.s) {
        // Mirror cmd_set_time: accept either a decimal unix-epoch
        // string or an ISO-8601 "YYYY-MM-DDTHH:MM:SS" timestamp.
        // Either way, the result is unix seconds, then we shift to
        // the Mac 1904 epoch.
        char *endp = NULL;
        long long parsed = strtoll(in.s, &endp, 10);
        if (endp && endp != in.s && *endp == '\0') {
            if (parsed < 0) {
                value_free(&in);
                return val_err("rtc.time: epoch must be non-negative");
            }
            mac_seconds = (uint32_t)((uint64_t)parsed + 2082844800u /* MAC_TO_UNIX_EPOCH */);
            resolved = true;
        } else {
            struct tm tm = {0};
            if (strptime(in.s, "%Y-%m-%dT%H:%M:%S", &tm)) {
                time_t t = timegm(&tm);
                if (t != (time_t)-1) {
                    mac_seconds = (uint32_t)((int64_t)t + 2082844800);
                    resolved = true;
                }
            }
        }
        if (!resolved) {
            value_free(&in);
            return val_err("rtc.time: expected unix-epoch integer or YYYY-MM-DDTHH:MM:SS");
        }
    } else {
        bool ok = true;
        uint64_t s = val_as_u64(&in, &ok);
        if (!ok) {
            value_free(&in);
            return val_err("rtc.time: value is not numeric");
        }
        // Numeric input is treated as Mac-epoch seconds (matches the
        // getter's V_UINT result). Use the string form for unix epochs
        // or ISO timestamps.
        mac_seconds = (uint32_t)s;
    }
    value_free(&in);
    rtc_set_seconds(rtc, mac_seconds);
    return val_none();
}

static value_t rtc_attr_read_only(struct object *self, const member_t *m) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    return val_bool(rtc ? rtc_get_read_only(rtc) : false);
}

static value_t rtc_attr_pram(struct object *self, const member_t *m) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    uint8_t buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = rtc_pram_read(rtc, (uint8_t)i);
    return val_bytes(buf, sizeof(buf));
}

static value_t rtc_method_pram_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    if (argc < 1)
        return val_err("rtc.pram_read: expected (addr)");
    bool ok = true;
    uint64_t addr = val_as_u64(&argv[0], &ok);
    if (!ok || addr > 0xFF)
        return val_err("rtc.pram_read: addr must be 0..255");
    value_t v = val_uint(1, rtc_pram_read(rtc, (uint8_t)addr));
    v.flags |= VAL_HEX;
    return v;
}

static value_t rtc_method_pram_write(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    rtc_t *rtc = rtc_from(self);
    if (!rtc)
        return val_err("rtc not available");
    if (argc < 2)
        return val_err("rtc.pram_write: expected (addr, value)");
    bool ok1 = true, ok2 = true;
    uint64_t addr = val_as_u64(&argv[0], &ok1);
    uint64_t value = val_as_u64(&argv[1], &ok2);
    if (!ok1 || !ok2)
        return val_err("rtc.pram_write: arguments must be numeric");
    if (addr > 0xFF || value > 0xFF)
        return val_err("rtc.pram_write: addr and value must be 0..255");
    if (!rtc_pram_write(rtc, (uint8_t)addr, (uint8_t)value))
        return val_err("rtc.pram_write: PRAM is write-protected");
    return val_none();
}

static const arg_decl_t rtc_pram_read_args[] = {
    {.name = "addr", .kind = V_UINT, .flags = VAL_HEX, .doc = "PRAM offset (0..255)"},
};
static const arg_decl_t rtc_pram_write_args[] = {
    {.name = "addr",  .kind = V_UINT, .flags = VAL_HEX, .doc = "PRAM offset (0..255)"},
    {.name = "value", .kind = V_UINT, .flags = VAL_HEX, .doc = "byte to write"       },
};

static const member_t rtc_members[] = {
    {.kind = M_ATTR,
     .name = "time",
     .doc = "Mac-epoch seconds (1904-based); writable",
     .flags = 0,
     .attr = {.type = V_UINT, .get = rtc_attr_time_get, .set = rtc_attr_time_set}},
    {.kind = M_ATTR,
     .name = "read_only",
     .doc = "Write-protect bit",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = rtc_attr_read_only, .set = NULL}},
    {.kind = M_ATTR,
     .name = "pram",
     .doc = "256-byte PRAM snapshot",
     .flags = VAL_RO,
     .attr = {.type = V_BYTES, .get = rtc_attr_pram, .set = NULL}},
    {.kind = M_METHOD,
     .name = "pram_read",
     .doc = "Read one PRAM byte",
     .method = {.args = rtc_pram_read_args, .nargs = 1, .result = V_UINT, .fn = rtc_method_pram_read}},
    {.kind = M_METHOD,
     .name = "pram_write",
     .doc = "Write one PRAM byte (honors the write-protect bit)",
     .method = {.args = rtc_pram_write_args, .nargs = 2, .result = V_NONE, .fn = rtc_method_pram_write}},
};

static const class_desc_t rtc_class = {
    .name = "rtc",
    .members = rtc_members,
    .n_members = sizeof(rtc_members) / sizeof(rtc_members[0]),
};

// === M7c — VIA peripheral class (via1 / via2) ===============================
//
// Plus has a single `via1`; SE/30 and IIcx add `via2`. Both share the
// same `via_class` member table — `via2` is just a second instance
// attached at install time when present (proposal §5.4 lists "via1
// and via2 (the SE/30 second VIA)").
//
// instance_data on the via object is a void* slot encoding which VIA
// to read: 0 → cfg->via1, 1 → cfg->via2. We keep config_t* as the
// data pointer for parity with other classes and resolve the
// pointer through user_data on each member. (Two member tables, one
// per instance, keep the dispatch branch-free in the getter.)

static via_t *via_instance_from(struct object *self, const member_t *m) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return NULL;
    unsigned which = (unsigned)(uintptr_t)m->attr.user_data;
    if (which == 0)
        return cfg->via1;
    return cfg->via2;
}
#define VIA_BYTE_GETTER(NAME, ACC)                                                                                     \
    static value_t via_attr_##NAME(struct object *self, const member_t *m) {                                           \
        via_t *via = via_instance_from(self, m);                                                                       \
        value_t v = val_uint(1, ACC(via));                                                                             \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

VIA_BYTE_GETTER(ifr, via_get_ifr)
VIA_BYTE_GETTER(ier, via_get_ier)
VIA_BYTE_GETTER(acr, via_get_acr)
VIA_BYTE_GETTER(pcr, via_get_pcr)
VIA_BYTE_GETTER(sr, via_get_sr)

static value_t via_attr_freq_factor(struct object *self, const member_t *m) {
    via_t *via = via_instance_from(self, m);
    return val_uint(1, via_get_freq_factor(via));
}

// Port child: instance_data on the parent VIA carries the VIA index;
// instance_data on the port child encodes (via_index << 1) | port_index
// so a single getter handles all four (via1/A, via1/B, via2/A, via2/B).

static via_t *via_for_port(struct object *self, const member_t *m, unsigned *port_out) {
    config_t *cfg = (config_t *)object_data(self);
    unsigned tag = (unsigned)(uintptr_t)m->attr.user_data;
    *port_out = tag & 1;
    unsigned which = (tag >> 1) & 1;
    if (!cfg)
        return NULL;
    return which == 0 ? cfg->via1 : cfg->via2;
}

static value_t via_port_attr_output(struct object *self, const member_t *m) {
    unsigned port = 0;
    via_t *via = via_for_port(self, m, &port);
    value_t v = val_uint(1, via_port_output(via, port));
    v.flags |= VAL_HEX;
    return v;
}
static value_t via_port_attr_input(struct object *self, const member_t *m) {
    unsigned port = 0;
    via_t *via = via_for_port(self, m, &port);
    value_t v = val_uint(1, via_port_input(via, port));
    v.flags |= VAL_HEX;
    return v;
}
static value_t via_port_attr_direction(struct object *self, const member_t *m) {
    unsigned port = 0;
    via_t *via = via_for_port(self, m, &port);
    value_t v = val_uint(1, via_port_direction(via, port));
    v.flags |= VAL_HEX;
    return v;
}

// One member table per (via_index, port_index) — four total, but the
// repetition is tiny because each is only three fields.
#define VIA_PORT_MEMBERS(VAR, TAG)                                                                                     \
    static const member_t VAR[] = {                                                                                    \
        {.kind = M_ATTR,                                                                                               \
         .name = "output",                                                                                             \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_port_attr_output, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}},   \
        {.kind = M_ATTR,                                                                                               \
         .name = "input",                                                                                              \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_port_attr_input, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)} },    \
        {.kind = M_ATTR,                                                                                               \
         .name = "direction",                                                                                          \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr =                                                                                                       \
             {.type = V_UINT, .get = via_port_attr_direction, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)} },    \
    }

// clang-format off — macro expands to definitions; no trailing semicolons.
VIA_PORT_MEMBERS(via1_port_a_members, 0); // (0<<1)|0 = via1, port A
VIA_PORT_MEMBERS(via1_port_b_members, 1); // (0<<1)|1 = via1, port B
VIA_PORT_MEMBERS(via2_port_a_members, 2); // (1<<1)|0 = via2, port A
VIA_PORT_MEMBERS(via2_port_b_members, 3); // (1<<1)|1 = via2, port B
// clang-format on

static const class_desc_t via1_port_a_class = {.name = "via_port",
                                               .members = via1_port_a_members,
                                               .n_members =
                                                   sizeof(via1_port_a_members) / sizeof(via1_port_a_members[0])};
static const class_desc_t via1_port_b_class = {.name = "via_port",
                                               .members = via1_port_b_members,
                                               .n_members =
                                                   sizeof(via1_port_b_members) / sizeof(via1_port_b_members[0])};
static const class_desc_t via2_port_a_class = {.name = "via_port",
                                               .members = via2_port_a_members,
                                               .n_members =
                                                   sizeof(via2_port_a_members) / sizeof(via2_port_a_members[0])};
static const class_desc_t via2_port_b_class = {.name = "via_port",
                                               .members = via2_port_b_members,
                                               .n_members =
                                                   sizeof(via2_port_b_members) / sizeof(via2_port_b_members[0])};

// Status-register member tables for via1 and via2. Same shape, only
// differ by the user_data tag identifying which instance to read.
#define VIA_REG_MEMBERS(VAR, TAG)                                                                                      \
    static const member_t VAR[] = {                                                                                    \
        {.kind = M_ATTR,                                                                                               \
         .name = "ifr",                                                                                                \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_attr_ifr, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}        },           \
        {.kind = M_ATTR,                                                                                               \
         .name = "ier",                                                                                                \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_attr_ier, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}        },           \
        {.kind = M_ATTR,                                                                                               \
         .name = "acr",                                                                                                \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_attr_acr, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}        },           \
        {.kind = M_ATTR,                                                                                               \
         .name = "pcr",                                                                                                \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_attr_pcr, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}        },           \
        {.kind = M_ATTR,                                                                                               \
         .name = "sr",                                                                                                 \
         .flags = VAL_RO | VAL_HEX,                                                                                    \
         .attr = {.type = V_UINT, .get = via_attr_sr, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}         },            \
        {.kind = M_ATTR,                                                                                               \
         .name = "freq_factor",                                                                                        \
         .flags = VAL_RO,                                                                                              \
         .attr = {.type = V_UINT, .get = via_attr_freq_factor, .set = NULL, .user_data = (void *)(uintptr_t)(TAG)}},   \
    }

// clang-format off
VIA_REG_MEMBERS(via1_members, 0);
VIA_REG_MEMBERS(via2_members, 1);
// clang-format on

static const class_desc_t via1_class = {
    .name = "via",
    .members = via1_members,
    .n_members = sizeof(via1_members) / sizeof(via1_members[0]),
};
static const class_desc_t via2_class = {
    .name = "via",
    .members = via2_members,
    .n_members = sizeof(via2_members) / sizeof(via2_members[0]),
};

// === M7d — SCSI peripheral class ============================================
//
// `scsi` exposes:
//   - `loopback` (R/W bool) — wraps scsi_get/set_loopback
//   - `bus` named child with `phase` (V_ENUM), `target`, `initiator`
//   - `devices` indexed children — sparse stable indices = SCSI ID 0..7
//     (proposal §5.4: "scsi.devices (indexed; each device exposes id,
//     type, image (path attribute), methods eject(), insert)")
//
// Empty SCSI IDs are holes in the indexed collection: `count()`
// returns the number of populated slots, `next()` skips empties.
// Per-entry objects are allocated once at install time and reused
// (the slot lives inside the controller; no realloc needed).

static scsi_t *scsi_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->scsi : NULL;
}

// --- Per-device entry class ------------------------------------------------
//
// Each device's instance_data is a small persistent struct
// `{config_t*, slot}` allocated at install time and freed at
// uninstall. Storing the slot here (rather than encoding in the
// pointer) keeps the substrate's instance_data semantics simple —
// only one indirection in the hot path.

typedef struct {
    config_t *cfg;
    int slot;
} scsi_device_data_t;

static scsi_t *scsi_dev_scsi(struct object *self, unsigned *slot_out) {
    scsi_device_data_t *dd = (scsi_device_data_t *)object_data(self);
    if (!dd || !dd->cfg) {
        *slot_out = 0;
        return NULL;
    }
    *slot_out = (unsigned)dd->slot;
    return dd->cfg->scsi;
}

static value_t scsi_dev_attr_id(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    (void)scsi_dev_scsi(self, &slot);
    return val_int((int)slot);
}

static const char *const SCSI_DEV_TYPE_NAMES[] = {"none", "hd", "cdrom"};

static value_t scsi_dev_attr_type(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    int t = scsi_device_type(scsi, slot);
    if (t < 0 || t > 2)
        t = 0;
    return val_enum(t, SCSI_DEV_TYPE_NAMES, 3);
}

static value_t scsi_dev_attr_vendor(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_vendor(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_product(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_product(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_revision(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_revision(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_block_size(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_uint(2, scsi_device_block_size(scsi, slot));
}
static value_t scsi_dev_attr_read_only(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_bool(scsi_device_read_only(scsi, slot));
}
static value_t scsi_dev_attr_medium_present(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_bool(scsi_device_medium_present(scsi, slot));
}

// `eject()` and `insert(path)` are deferred — they need the
// image-loading plumbing that lands with M7e (floppy.drives) and the
// M8 storage rollout. For M7d the methods are stubs returning a
// V_ERROR explaining the deferral, so paths still resolve and tests
// that only read attributes pass cleanly.
static value_t scsi_dev_method_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_err("scsi.devices.N.eject(): deferred — see M7e / M8 plan");
}
static value_t scsi_dev_method_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_err("scsi.devices.N.insert(): deferred — see M7e / M8 plan");
}

static const arg_decl_t scsi_dev_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Host path or storage URI of the image to mount"},
};

static const member_t scsi_device_members[] = {
    {.kind = M_ATTR,   .name = "id",   .flags = VAL_RO,       .attr = {.type = V_INT, .get = scsi_dev_attr_id, .set = NULL}   },
    {.kind = M_ATTR,   .name = "type", .flags = VAL_RO,       .attr = {.type = V_ENUM, .get = scsi_dev_attr_type, .set = NULL}},
    {.kind = M_ATTR,
     .name = "vendor",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_vendor, .set = NULL}                                                     },
    {.kind = M_ATTR,
     .name = "product",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_product, .set = NULL}                                                    },
    {.kind = M_ATTR,
     .name = "revision",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_revision, .set = NULL}                                                   },
    {.kind = M_ATTR,
     .name = "block_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scsi_dev_attr_block_size, .set = NULL}                                                   },
    {.kind = M_ATTR,
     .name = "read_only",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scsi_dev_attr_read_only, .set = NULL}                                                    },
    {.kind = M_ATTR,
     .name = "medium_present",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scsi_dev_attr_medium_present, .set = NULL}                                               },
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Eject medium (CD-ROM); deferred until M7e / M8",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = scsi_dev_method_eject}                                      },
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Insert image; deferred until M7e / M8",
     .method = {.args = scsi_dev_insert_args, .nargs = 1, .result = V_NONE, .fn = scsi_dev_method_insert}                     },
};

static const class_desc_t scsi_device_class = {
    .name = "scsi_device",
    .members = scsi_device_members,
    .n_members = sizeof(scsi_device_members) / sizeof(scsi_device_members[0]),
};

// --- bus child class -------------------------------------------------------

static const char *const SCSI_PHASE_NAMES[] = {
    "bus_free", "arbitration", "selection", "reselection", "command",
    "data_in",  "data_out",    "status",    "message_in",  "message_out",
};

static value_t scsi_bus_attr_phase(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    int p = scsi_get_bus_phase(cfg ? cfg->scsi : NULL);
    if (p < 0 || p > 9)
        p = 0;
    return val_enum(p, SCSI_PHASE_NAMES, 10);
}
static value_t scsi_bus_attr_target(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_int(scsi_get_bus_target(cfg ? cfg->scsi : NULL));
}
static value_t scsi_bus_attr_initiator(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_int(scsi_get_bus_initiator(cfg ? cfg->scsi : NULL));
}

static const member_t scsi_bus_members[] = {
    {.kind = M_ATTR,
     .name = "phase",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = scsi_bus_attr_phase, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "target",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scsi_bus_attr_target, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "initiator",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scsi_bus_attr_initiator, .set = NULL}},
};
static const class_desc_t scsi_bus_class = {
    .name = "scsi_bus",
    .members = scsi_bus_members,
    .n_members = sizeof(scsi_bus_members) / sizeof(scsi_bus_members[0]),
};

// --- devices collection: indexed children -----------------------------------

// Per-slot persistent device data, filled at install-time. The
// devices array on the controller has 8 fixed slots; we mirror them
// 1:1 with 8 slots' worth of data + object pointers. Empty slots
// have type=scsi_dev_none — `count()` and `next()` skip them.

static scsi_device_data_t g_scsi_dev_data[8];
static struct object *g_scsi_dev_objs[8];

static struct object *scsi_devices_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->scsi || index < 0 || index > 7)
        return NULL;
    if (!scsi_device_present(cfg->scsi, (unsigned)index))
        return NULL;
    return g_scsi_dev_objs[index];
}
static int scsi_devices_count(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->scsi)
        return 0;
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (scsi_device_present(cfg->scsi, (unsigned)i))
            n++;
    return n;
}
static int scsi_devices_next(struct object *self, int prev_index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->scsi)
        return -1;
    for (int i = prev_index + 1; i < 8; i++)
        if (scsi_device_present(cfg->scsi, (unsigned)i))
            return i;
    return -1;
}

static const member_t scsi_devices_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &scsi_device_class,
               .indexed = true,
               .get = scsi_devices_get,
               .count = scsi_devices_count,
               .next = scsi_devices_next,
               .lookup = NULL}},
};
static const class_desc_t scsi_devices_collection_class = {
    .name = "scsi_devices",
    .members = scsi_devices_collection_members,
    .n_members = 1,
};

// --- top-level scsi class ---------------------------------------------------

static value_t scsi_attr_loopback_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(scsi_get_loopback(scsi_from(self)));
}
static value_t scsi_attr_loopback_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    scsi_t *scsi = scsi_from(self);
    if (!scsi) {
        value_free(&in);
        return val_err("scsi not available");
    }
    bool b = val_as_bool(&in);
    value_free(&in);
    scsi_set_loopback(scsi, b);
    return val_none();
}

static const member_t scsi_members[] = {
    {.kind = M_ATTR,
     .name = "loopback",
     .doc = "Loopback test card / passive terminator",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = scsi_attr_loopback_get, .set = scsi_attr_loopback_set}},
};

static const class_desc_t scsi_class = {
    .name = "scsi",
    .members = scsi_members,
    .n_members = sizeof(scsi_members) / sizeof(scsi_members[0]),
};

// === M7e — floppy peripheral class ==========================================
//
// `floppy` is the unified IWM/SWIM controller. Both Plus (IWM-only)
// and SE/30 (SWIM dual-mode) attach a floppy_t at machine init, so
// the object is always present when cfg->floppy is non-NULL.
//
// Drive layout matches the proposal: `floppy.drives[0]` is the
// internal drive, `floppy.drives[1]` is external. Indexed children
// are dense (always exactly 2 slots) — index sparseness only matters
// for collections that grow (breakpoints, scsi.devices); the floppy
// drive count is a hardware constant.

static floppy_t *floppy_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->floppy : NULL;
}

static const char *const FLOPPY_TYPE_NAMES[] = {"iwm", "swim"};

static value_t floppy_attr_type(struct object *self, const member_t *m) {
    (void)m;
    floppy_t *floppy = floppy_from(self);
    int t = floppy ? floppy_get_type(floppy) : 0;
    if (t < 0 || t > 1)
        t = 0;
    return val_enum(t, FLOPPY_TYPE_NAMES, 2);
}

static value_t floppy_attr_sel(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(floppy_get_sel(floppy_from(self)));
}

static const member_t floppy_members[] = {
    {.kind = M_ATTR,
     .name = "type",
     .doc = "Controller type: iwm (Plus) or swim (SE/30)",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = floppy_attr_type, .set = NULL}},
    {.kind = M_ATTR,
     .name = "sel",
     .doc = "VIA-driven head-select signal",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_attr_sel, .set = NULL} },
};

static const class_desc_t floppy_class = {
    .name = "floppy",
    .members = floppy_members,
    .n_members = sizeof(floppy_members) / sizeof(floppy_members[0]),
};

// --- Per-drive entry class -------------------------------------------------

typedef struct {
    config_t *cfg;
    int slot;
} floppy_drive_data_t;

static floppy_t *floppy_drive_floppy(struct object *self, unsigned *slot_out) {
    floppy_drive_data_t *dd = (floppy_drive_data_t *)object_data(self);
    if (!dd || !dd->cfg) {
        *slot_out = 0;
        return NULL;
    }
    *slot_out = (unsigned)dd->slot;
    return dd->cfg->floppy;
}

static value_t floppy_drive_attr_index(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    (void)floppy_drive_floppy(self, &slot);
    return val_int((int)slot);
}

static value_t floppy_drive_attr_present(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_bool(floppy && floppy_is_inserted(floppy, (int)slot));
}

static value_t floppy_drive_attr_track(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_int(floppy_drive_track(floppy, slot));
}

static value_t floppy_drive_attr_side(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_int(floppy_drive_side(floppy, slot));
}

static value_t floppy_drive_attr_motor_on(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_bool(floppy_drive_motor_on(floppy, slot));
}

static value_t floppy_drive_attr_disk(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    const char *p = floppy_drive_disk_path(floppy, slot);
    return val_str(p ? p : "");
}

static value_t floppy_drive_method_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    if (!floppy)
        return val_err("floppy not available");
    if (!floppy_drive_eject(floppy, slot))
        return val_err("drive %u: no disk inserted", slot);
    return val_none();
}

// `insert(path)` is deferred — image loading and floppy_insert
// require the M8 storage rollout to converge cleanly. For M7e the
// method is a stub returning V_ERROR, mirroring the scsi.devices
// eject/insert deferral. Paths still resolve.
static value_t floppy_drive_method_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_err("floppy.drives.N.insert(): deferred — see M8 plan");
}

static const arg_decl_t floppy_drive_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Host path or storage URI of the image to mount"},
};

static const member_t floppy_drive_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_index, .set = NULL}},
    {.kind = M_ATTR,
     .name = "present",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_drive_attr_present, .set = NULL}},
    {.kind = M_ATTR,
     .name = "track",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_track, .set = NULL}},
    {.kind = M_ATTR,
     .name = "side",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_side, .set = NULL}},
    {.kind = M_ATTR,
     .name = "motor_on",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_drive_attr_motor_on, .set = NULL}},
    {.kind = M_ATTR,
     .name = "disk",
     .doc = "Path to currently inserted image (empty when no disk)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = floppy_drive_attr_disk, .set = NULL}},
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Remove the inserted disk",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = floppy_drive_method_eject}},
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Insert a disk image; deferred until M8",
     .method = {.args = floppy_drive_insert_args, .nargs = 1, .result = V_NONE, .fn = floppy_drive_method_insert}},
};

static const class_desc_t floppy_drive_class = {
    .name = "floppy_drive",
    .members = floppy_drive_members,
    .n_members = sizeof(floppy_drive_members) / sizeof(floppy_drive_members[0]),
};

// --- Drives collection: indexed children -----------------------------------

static floppy_drive_data_t g_floppy_drive_data[2];
static struct object *g_floppy_drive_objs[2];

static struct object *floppy_drives_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->floppy || index < 0 || index > 1)
        return NULL;
    return g_floppy_drive_objs[index];
}
static int floppy_drives_count(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return (cfg && cfg->floppy) ? 2 : 0;
}
static int floppy_drives_next(struct object *self, int prev_index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !cfg->floppy)
        return -1;
    int next = prev_index + 1;
    return next < 2 ? next : -1;
}

static const member_t floppy_drives_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &floppy_drive_class,
               .indexed = true,
               .get = floppy_drives_get,
               .count = floppy_drives_count,
               .next = floppy_drives_next,
               .lookup = NULL}},
};
static const class_desc_t floppy_drives_collection_class = {
    .name = "floppy_drives",
    .members = floppy_drives_collection_members,
    .n_members = 1,
};

// === M7f — sound peripheral class ===========================================
//
// Plus's PWM sound module per proposal §5.4: `sound.enabled`,
// `sound.sample_rate`, `sound.volume` attributes plus `mute(bool)`
// method. SE/30 / IIcx use the Apple Sound Chip and don't populate
// `cfg->sound`; the object is only attached when the field is set.

static sound_t *sound_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->sound : NULL;
}

static value_t sound_attr_enabled_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(sound_get_enabled(sound_from(self)));
}
static value_t sound_attr_enabled_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    sound_t *sound = sound_from(self);
    if (!sound) {
        value_free(&in);
        return val_err("sound not available");
    }
    bool b = val_as_bool(&in);
    value_free(&in);
    sound_enable(sound, b);
    return val_none();
}

static value_t sound_attr_volume_get(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, sound_get_volume(sound_from(self)));
}
static value_t sound_attr_volume_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    sound_t *sound = sound_from(self);
    if (!sound) {
        value_free(&in);
        return val_err("sound not available");
    }
    bool ok = true;
    uint64_t v = val_as_u64(&in, &ok);
    value_free(&in);
    if (!ok)
        return val_err("sound.volume: value is not numeric");
    if (v >= 8)
        return val_err("sound.volume: must be 0..7 (got %llu)", (unsigned long long)v);
    sound_volume(sound, (unsigned)v);
    return val_none();
}

static value_t sound_attr_sample_rate(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, sound_get_sample_rate(sound_from(self)));
}

static value_t sound_method_mute(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    sound_t *sound = sound_from(self);
    if (!sound)
        return val_err("sound not available");
    if (argc < 1)
        return val_err("sound.mute: expected (muted)");
    bool muted = val_as_bool(&argv[0]);
    sound_mute(sound, muted);
    return val_none();
}

static const arg_decl_t sound_mute_args[] = {
    {.name = "muted", .kind = V_BOOL, .doc = "true to mute, false to unmute"},
};

static const member_t sound_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Sound output gate (writable mirror of mute)",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = sound_attr_enabled_get, .set = sound_attr_enabled_set}},
    {.kind = M_ATTR,
     .name = "volume",
     .doc = "Output level (0..7)",
     .flags = 0,
     .attr = {.type = V_UINT, .get = sound_attr_volume_get, .set = sound_attr_volume_set}},
    {.kind = M_ATTR,
     .name = "sample_rate",
     .doc = "Output sample rate in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sound_attr_sample_rate, .set = NULL}},
    {.kind = M_METHOD,
     .name = "mute",
     .doc = "Mute or unmute the sound output",
     .method = {.args = sound_mute_args, .nargs = 1, .result = V_NONE, .fn = sound_method_mute}},
};

static const class_desc_t sound_class = {
    .name = "sound",
    .members = sound_members,
    .n_members = sizeof(sound_members) / sizeof(sound_members[0]),
};

// === M8 — network.appletalk + input.mouse ===================================
//
// First slice of M8 (proposal §5.8 / §5.9). Adds:
//
//   network.appletalk.shares — indexed children, sparse stable indices.
//                              Each share exposes name / path / vol_id;
//                              add(name, path) / remove(name) live on
//                              the collection class.
//   network.appletalk.printer — `enabled` (R/O bool), `name` (R/O str
//                              — empty when disabled). Methods
//                              `enable(name?)` / `disable()`.
//   input.mouse — methods move(x, y), click(down), trace(enabled).
//                 Wraps debug_mac_set_mouse / system_mouse_update /
//                 debug_mac_set_trace_mouse so the legacy `set-mouse`
//                 / `mouse-button` / `trace-mouse` commands continue
//                 to work alongside.
//
// Storage and the top-level introspection root methods land in a
// follow-up M8 sub-commit; the underlying plumbing for storage.import
// needs the M8 storage rollout the proposal describes in more detail.

// --- network.appletalk.shares.* indexed entries ----------------------------

// Per-share entry data. Indexed by slot (0..MAX_SHARES-1); the
// collection's get() consults atalk_share_in_use() and returns
// the corresponding pre-allocated object so shares with `in_use=false`
// are holes in the collection.
typedef struct {
    int slot;
} atalk_share_data_t;

static atalk_share_data_t g_atalk_share_data[8];
static struct object *g_atalk_share_objs[8];

static int atalk_share_data_slot(struct object *self) {
    atalk_share_data_t *d = (atalk_share_data_t *)object_data(self);
    return d ? d->slot : -1;
}

static value_t atalk_share_attr_name(struct object *self, const member_t *m) {
    (void)m;
    const char *s = atalk_share_name(atalk_share_data_slot(self));
    return val_str(s ? s : "");
}
static value_t atalk_share_attr_path(struct object *self, const member_t *m) {
    (void)m;
    const char *s = atalk_share_path(atalk_share_data_slot(self));
    return val_str(s ? s : "");
}
static value_t atalk_share_attr_vol_id(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(2, atalk_share_vol_id(atalk_share_data_slot(self)));
}

static value_t atalk_share_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    const char *name = atalk_share_name(atalk_share_data_slot(self));
    if (!name)
        return val_err("share already removed");
    if (atalk_share_remove(name) != 0)
        return val_err("atalk_share_remove failed");
    return val_none();
}

static const member_t atalk_share_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = atalk_share_attr_name, .set = NULL}                  },
    {.kind = M_ATTR,
     .name = "path",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = atalk_share_attr_path, .set = NULL}                  },
    {.kind = M_ATTR,
     .name = "vol_id",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = atalk_share_attr_vol_id, .set = NULL}                  },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this AppleShare volume",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = atalk_share_method_remove}},
};

static const class_desc_t atalk_share_class = {
    .name = "atalk_share",
    .members = atalk_share_members,
    .n_members = sizeof(atalk_share_members) / sizeof(atalk_share_members[0]),
};

// --- shares collection -----------------------------------------------------

static struct object *atalk_shares_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= 8)
        return NULL;
    if (!atalk_share_in_use(index))
        return NULL;
    return g_atalk_share_objs[index];
}
static int atalk_shares_count(struct object *self) {
    (void)self;
    int n = 0;
    int max = atalk_share_max();
    for (int i = 0; i < max && i < 8; i++)
        if (atalk_share_in_use(i))
            n++;
    return n;
}
static int atalk_shares_next(struct object *self, int prev_index) {
    (void)self;
    int max = atalk_share_max();
    for (int i = prev_index + 1; i < max && i < 8; i++)
        if (atalk_share_in_use(i))
            return i;
    return -1;
}

static value_t atalk_shares_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("appletalk.shares.add: expected (name, path)");
    if (atalk_share_add(argv[0].s, argv[1].s) != 0)
        return val_err("atalk_share_add failed (see log)");
    return val_none();
}

static value_t atalk_shares_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("appletalk.shares.remove: expected (name)");
    if (atalk_share_remove(argv[0].s) != 0)
        return val_err("atalk_share_remove failed (no such share?)");
    return val_none();
}

static const arg_decl_t atalk_shares_add_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Volume name (max 32 chars)"          },
    {.name = "path", .kind = V_STRING, .doc = "Host path to a directory under MEMFS"},
};
static const arg_decl_t atalk_shares_remove_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Volume name to remove"},
};

static const member_t atalk_shares_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Add an AppleShare volume",
     .method = {.args = atalk_shares_add_args, .nargs = 2, .result = V_NONE, .fn = atalk_shares_method_add}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove an AppleShare volume by name",
     .method = {.args = atalk_shares_remove_args, .nargs = 1, .result = V_NONE, .fn = atalk_shares_method_remove}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &atalk_share_class,
               .indexed = true,
               .get = atalk_shares_get,
               .count = atalk_shares_count,
               .next = atalk_shares_next,
               .lookup = NULL}},
};

static const class_desc_t atalk_shares_collection_class = {
    .name = "atalk_shares",
    .members = atalk_shares_collection_members,
    .n_members = sizeof(atalk_shares_collection_members) / sizeof(atalk_shares_collection_members[0]),
};

// --- network.appletalk.printer --------------------------------------------

static value_t atalk_printer_attr_enabled(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(atalk_printer_is_enabled());
}
static value_t atalk_printer_attr_name(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *n = atalk_printer_object_name();
    return val_str(n ? n : "");
}

static value_t atalk_printer_method_enable(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *name = (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s) ? argv[0].s : NULL;
    if (atalk_printer_enable(name) != 0)
        return val_err("atalk_printer_enable failed");
    return val_none();
}
static value_t atalk_printer_method_disable(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    if (atalk_printer_disable() != 0)
        return val_err("atalk_printer_disable failed");
    return val_none();
}

static const arg_decl_t atalk_printer_enable_args[] = {
    {.name = "name", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional NBP entity name override"},
};

static const member_t atalk_printer_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = atalk_printer_attr_enabled, .set = NULL}                                      },
    {.kind = M_ATTR,
     .name = "name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = atalk_printer_attr_name, .set = NULL}                                       },
    {.kind = M_METHOD,
     .name = "enable",
     .doc = "Advertise the LaserWriter via NBP",
     .method = {.args = atalk_printer_enable_args, .nargs = 1, .result = V_NONE, .fn = atalk_printer_method_enable}},
    {.kind = M_METHOD,
     .name = "disable",
     .doc = "Stop advertising the LaserWriter",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = atalk_printer_method_disable}                    },
};

static const class_desc_t atalk_printer_class = {
    .name = "atalk_printer",
    .members = atalk_printer_members,
    .n_members = sizeof(atalk_printer_members) / sizeof(atalk_printer_members[0]),
};

// --- network.appletalk + network ------------------------------------------

static const class_desc_t atalk_class = {
    .name = "appletalk",
    .members = NULL,
    .n_members = 0,
};

static const class_desc_t network_class = {
    .name = "network",
    .members = NULL,
    .n_members = 0,
};

// --- mouse ----------------------------------------------------------------

// Mode flag string -> legacy short option.
//   "default" / NULL  → no flag (per-platform default routing)
//   "global"          → --global  (Mac OS Toolbox MTemp write)
//   "hw"              → --hw      (raw quadrature / ADB delta)
//   "aux"             → --aux     (A/UX MAE physical-page write)
// Returns "" for default, an `--<flag>` token otherwise, or NULL if the
// caller passed an unknown mode.
static const char *mouse_mode_flag(const value_t *v) {
    if (!v || v->kind != V_STRING || !v->s || !*v->s)
        return "";
    if (strcmp(v->s, "default") == 0)
        return "";
    if (strcmp(v->s, "global") == 0)
        return "--global ";
    if (strcmp(v->s, "hw") == 0)
        return "--hw ";
    if (strcmp(v->s, "aux") == 0)
        return "--aux ";
    return NULL;
}

static value_t mouse_method_move(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("mouse.move: expected (x, y, [mode])");
    bool okx = true, oky = true;
    int64_t x = val_as_i64(&argv[0], &okx);
    int64_t y = val_as_i64(&argv[1], &oky);
    if (!okx || !oky)
        return val_err("mouse.move: x and y must be integers");
    const char *flag = (argc >= 3) ? mouse_mode_flag(&argv[2]) : "";
    if (!flag)
        return val_err("mouse.move: mode must be one of \"default\"/\"global\"/\"hw\"/\"aux\"");
    char line[128];
    int n = snprintf(line, sizeof(line), "set-mouse %s%lld %lld", flag, (long long)x, (long long)y);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("mouse.move: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t mouse_method_click(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool down = (argc >= 1) ? val_as_bool(&argv[0]) : true;
    const char *flag = (argc >= 2) ? mouse_mode_flag(&argv[1]) : "";
    if (!flag)
        return val_err("mouse.click: mode must be one of \"default\"/\"global\"/\"hw\"");
    char line[64];
    int n = snprintf(line, sizeof(line), "mouse-button %s%s", flag, down ? "down" : "up");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("mouse.click: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t mouse_method_trace(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("mouse.trace: expected (enabled)");
    debug_mac_set_trace_mouse(val_as_bool(&argv[0]));
    return val_none();
}

static const arg_decl_t mouse_move_args[] = {
    {.name = "x", .kind = V_INT, .doc = "Target X coordinate"},
    {.name = "y", .kind = V_INT, .doc = "Target Y coordinate"},
    {.name = "mode",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "\"default\" (per-platform), \"global\" (Toolbox MTemp), \"hw\" (raw quadrature), or \"aux\" (A/UX MAE)"},
};
static const arg_decl_t mouse_click_args[] = {
    {.name = "down", .kind = V_BOOL, .flags = OBJ_ARG_OPTIONAL, .doc = "true = press, false = release (default true)"},
    {.name = "mode",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "\"default\" (per-platform), \"global\" (Toolbox MBState), or \"hw\" (raw)"                              },
};
static const arg_decl_t mouse_trace_args[] = {
    {.name = "enabled", .kind = V_BOOL, .doc = "true = log mouse position once per second"},
};

static const member_t mouse_members[] = {
    {.kind = M_METHOD,
     .name = "move",
     .doc = "Set mouse position; optional mode chooses the routing path",
     .method = {.args = mouse_move_args, .nargs = 3, .result = V_BOOL, .fn = mouse_method_move}  },
    {.kind = M_METHOD,
     .name = "click",
     .doc = "Press or release the mouse button; optional mode chooses the routing path",
     .method = {.args = mouse_click_args, .nargs = 2, .result = V_BOOL, .fn = mouse_method_click}},
    {.kind = M_METHOD,
     .name = "trace",
     .doc = "Toggle the 1 Hz mouse-position trace logger",
     .method = {.args = mouse_trace_args, .nargs = 1, .result = V_NONE, .fn = mouse_method_trace}},
};

static const class_desc_t mouse_class = {
    .name = "mouse",
    .members = mouse_members,
    .n_members = sizeof(mouse_members) / sizeof(mouse_members[0]),
};

// --- vfs ------------------------------------------------------------------
//
// Wraps the shell's filesystem commands (ls, mkdir, cat) under a single
// object so scripts have a typed entry point. Each method delegates to
// shell_dispatch — same scaffolding as the rom_*/hd_*/etc. wrappers.

static value_t vfs_method_ls(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    char line[1024];
    int n;
    if (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s)
        n = snprintf(line, sizeof(line), "ls \"%s\"", argv[0].s);
    else
        n = snprintf(line, sizeof(line), "ls");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("vfs.ls: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t vfs_method_mkdir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vfs.mkdir: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "mkdir \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("vfs.mkdir: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t vfs_method_cat(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vfs.cat: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "cat \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("vfs.cat: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static const arg_decl_t vfs_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Filesystem path"},
};
static const arg_decl_t vfs_path_arg_optional[] = {
    {.name = "path", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Directory path (default: cwd)"},
};

static const member_t vfs_members[] = {
    {.kind = M_METHOD,
     .name = "ls",
     .doc = "List directory contents (or current directory)",
     .method = {.args = vfs_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = vfs_method_ls}},
    {.kind = M_METHOD,
     .name = "mkdir",
     .doc = "Create a directory",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_mkdir}      },
    {.kind = M_METHOD,
     .name = "cat",
     .doc = "Print the contents of a text file",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_cat}        },
};

static const class_desc_t vfs_class = {
    .name = "vfs",
    .members = vfs_members,
    .n_members = sizeof(vfs_members) / sizeof(vfs_members[0]),
};

// --- find -----------------------------------------------------------------
//
// Wraps `find str|bytes|long|word`. Each method takes the literal
// pattern token plus an optional rest string (range / "all" markers)
// and dispatches the joined legacy line. Variadic byte patterns are
// accepted as a space-separated hex string ("4E 71") since the legacy
// parser tokenises on whitespace anyway.

static value_t find_dispatch_kind(const char *kind, int argc, const value_t *argv, const char *err_label) {
    if (argc < 1)
        return val_err("%s: expected (pattern, [rest])", err_label);
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "find %s ", kind);
    if (pos < 0)
        return val_err("%s: format error", err_label);
    // `find bytes` is variadic in the legacy parser: each hex byte
    // must arrive as a separate whitespace-delimited token. Forward
    // the pattern string unquoted so re-tokenisation splits it.
    // `find str` needs the opposite — the pattern is one token even
    // if it contains spaces — so we quote it. Numeric patterns pass
    // through as `0xN`.
    bool is_bytes = (strcmp(kind, "bytes") == 0);
    if (argv[0].kind == V_STRING) {
        if (is_bytes)
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", argv[0].s ? argv[0].s : "");
        else
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\"%s\"", argv[0].s ? argv[0].s : "");
    } else if (argv[0].kind == V_INT) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "0x%llx", (unsigned long long)argv[0].i);
    } else if (argv[0].kind == V_UINT) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "0x%llx", (unsigned long long)argv[0].u);
    } else {
        return val_err("%s: pattern must be string or integer", err_label);
    }
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, " %s", argv[1].s);
    }
    if (pos < 0 || (size_t)pos >= sizeof(buf))
        return val_err("%s: arguments too long", err_label);
    return val_bool(shell_dispatch(buf) == 0);
}

static value_t find_method_str(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_dispatch_kind("str", argc, argv, "find.str");
}

static value_t find_method_bytes(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_dispatch_kind("bytes", argc, argv, "find.bytes");
}

static value_t find_method_long(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_dispatch_kind("long", argc, argv, "find.long");
}

static value_t find_method_word(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_dispatch_kind("word", argc, argv, "find.word");
}

static const arg_decl_t find_str_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Search text"},
    {.name = "rest",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional range [+ \"all\"], e.g. \"$400000..$440000 all\""},
};
static const arg_decl_t find_bytes_args[] = {
    {.name = "hex", .kind = V_STRING, .doc = "Space-separated hex bytes (\"4E 71\")"},
    {.name = "rest", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional range [+ \"all\"]"},
};
static const arg_decl_t find_int_args[] = {
    {.name = "value", .kind = V_INT, .doc = "Integer value to search for"},
    {.name = "rest", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional range [+ \"all\"]"},
};

static const member_t find_members[] = {
    {.kind = M_METHOD,
     .name = "str",
     .doc = "Search memory for a UTF-8 string",
     .method = {.args = find_str_args, .nargs = 2, .result = V_BOOL, .fn = find_method_str}    },
    {.kind = M_METHOD,
     .name = "bytes",
     .doc = "Search memory for a sequence of bytes (hex string)",
     .method = {.args = find_bytes_args, .nargs = 2, .result = V_BOOL, .fn = find_method_bytes}},
    {.kind = M_METHOD,
     .name = "long",
     .doc = "Search memory for a 32-bit integer",
     .method = {.args = find_int_args, .nargs = 2, .result = V_BOOL, .fn = find_method_long}   },
    {.kind = M_METHOD,
     .name = "word",
     .doc = "Search memory for a 16-bit integer",
     .method = {.args = find_int_args, .nargs = 2, .result = V_BOOL, .fn = find_method_word}   },
};

static const class_desc_t find_class = {
    .name = "find",
    .members = find_members,
    .n_members = sizeof(find_members) / sizeof(find_members[0]),
};

// --- keyboard -------------------------------------------------------------
//
// Wraps the legacy `key <name|0xNN>` command. The arg is either a string
// name ("return", "space", "esc", a-z, 0-9 …) or an integer ADB virtual
// keycode (0x00–0x7F). Mirrors the legacy parser in cmd_key.

static value_t keyboard_method_press(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("keyboard.press: expected (key) — name string or keycode int");
    char line[64];
    int n;
    if (argv[0].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "key %s", argv[0].s ? argv[0].s : "");
    } else if (argv[0].kind == V_INT) {
        n = snprintf(line, sizeof(line), "key 0x%02llx", (long long)argv[0].i);
    } else if (argv[0].kind == V_UINT) {
        n = snprintf(line, sizeof(line), "key 0x%02llx", (long long)argv[0].u);
    } else {
        return val_err("keyboard.press: key must be a string name or integer keycode");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("keyboard.press: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static const arg_decl_t keyboard_press_args[] = {
    {.name = "key", .kind = V_STRING, .doc = "Key name (\"return\"/\"esc\"/\"a\"/...) or ADB keycode int"},
};

static const member_t keyboard_members[] = {
    {.kind = M_METHOD,
     .name = "press",
     .doc = "Tap a key (down + up) on the emulated keyboard",
     .method = {.args = keyboard_press_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_press}},
};

static const class_desc_t keyboard_class = {
    .name = "keyboard",
    .members = keyboard_members,
    .n_members = sizeof(keyboard_members) / sizeof(keyboard_members[0]),
};

// --- screen ---------------------------------------------------------------
//
// Wraps the legacy `screenshot` subcommand family. Each method
// delegates to shell_dispatch so the underlying framebuffer logic in
// debug.c stays the single source of truth — same scaffolding pattern
// as the rom_*/hd_*/etc. wrappers.

static value_t screen_method_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.save: expected (path)");
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("screen.save: empty path");
    size_t n = strlen(path);
    if (n < 4 || strcasecmp(path + n - 4, ".png") != 0)
        return val_err("screen.save: path must end in .png (got '%s')", path);
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.save: framebuffer not available");
    if (save_framebuffer_as_png(fb, path) < 0)
        return val_err("screen.save: failed to save '%s'", path);
    return val_bool(true);
}

static value_t screen_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.match: expected (reference_path)");
    const char *ref = argv[0].s ? argv[0].s : "";
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.match: framebuffer not available");
    int result = match_framebuffer_with_png(fb, ref);
    if (result < 0) {
        printf("MATCH FAILED: Error loading reference image.\n");
        return val_bool(false);
    }
    if (result == 0) {
        printf("MATCH OK: Screen matches '%s'.\n", ref);
        return val_bool(true);
    }
    printf("MATCH FAILED: Screen does not match '%s'.\n", ref);
    return val_bool(false);
}

static value_t screen_method_match_or_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.match_or_save: expected (reference_path, [actual_path])");
    const char *ref = argv[0].s ? argv[0].s : "";
    const char *actual = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.match_or_save: framebuffer not available");
    int result = match_framebuffer_with_png(fb, ref);
    if (result < 0) {
        printf("MATCH FAILED: Error loading reference image.\n");
        if (actual)
            save_framebuffer_as_png(fb, actual);
        return val_bool(false);
    }
    if (result == 0) {
        printf("MATCH OK: Screen matches '%s'.\n", ref);
        return val_bool(true);
    }
    if (actual) {
        save_framebuffer_as_png(fb, actual);
        printf("MATCH FAILED: Screen does not match '%s'. Saved actual to '%s'.\n", ref, actual);
    } else {
        printf("MATCH FAILED: Screen does not match '%s'.\n", ref);
    }
    return val_bool(false);
}

static value_t screen_method_checksum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.checksum: framebuffer not available");
    if (argc == 0)
        return val_int((int64_t)(int32_t)framebuffer_checksum(fb));
    if (argc < 4)
        return val_err("screen.checksum: expected (top, left, bottom, right) or no args");
    bool ok = true;
    int64_t t = val_as_i64(&argv[0], &ok);
    int64_t l = val_as_i64(&argv[1], &ok);
    int64_t b = val_as_i64(&argv[2], &ok);
    int64_t r = val_as_i64(&argv[3], &ok);
    if (!ok)
        return val_err("screen.checksum: region args must be integers");
    if (t < 0 || l < 0 || b <= t || r <= l || b > DEBUG_SCREEN_HEIGHT || r > DEBUG_SCREEN_WIDTH)
        return val_err("screen.checksum: invalid region bounds (0,0)-(%d,%d)", DEBUG_SCREEN_WIDTH, DEBUG_SCREEN_HEIGHT);
    return val_int((int64_t)(int32_t)framebuffer_region_checksum(fb, (int)t, (int)l, (int)b, (int)r));
}

static const arg_decl_t screen_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output PNG path (must end in .png)"},
};
static const arg_decl_t screen_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
};
static const arg_decl_t screen_match_or_save_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
    {.name = "actual", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Path to write current screen on miss"},
};
static const arg_decl_t screen_checksum_args[] = {
    {.name = "top",    .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region top edge"   },
    {.name = "left",   .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region left edge"  },
    {.name = "bottom", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region bottom edge"},
    {.name = "right",  .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region right edge" },
};

static const member_t screen_members[] = {
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Save the current framebuffer to a PNG file",
     .method = {.args = screen_save_args, .nargs = 1, .result = V_BOOL, .fn = screen_method_save}                  },
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the framebuffer against a reference PNG (true if identical)",
     .method = {.args = screen_match_args, .nargs = 1, .result = V_BOOL, .fn = screen_method_match}                },
    {.kind = M_METHOD,
     .name = "match_or_save",
     .doc = "Like `match`, but also write the current screen to `actual` on mismatch",
     .method = {.args = screen_match_or_save_args, .nargs = 2, .result = V_BOOL, .fn = screen_method_match_or_save}},
    {.kind = M_METHOD,
     .name = "checksum",
     .doc = "Polynomial hash of the framebuffer (full screen or top/left/bottom/right region)",
     .method = {.args = screen_checksum_args, .nargs = 4, .result = V_INT, .fn = screen_method_checksum}           },
};

static const class_desc_t screen_class = {
    .name = "screen",
    .members = screen_members,
    .n_members = sizeof(screen_members) / sizeof(screen_members[0]),
};

// === M8 (slice 2) — storage object with images indexed children ============
//
// Replaces the M2 `storage` stub with a real class. `storage.images`
// enumerates the cfg->images[] entries. Slot index in the indexed
// child matches the slot in cfg->images[]; n_images is dense from
// 0..n_images-1, so the collection's count() returns cfg->n_images
// and next(prev) advances to prev+1 until n_images.
//
// `storage.import(host_path, dst_path)` is reserved per proposal §5.7
// but stubbed with a deferral error — image_persist_volatile + the
// surrounding plumbing land in a follow-up M8 sub-commit.

typedef struct {
    config_t *cfg;
    int slot;
} storage_image_data_t;

static storage_image_data_t g_storage_image_data[MAX_IMAGES];
static struct object *g_storage_image_objs[MAX_IMAGES];

static image_t *storage_image_at(struct object *self) {
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    if (!d || !d->cfg)
        return NULL;
    if (d->slot < 0 || d->slot >= d->cfg->n_images)
        return NULL;
    return d->cfg->images[d->slot];
}

static value_t storage_image_attr_index(struct object *self, const member_t *m) {
    (void)m;
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    return val_int(d ? d->slot : -1);
}
static value_t storage_image_attr_filename(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_get_filename(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_path(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_path(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_uint(8, img ? (uint64_t)img->raw_size : 0);
}
static value_t storage_image_attr_writable(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_bool(img ? img->writable : false);
}

static const char *const STORAGE_IMAGE_TYPE_NAMES[] = {
    "other", "fd_ss", "fd_ds", "fd_hd", "hd", "cdrom",
};

static value_t storage_image_attr_type(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    int t = img ? (int)img->type : 0;
    int max = (int)(sizeof(STORAGE_IMAGE_TYPE_NAMES) / sizeof(STORAGE_IMAGE_TYPE_NAMES[0]));
    if (t < 0 || t >= max)
        t = 0;
    return val_enum(t, STORAGE_IMAGE_TYPE_NAMES, (size_t)max);
}

static const member_t storage_image_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = storage_image_attr_index, .set = NULL}      },
    {.kind = M_ATTR,
     .name = "filename",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_filename, .set = NULL}},
    {.kind = M_ATTR,
     .name = "path",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_path, .set = NULL}    },
    {.kind = M_ATTR,
     .name = "raw_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = storage_image_attr_raw_size, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "writable",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = storage_image_attr_writable, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "type",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = storage_image_attr_type, .set = NULL}      },
};

static const class_desc_t storage_image_class = {
    .name = "image",
    .members = storage_image_members,
    .n_members = sizeof(storage_image_members) / sizeof(storage_image_members[0]),
};

static struct object *storage_images_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || index < 0 || index >= MAX_IMAGES)
        return NULL;
    if (index >= cfg->n_images || !cfg->images[index])
        return NULL;
    return g_storage_image_objs[index];
}
static int storage_images_count(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return 0;
    int n = 0;
    for (int i = 0; i < cfg->n_images; i++)
        if (cfg->images[i])
            n++;
    return n;
}
static int storage_images_next(struct object *self, int prev_index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return -1;
    for (int i = prev_index + 1; i < cfg->n_images; i++)
        if (cfg->images[i])
            return i;
    return -1;
}

// `storage.import(host_path, dst_path?)` — copy `host_path` into the
// emulator's persistent storage. When `dst_path` is empty (or absent
// — second arg is optional), falls back to the content-hash path
// produced by image_persist_volatile (/opfs/images/<hash>.img). When
// `dst_path` is non-empty, the source is copied verbatim through the
// VFS so paths like "/opfs/images/foo.img" can be picked explicitly.
//
// Returns the resolved destination path as a V_STRING.
static value_t storage_method_import(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.import: expected (host_path, [dst_path])");
    const char *host_path = argv[0].s;
    const char *dst_path = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;

    if (!dst_path) {
        // Hash-named persistence — handles the drag-drop / volatile-
        // path case and is idempotent on repeat imports.
        char *resolved = image_persist_volatile(host_path);
        if (!resolved)
            return val_err("storage.import: failed to persist '%s'", host_path);
        value_t v = val_str(resolved);
        free(resolved);
        return v;
    }

    // Explicit destination — defer to the legacy `cp` command via
    // shell_dispatch so quoting and VFS handling stay in one place.
    char line[1024];
    int n = snprintf(line, sizeof(line), "cp \"%s\" \"%s\"", host_path, dst_path);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("storage.import: arguments too long");
    if (shell_dispatch(line) != 0)
        return val_err("storage.import: cp '%s' -> '%s' failed", host_path, dst_path);
    return val_str(dst_path);
}

static const arg_decl_t storage_import_args[] = {
    {.name = "host_path", .kind = V_STRING, .doc = "Host path to read"},
    {.name = "dst_path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Destination path; empty → /opfs/images/<hash>.img"},
};

static const member_t storage_images_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &storage_image_class,
               .indexed = true,
               .get = storage_images_get,
               .count = storage_images_count,
               .next = storage_images_next,
               .lookup = NULL}},
};

static const class_desc_t storage_images_collection_class = {
    .name = "storage_images",
    .members = storage_images_collection_members,
    .n_members = sizeof(storage_images_collection_members) / sizeof(storage_images_collection_members[0]),
};

// `storage.list_dir(path)` — list directory entries via the VFS as a
// V_LIST<V_STRING>. Powers the M10b/c migration of url-media.js's
// legacy `ls $ROMS_DIR` (whose stdout-only output had no typed
// successor until now).
static value_t storage_method_list_dir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.list_dir: expected (path)");
    vfs_dir_t *d = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(argv[0].s, &d, &be);
    if (rc < 0 || !d || !be)
        return val_list(NULL, 0); // empty list (treat unreadable dirs as no entries)
    size_t cap = 16, n = 0;
    value_t *items = (value_t *)calloc(cap, sizeof(value_t));
    if (!items) {
        be->closedir(d);
        return val_err("storage.list_dir: out of memory");
    }
    vfs_dirent_t ent;
    while (be->readdir(d, &ent) > 0) {
        if (ent.name[0] == '.' && (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0')))
            continue;
        if (n >= cap) {
            size_t new_cap = cap * 2;
            value_t *nb = (value_t *)realloc(items, new_cap * sizeof(value_t));
            if (!nb) {
                for (size_t i = 0; i < n; i++)
                    value_free(&items[i]);
                free(items);
                be->closedir(d);
                return val_err("storage.list_dir: out of memory");
            }
            items = nb;
            cap = new_cap;
        }
        items[n++] = val_str(ent.name);
    }
    be->closedir(d);
    return val_list(items, n);
}

static const arg_decl_t storage_list_dir_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Directory path"},
};

static const member_t storage_members[] = {
    {.kind = M_METHOD,
     .name = "import",
     .doc = "Persist a host file under /images/ (deferred — see proposal §5.7)",
     .method = {.args = storage_import_args, .nargs = 2, .result = V_STRING, .fn = storage_method_import}  },
    {.kind = M_METHOD,
     .name = "list_dir",
     .doc = "List directory entries (V_LIST of V_STRING names)",
     .method = {.args = storage_list_dir_args, .nargs = 1, .result = V_LIST, .fn = storage_method_list_dir}},
};

static const class_desc_t storage_class_real = {
    .name = "storage",
    .members = storage_members,
    .n_members = sizeof(storage_members) / sizeof(storage_members[0]),
};

// === M8 (slice 3) — top-level root methods ==================================
//
// Registers the introspection-and-utility subset of proposal §5.10's
// root methods: `objects`, `attributes`, `methods`, `help`, `print`,
// and `time`. These are the ones with no dependency on legacy command
// internals — wrappers for `cp`, `peeler`, `hd_*`, `rom_*`, `vrom_*`,
// `partmap`, `probe`, `list_partitions`, `unmount`, `let`, `quit`,
// `source`, `hd_create`, `hd_download` defer to a follow-up
// substrate-and-shell sub-commit (the `quit` / `source` ones in
// particular need shell-state plumbing).
//
// All five introspection methods accept an optional path string; an
// empty / missing path resolves to the root itself.

static struct object *resolve_target(const value_t *path_arg) {
    const char *path = (path_arg && path_arg->kind == V_STRING && path_arg->s) ? path_arg->s : "";
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n))
        return NULL;
    // For attribute / method nodes we report on the parent object's
    // class. For object-typed nodes (M_CHILD or named children) we
    // descend to the target object.
    if (!n.member)
        return n.obj;
    if (n.member->kind != M_CHILD)
        return n.obj;
    if (n.member->child.indexed) {
        if (n.index < 0 || !n.member->child.get)
            return n.obj;
        struct object *c = n.member->child.get(n.obj, n.index);
        return c ? c : n.obj;
    }
    if (n.member->child.lookup) {
        struct object *c = n.member->child.lookup(n.obj, n.member->name);
        if (c)
            return c;
    }
    return n.obj;
}

// Append `name` as a V_STRING into a growing items array. Returns
// false on allocation failure (caller falls through to V_LIST with
// what's been accumulated).
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} string_list_acc_t;

static bool string_list_push(string_list_acc_t *acc, const char *name) {
    if (!name)
        return true;
    if (acc->len + 1 > acc->cap) {
        size_t cap = acc->cap ? acc->cap * 2 : 16;
        value_t *t = (value_t *)realloc(acc->items, cap * sizeof(value_t));
        if (!t)
            return false;
        acc->items = t;
        acc->cap = cap;
    }
    acc->items[acc->len++] = val_str(name);
    return true;
}

// Walks the class member table, pushing names of members matching
// `kind`. Plus, for kind == M_CHILD, also enumerates runtime-attached
// named children (object_each_attached).
typedef struct {
    string_list_acc_t *acc;
    member_kind_t kind; // 0 (M_ATTR base) means "any"
} class_walker_t;

static void each_attached_collect(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    string_list_acc_t *acc = (string_list_acc_t *)ud;
    string_list_push(acc, object_name(child));
}

static value_t method_root_objects(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("objects: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_CHILD)
                string_list_push(&acc, cls->members[i].name);
    }
    object_each_attached(target, each_attached_collect, &acc);
    return val_list(acc.items, acc.len);
}

static value_t method_root_attributes(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("attributes: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_ATTR)
                string_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

static value_t method_root_methods(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("methods: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_METHOD)
                string_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

// `help(path?)` — return the doc string of the resolved member. For
// object-typed nodes, returns the class name (no separate "class doc"
// field exists in the substrate yet).
static value_t method_root_help(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].kind == V_STRING && argv[0].s) ? argv[0].s : "";
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n))
        return val_err("help: path did not resolve");
    if (n.member && n.member->doc)
        return val_str(n.member->doc);
    if (n.member)
        return val_str(n.member->name ? n.member->name : "");
    const class_desc_t *cls = object_class(n.obj);
    return val_str(cls && cls->name ? cls->name : "");
}

// `time()` — wall-clock seconds since the Unix epoch. Useful for
// timestamping log lines from scripts; deterministic test runs use
// `rtc.time =` (M7b) instead.
static value_t method_root_time(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_uint(8, (uint64_t)time(NULL));
}

// `print(value)` — formats a value as a string. The implementation
// just round-trips through V_STRING for now: numerics → decimal/hex
// per flags, strings stay strings, others get a class-shaped tag.
// This matches the proposal's §5.10 listing without committing to a
// rich formatter (which lands with M9 / M10).
static value_t method_root_print(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_str("");
    const value_t *v = &argv[0];
    char buf[256];
    switch (v->kind) {
    case V_NONE:
        return val_str("");
    case V_BOOL:
        return val_str(v->b ? "true" : "false");
    case V_INT:
        snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
        return val_str(buf);
    case V_UINT:
        if (v->flags & VAL_HEX)
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v->u);
        else
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v->u);
        return val_str(buf);
    case V_FLOAT:
        snprintf(buf, sizeof(buf), "%g", v->f);
        return val_str(buf);
    case V_STRING:
        return val_str(v->s ? v->s : "");
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            return val_str(v->enm.table[v->enm.idx]);
        snprintf(buf, sizeof(buf), "<enum:%d>", v->enm.idx);
        return val_str(buf);
    case V_OBJECT: {
        const class_desc_t *cls = v->obj ? object_class(v->obj) : NULL;
        snprintf(buf, sizeof(buf), "<object:%s>", cls && cls->name ? cls->name : "?");
        return val_str(buf);
    }
    default:
        return val_str("<value>");
    }
}

// --- M8 (slice 4) — root methods that wrap legacy shell commands ----------
//
// Per proposal §5.10 these are top-level methods that flatten existing
// `image foo` / `hd foo` / `rom foo` / `vrom foo` / `peeler` / `cp` /
// `quit` shell forms into one-call methods. Each wrapper builds a
// quoted command line and routes it through shell_dispatch — that
// keeps argument parsing and quoting in one place (the shell tokenizer)
// and avoids duplicating the per-command argument validation.
//
// `let` and `source` defer to a later sub-commit — they touch
// shell-state plumbing (variables, script context) that the M9–M10
// cutover rewrites end-to-end.
//
// `hd_download` defers similarly — it requires platform/network state
// outside the scope of the object tree.

// Append `s` to `out` enclosed in double quotes. Backslashes and
// embedded double quotes are escaped to match the shell tokenizer's
// expectations. Returns false if the buffer is full.
static bool append_quoted(char *out, size_t cap, size_t *pos, const char *s) {
    if (!s)
        s = "";
    if (*pos + 1 >= cap)
        return false;
    out[(*pos)++] = '"';
    for (const char *p = s; *p; p++) {
        if (*pos + 2 >= cap)
            return false;
        if (*p == '"' || *p == '\\')
            out[(*pos)++] = '\\';
        out[(*pos)++] = *p;
    }
    if (*pos + 1 >= cap)
        return false;
    out[(*pos)++] = '"';
    out[*pos] = '\0';
    return true;
}

static bool append_literal(char *out, size_t cap, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap)
        return false;
    memcpy(out + *pos, s, n);
    *pos += n;
    out[*pos] = '\0';
    return true;
}

// Build a shell line "<cmd> [arg1 [arg2 [...]]]" from V_STRING argv,
// then dispatch. Returns 0 on dispatch success (legacy command was
// found and ran to completion), -1 if the command line couldn't be
// built. The legacy commands' own return codes are intentionally
// ignored — different commands use the int return for unrelated
// purposes (rom_validate uses 1 for "valid", cp uses byte counts on
// some paths, …) so a uniform success-vs-error read on it is wrong.
// Commands that fail print to stderr; the caller reads that out of
// band, just like at the shell.
// Build a shell-form line "<cmd> <arg> <arg>..." (each arg double-quoted)
// and dispatch it. Returns -1 on a build error (non-string arg, line too
// long), otherwise the legacy command's int return cast through uint64_t.
// Caller decides how to interpret the value — most legacy commands use
// 0 = success, but some return cmd_bool (1 = success). The two helpers
// below codify each convention so the migration can stop carrying the
// inconsistency.
static int64_t dispatch_with_string_args(const char *cmd, int argc, const value_t *argv) {
    char line[1024];
    size_t pos = 0;
    line[0] = '\0';
    if (!append_literal(line, sizeof(line), &pos, cmd))
        return -1;
    for (int i = 0; i < argc; i++) {
        if (argv[i].kind != V_STRING)
            return -1;
        if (!append_literal(line, sizeof(line), &pos, " "))
            return -1;
        if (!append_quoted(line, sizeof(line), &pos, argv[i].s ? argv[i].s : ""))
            return -1;
    }
    return (int64_t)shell_dispatch(line);
}

// `cp(src, dst, [flags])` — top-level alias for `storage.import` per
// proposal §5.10 ("preserve UNIX muscle memory"). Routes through the
// legacy `cp` command directly to keep -r/-R handling in one place.
// Returns true on success, false on dispatch / copy failure.
static value_t method_root_cp(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("cp: expected (src, dst, [flags])");
    int64_t rc = dispatch_with_string_args("cp", argc, argv);
    if (rc < 0)
        return val_err("cp: dispatch failed (command line too long?)");
    return val_bool(rc == 0);
}

// `peeler(path, [out_dir])` — extract a Mac archive
// (.sit/.cpt/.hqx/.bin) via the legacy `peeler` shell command.
// Returns true on successful extraction. Note: legacy peeler takes the
// output directory via `-o <dir>` (not as a positional arg), so this
// wrapper builds the line by hand rather than going through
// dispatch_with_string_args.
static value_t method_root_peeler(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("peeler: expected (path, [out_dir])");
    const char *path = argv[0].s ? argv[0].s : "";
    const char *out_dir = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    char line[1024];
    int n = out_dir ? snprintf(line, sizeof(line), "peeler -o \"%s\" \"%s\"", out_dir, path)
                    : snprintf(line, sizeof(line), "peeler \"%s\"", path);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("peeler: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `peeler_probe(path)` — true if the given file is a peeler-supported
// archive (`peeler --probe` returns 0 on success).
static value_t method_root_peeler_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("peeler_probe: expected (path)");
    char line[512];
    int n = snprintf(line, sizeof(line), "peeler --probe \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("peeler_probe: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `fd_probe(path)` — true if the file passes legacy `fd probe`.
static value_t method_root_fd_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_probe: expected (path)");
    char line[512];
    int n = snprintf(line, sizeof(line), "fd probe \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("fd_probe: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `find_media(dir, [dst])` — search a directory for a recognised
// floppy image; if `dst` is given, the image is copied there. Returns
// true if a match was found.
static value_t method_root_find_media(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("find_media: expected (dir, [dst])");
    int64_t rc = dispatch_with_string_args("find-media", argc, argv);
    if (rc < 0)
        return val_err("find_media: dispatch failed");
    return val_bool(rc == 0);
}

// `hd_create(path, size)` — wraps `hd create <path> <size>`.
static value_t method_root_hd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("hd_create: expected (path, size)");
    char line[512];
    int n;
    // size accepts either a string ("HD20SC", "40M", "21411840") or a
    // bare integer (raw byte count). The shell-form size parser handles
    // both, so just stringify whichever variant we got.
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" \"%s\"", argv[0].s, argv[1].s);
    } else if (argv[1].kind == V_INT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %lld", argv[0].s, (long long)argv[1].i);
    } else if (argv[1].kind == V_UINT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %llu", argv[0].s, (unsigned long long)argv[1].u);
    } else {
        return val_err("hd_create: size must be string or integer");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_create: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `hd_download(src, dst)` — export a hard disk image (base + delta) to a
// flat file. Wraps the legacy `hd download` command, which the headless
// build does support (the WASM platform's fetch glue is a separate
// concern that the typed wrapper doesn't change).
static value_t method_root_hd_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("hd_download: expected (source_path, dest_path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "hd download \"%s\" \"%s\"", argv[0].s ? argv[0].s : "",
                     argv[1].s ? argv[1].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_download: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// rom_* / vrom_* — wrap the `rom probe|validate` / `vrom probe|validate`
// subcommand forms. Each takes a single path argument. Two helpers
// codify the two legacy return conventions: cmd_int (0 = success) for
// the *_probe family and cmd_bool (1 = valid) for the *_validate family.
static int64_t dispatch_subcmd_path_rc(const char *cmd, const char *sub, int argc, const value_t *argv) {
    if (argc < 1 || argv[0].kind != V_STRING)
        return -2; // signal "bad arg" distinct from dispatch failure
    char line[512];
    int n = snprintf(line, sizeof(line), "%s %s \"%s\"", cmd, sub, argv[0].s);
    if (n < 0 || (size_t)n >= sizeof(line))
        return -1;
    return (int64_t)shell_dispatch(line);
}

static value_t method_root_rom_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    // No-arg form: probe the currently loaded ROM.
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s || !*argv[0].s) {
        char line[16];
        snprintf(line, sizeof(line), "rom probe");
        return val_bool(shell_dispatch(line) == 0);
    }
    int64_t rc = dispatch_subcmd_path_rc("rom", "probe", argc, argv);
    if (rc == -2)
        return val_err("rom_probe: expected (path)");
    if (rc == -1)
        return val_err("rom_probe: argument too long");
    return val_bool(rc == 0);
}
static value_t method_root_rom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t rc = dispatch_subcmd_path_rc("rom", "validate", argc, argv);
    if (rc == -2)
        return val_err("rom_validate: expected (path)");
    if (rc == -1)
        return val_err("rom_validate: argument too long");
    // cmd_bool semantics: 1 = valid.
    return val_bool(rc == 1);
}
static value_t method_root_vrom_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t rc = dispatch_subcmd_path_rc("vrom", "probe", argc, argv);
    if (rc == -2)
        return val_err("vrom_probe: expected (path)");
    if (rc == -1)
        return val_err("vrom_probe: argument too long");
    return val_bool(rc == 0);
}
static value_t method_root_vrom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t rc = dispatch_subcmd_path_rc("vrom", "validate", argc, argv);
    if (rc == -2)
        return val_err("vrom_validate: expected (path)");
    if (rc == -1)
        return val_err("vrom_validate: argument too long");
    return val_bool(rc == 1);
}

// image_* — wraps the `image partmap|probe|list|unmount` subcommands
// per proposal §5.10's "Legacy `image foo` subcommand machinery
// becomes shims that flatten to top-level method calls". These four
// commands print info to stdout and return cmd_int; we expose the
// success bit so callers can branch on it.
static value_t image_subcmd_bool(const char *sub, int argc, const value_t *argv, const char *err_prefix) {
    int64_t rc = dispatch_subcmd_path_rc("image", sub, argc, argv);
    if (rc == -2)
        return val_err("%s: expected (path)", err_prefix);
    if (rc == -1)
        return val_err("%s: argument too long", err_prefix);
    return val_bool(rc == 0);
}
static value_t method_root_partmap(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return image_subcmd_bool("partmap", argc, argv, "partmap");
}
static value_t method_root_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return image_subcmd_bool("probe", argc, argv, "probe");
}
static value_t method_root_list_partitions(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return image_subcmd_bool("list", argc, argv, "list_partitions");
}
static value_t method_root_unmount(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return image_subcmd_bool("unmount", argc, argv, "unmount");
}

// `quit()` — dispatches the legacy `quit` command. The headless main
// loop already handles the exit signal it raises; the WASM platform
// turns it into a "stop running" notification.
static value_t method_root_quit(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    char line[8] = "quit";
    shell_dispatch(line);
    return val_none();
}

// === Checkpoint root methods (M10b — checkpoint area) ======================
//
// Thin V_BOOL wrappers around the legacy `checkpoint <subcmd>` form.
// `running()` exposes scheduler_is_running so JS can replace
// `runCommand('status') === 1` with a direct boolean read.

static value_t method_root_checkpoint_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    char line[24];
    snprintf(line, sizeof(line), "checkpoint --probe");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t method_root_checkpoint_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    char line[24];
    snprintf(line, sizeof(line), "checkpoint clear");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t method_root_checkpoint_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    char line[1024];
    int n;
    if (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s)
        n = snprintf(line, sizeof(line), "checkpoint --load \"%s\"", argv[0].s);
    else
        n = snprintf(line, sizeof(line), "checkpoint --load");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("checkpoint_load: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t method_root_checkpoint_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("checkpoint_save: expected (path, [mode])");
    char line[1024];
    int n;
    const char *path = argv[0].s ? argv[0].s : "";
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s)
        n = snprintf(line, sizeof(line), "checkpoint --save \"%s\" %s", path, argv[1].s);
    else
        n = snprintf(line, sizeof(line), "checkpoint --save \"%s\"", path);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("checkpoint_save: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t method_root_register_machine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("register_machine: expected (id, created)");
    char line[256];
    int n = snprintf(line, sizeof(line), "checkpoint --machine \"%s\" \"%s\"", argv[0].s ? argv[0].s : "",
                     argv[1].s ? argv[1].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("register_machine: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `auto_checkpoint` attribute (V_BOOL, rw) — exposes the WASM
// background-checkpoint loop's enabled flag. Headless's weak defaults
// stub out (no auto-checkpoint there), so reads return false and
// writes are silently ignored on that platform.
static value_t attr_auto_checkpoint_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(gs_checkpoint_auto_get());
}

static value_t attr_auto_checkpoint_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    bool b = val_as_bool(&in);
    value_free(&in);
    gs_checkpoint_auto_set(b);
    return val_none();
}

// `running()` — true if the scheduler is currently running. Mirrors
// the legacy `status` command's int return (cmd_int 1 = running).
static value_t method_root_running(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    return val_bool(s ? scheduler_is_running(s) : false);
}

// === ROM / disk-mount / scheduler root methods (M10b — drop area) ==========

// `rom_checksum(path)` — return the 8-char hex checksum of a ROM file,
// or empty string when the file doesn't validate. Mirrors the legacy
// `rom checksum` command's printed output as a typed return value, so
// JS can avoid runCommandJSON + stdout-parsing.
static value_t method_root_rom_checksum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("rom_checksum: expected (path)");
    FILE *f = fopen(argv[0].s, "rb");
    if (!f)
        return val_str("");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return val_str("");
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fclose(f);
        return val_str("");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return val_str("");
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return val_str("");
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return val_str("");
    }
    uint32_t cksum = 0;
    const rom_info_t *info = rom_identify_data(buf, (size_t)sz, &cksum);
    free(buf);
    if (!info)
        return val_str("");
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", cksum);
    return val_str(hex);
}

static value_t method_root_rom_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("rom_load: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "rom load \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("rom_load: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `fd_insert(path, slot, writable)` — mount a floppy image into one of
// the 1–2 floppy drives. Returns true on successful insert.
static value_t method_root_fd_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("fd_insert: expected (path, slot, [writable])");
    int64_t slot = 0;
    bool ok = false;
    slot = (int64_t)val_as_i64(&argv[1], &ok);
    if (!ok && argv[1].kind == V_UINT)
        slot = (int64_t)argv[1].u;
    bool writable = false;
    if (argc >= 3)
        writable = val_as_bool(&argv[2]);
    char line[1024];
    int n = snprintf(line, sizeof(line), "fd insert \"%s\" %lld %s", argv[0].s ? argv[0].s : "", (long long)slot,
                     writable ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("fd_insert: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `run([cycles])` — start the scheduler. With no argument, runs
// indefinitely (until paused / exception). With a cycle count, runs
// for that many CPU cycles and pauses.
static value_t method_root_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    char line[64];
    int n;
    if (argc >= 1) {
        bool ok = false;
        int64_t cycles = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            cycles = (int64_t)argv[0].u;
        n = snprintf(line, sizeof(line), "run %lld", (long long)cycles);
    } else {
        n = snprintf(line, sizeof(line), "run");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("run: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// === Boot/setup root methods (M10b — url-media + config-dialog area) =======

// `vrom_load(path)` — set the video-ROM path for the next machine init.
static value_t method_root_vrom_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vrom_load: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "vrom load \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("vrom_load: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `hd_attach(path, id)` — attach a hard-disk image at the given SCSI id.
static value_t method_root_hd_attach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("hd_attach: expected (path, [id])");
    int64_t id = 0; // Legacy default — matches cmd_hd_handler's `attach` branch.
    if (argc >= 2) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[1], &ok);
        if (!ok && argv[1].kind == V_UINT)
            id = (int64_t)argv[1].u;
    }
    char line[1024];
    int n = snprintf(line, sizeof(line), "hd attach \"%s\" %lld", argv[0].s ? argv[0].s : "", (long long)id);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_attach: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// === dump_tree (M12 — auto-generated docs) =================================
//
// Walk the live object tree and emit one JSON description of every
// reachable node. The frontend doesn't consume this — it powers
// scripts/dump_object_model.py, which renders docs/object-model-
// reference.md. Doing the walk in C keeps the format stable across
// model changes; the python only formats markdown.

struct dump_buf {
    char *buf;
    size_t cap;
    size_t pos;
    bool overflow;
};

static void dump_append(struct dump_buf *b, const char *s) {
    if (!s || b->overflow)
        return;
    size_t n = strlen(s);
    if (b->pos + n + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < b->pos + n + 1)
            new_cap *= 2;
        char *nb = (char *)realloc(b->buf, new_cap);
        if (!nb) {
            b->overflow = true;
            return;
        }
        b->buf = nb;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->pos, s, n);
    b->pos += n;
    b->buf[b->pos] = '\0';
}

static void dump_appendf(struct dump_buf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    dump_append(b, tmp);
}

static void dump_append_jstring(struct dump_buf *b, const char *s) {
    dump_append(b, "\"");
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
            case '"':
                dump_append(b, "\\\"");
                break;
            case '\\':
                dump_append(b, "\\\\");
                break;
            case '\n':
                dump_append(b, "\\n");
                break;
            case '\r':
                dump_append(b, "\\r");
                break;
            case '\t':
                dump_append(b, "\\t");
                break;
            default:
                if (c < 0x20)
                    dump_appendf(b, "\\u%04x", c);
                else {
                    char ch[2] = {(char)c, '\0'};
                    dump_append(b, ch);
                }
            }
        }
    }
    dump_append(b, "\"");
}

static const char *value_kind_name(value_kind_t k) {
    switch (k) {
    case V_NONE:
        return "none";
    case V_BOOL:
        return "bool";
    case V_INT:
        return "int";
    case V_UINT:
        return "uint";
    case V_FLOAT:
        return "float";
    case V_STRING:
        return "string";
    case V_BYTES:
        return "bytes";
    case V_ENUM:
        return "enum";
    case V_LIST:
        return "list";
    case V_OBJECT:
        return "object";
    case V_ERROR:
        return "error";
    }
    return "?";
}

static void dump_object_recursive(struct dump_buf *b, struct object *o, const char *path);

struct dump_child_acc {
    struct dump_buf *b;
    const char *path;
    bool first;
};

static void dump_attached_cb(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    struct dump_child_acc *acc = (struct dump_child_acc *)ud;
    const char *name = object_name(child);
    if (!name)
        return;
    if (!acc->first)
        dump_append(acc->b, ",");
    acc->first = false;
    char child_path[256];
    if (acc->path && *acc->path)
        snprintf(child_path, sizeof(child_path), "%s.%s", acc->path, name);
    else
        snprintf(child_path, sizeof(child_path), "%s", name);
    dump_object_recursive(acc->b, child, child_path);
}

static void dump_object_recursive(struct dump_buf *b, struct object *o, const char *path) {
    if (!o)
        return;
    const class_desc_t *cls = object_class(o);
    dump_append(b, "{\"path\":");
    dump_append_jstring(b, path ? path : "");
    dump_append(b, ",\"class\":");
    dump_append_jstring(b, (cls && cls->name) ? cls->name : "");
    dump_append(b, ",\"members\":[");
    bool first_m = true;
    if (cls && cls->members) {
        for (size_t i = 0; i < cls->n_members; i++) {
            const member_t *mem = &cls->members[i];
            if (!mem->name)
                continue;
            if (!first_m)
                dump_append(b, ",");
            first_m = false;
            dump_append(b, "{\"name\":");
            dump_append_jstring(b, mem->name);
            dump_append(b, ",\"kind\":");
            switch (mem->kind) {
            case M_ATTR:
                dump_append(b, "\"attr\",\"type\":");
                dump_append_jstring(b, value_kind_name(mem->attr.type));
                dump_append(b, ",\"writable\":");
                dump_append(b, mem->attr.set ? "true" : "false");
                break;
            case M_METHOD: {
                dump_append(b, "\"method\",\"result\":");
                dump_append_jstring(b, value_kind_name(mem->method.result));
                dump_append(b, ",\"args\":[");
                for (int a = 0; a < mem->method.nargs; a++) {
                    if (a)
                        dump_append(b, ",");
                    const arg_decl_t *ad = &mem->method.args[a];
                    dump_append(b, "{\"name\":");
                    dump_append_jstring(b, ad->name ? ad->name : "");
                    dump_append(b, ",\"kind\":");
                    dump_append_jstring(b, value_kind_name(ad->kind));
                    dump_append(b, ",\"optional\":");
                    dump_append(b, (ad->flags & OBJ_ARG_OPTIONAL) ? "true" : "false");
                    if (ad->doc) {
                        dump_append(b, ",\"doc\":");
                        dump_append_jstring(b, ad->doc);
                    }
                    dump_append(b, "}");
                }
                dump_append(b, "]");
                break;
            }
            case M_CHILD:
                dump_append(b, "\"child\",\"indexed\":");
                dump_append(b, mem->child.indexed ? "true" : "false");
                if (mem->child.cls && mem->child.cls->name) {
                    dump_append(b, ",\"class\":");
                    dump_append_jstring(b, mem->child.cls->name);
                }
                break;
            }
            if (mem->doc) {
                dump_append(b, ",\"doc\":");
                dump_append_jstring(b, mem->doc);
            }
            dump_append(b, "}");
        }
    }
    dump_append(b, "],\"children\":[");
    struct dump_child_acc acc = {.b = b, .path = path, .first = true};
    object_each_attached(o, dump_attached_cb, &acc);
    dump_append(b, "]}");
}

// === Debugging-area root methods ===========================================
//
// Thin wrappers around the legacy `info` / `d` / `break` / `logpoint` /
// `log` shell commands. The legacy parsing stays in their respective
// handlers; these methods exist so test scripts and the typed-bridge
// have one consistent path-form interface.

static value_t method_root_info_regs(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_print_regs();
    return val_bool(true);
}

static value_t method_root_info_fpregs(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_print_fpregs();
    return val_bool(true);
}

static value_t method_root_info_mac(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_print_mac_state();
    return val_bool(true);
}

static value_t method_root_disasm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 16;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("disasm: count must be integer");
        if (count <= 0)
            count = 16;
    }
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return val_err("disasm: CPU not initialised");
    uint32_t addr = cpu_get_pc(cpu);
    char buf[160];
    for (int i = 0; i < (int)count; i++) {
        int instr_len = debugger_disasm(buf, sizeof(buf), addr);
        printf("%s\n", buf);
        addr += 2 * instr_len;
    }
    return val_bool(true);
}

// `break_set(target)` accepts a numeric address or a string the legacy
// address parser understands (`$0040A714`, `0x004007ba`, symbol names,
// expressions). For string args we stringify into a temporary buffer
// so parse_address sees a single token.
static value_t method_root_break_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("break_set: expected (address)");
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("break_set: debug not available");
    char target[64];
    if (argv[0].kind == V_STRING) {
        snprintf(target, sizeof(target), "%s", argv[0].s ? argv[0].s : "");
    } else {
        bool ok = false;
        uint64_t a = val_as_u64(&argv[0], &ok);
        if (!ok)
            return val_err("break_set: address must be integer or string");
        snprintf(target, sizeof(target), "0x%llx", (unsigned long long)a);
    }
    uint32_t addr;
    addr_space_t sp;
    if (!parse_address(target, &addr, &sp))
        return val_err("break_set: invalid address '%s'", target);
    breakpoint_t *bp = set_breakpoint(debug, addr, sp == ADDR_PHYSICAL ? ADDR_PHYSICAL : ADDR_LOGICAL);
    if (!bp)
        return val_err("break_set: failed to install breakpoint at $%08X", addr);
    printf("Breakpoint set at $%08X.\n", addr);
    return val_bool(true);
}

static value_t method_root_break_list_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("break_list_dump: debug not available");
    list_breakpoints(debug);
    return val_bool(true);
}

static value_t method_root_break_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("break_clear: debug not available");
    int count = delete_all_breakpoints(debug);
    printf("Deleted %d breakpoint(s).\n", count);
    return val_bool(true);
}

static value_t method_root_logpoint_list_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("logpoint_list_dump: debug not available");
    list_logpoints(debug);
    return val_bool(true);
}

static value_t method_root_logpoint_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("logpoint_clear: debug not available");
    delete_all_logpoints(debug);
    return val_bool(true);
}

// `logpoint_set(spec)` — pass the legacy logpoint spec as a single string
// (e.g. `--write 0x000016A.l "Ticks bumped..." level=5`). The legacy
// parser handles `--write` / `--read` / address / message / level.
static value_t method_root_logpoint_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("logpoint_set: expected (spec_string)");
    char line[2048];
    int n = snprintf(line, sizeof(line), "logpoint %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("logpoint_set: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `log_set(subsys, level_or_spec)` — adjust per-subsystem log level. The
// second arg accepts either an integer level or a full named-arg spec
// string (e.g. `"level=5 file=/tmp/foo.txt stdout=off ts=on"`); the spec
// form is forwarded verbatim to the legacy `log` parser.
static value_t method_root_log_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("log_set: expected (subsys, level|spec)");
    char line[512];
    int n;
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "log %s %s", argv[0].s, argv[1].s ? argv[1].s : "");
    } else {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok)
            return val_err("log_set: second arg must be integer level or spec string");
        n = snprintf(line, sizeof(line), "log %s %lld", argv[0].s, (long long)level);
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("log_set: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `stop()` — interrupt the scheduler.
static value_t method_root_stop(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("stop: scheduler not initialised");
    scheduler_stop(s);
    return val_bool(true);
}

// `step([n])` — single-step n instructions (default 1).
static value_t method_root_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 1;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("step: count must be integer");
    }
    if (count <= 0)
        return val_err("step: count must be positive");
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("step: scheduler not initialised");
    scheduler_run_instructions(s, (int)count);
    scheduler_stop(s);
    return val_bool(true);
}

// `background_checkpoint(name)` — capture a snapshot under the given label.
static value_t method_root_background_checkpoint(struct object *self, const member_t *m, int argc,
                                                 const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("background_checkpoint: expected (name)");
    char line[256];
    int n = snprintf(line, sizeof(line), "background-checkpoint %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("background_checkpoint: name too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `path_exists(path)` — true if the path exists in the shell VFS.
static value_t method_root_path_exists(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("path_exists: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "exists \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("path_exists: path too long");
    // legacy `exists` returns 0=exists, 1=missing — invert for V_BOOL.
    return val_bool(shell_dispatch(line) == 0);
}

// `path_size(path)` — file size in bytes (0 on stat failure).
static value_t method_root_path_size(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("path_size: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "size \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("path_size: path too long");
    uint64_t r = shell_dispatch(line);
    return val_uint(8, r);
}

// `fd_create(path, [size_str])` — create a blank floppy image. Optional
// size string accepts the legacy forms `"400K"` / `"800K"` / `"1.4MB"`;
// when omitted, the legacy default (1.4 MB) applies.
static value_t method_root_fd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_create: expected (path, [size])");
    char line[512];
    int n;
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s)
        n = snprintf(line, sizeof(line), "fd create \"%s\" %s", argv[0].s ? argv[0].s : "", argv[1].s);
    else
        n = snprintf(line, sizeof(line), "fd create \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("fd_create: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `print_value(target)` — read a register / condition code / memory cell
// via the legacy `print` command. Returns the numeric value as V_UINT;
// the test convention uses `>>> 0` to truncate to uint32.
static value_t method_root_print_value(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("print_value: expected (target)");
    char line[256];
    int n = snprintf(line, sizeof(line), "print %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("print_value: target too long");
    return val_uint(8, shell_dispatch(line));
}

// `set_value(target, value)` — write a register / condition code / memory
// cell via the legacy `set` command. Target syntax matches the legacy
// command (`d5`, `pc`, `z`, `0x1000.b`, etc.). Numeric values are
// stringified as hex; string values pass through verbatim.
static value_t method_root_set_value(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("set_value: expected (target, value)");
    char value_buf[64];
    const char *value_str = NULL;
    if (argv[1].kind == V_STRING) {
        value_str = argv[1].s ? argv[1].s : "";
    } else {
        bool ok = false;
        uint64_t v = val_as_u64(&argv[1], &ok);
        if (!ok)
            return val_err("set_value: value must be integer or string");
        snprintf(value_buf, sizeof(value_buf), "0x%llx", (unsigned long long)v);
        value_str = value_buf;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "set %s %s", argv[0].s ? argv[0].s : "", value_str);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("set_value: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `examine(addr, [count])` — hex-dump `count` bytes from `addr` (legacy
// `x` / `examine`). `addr` accepts integer or string (alias / expression).
static value_t method_root_examine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("examine: expected (addr, [count])");
    char line[128];
    int n;
    bool addr_ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &addr_ok);
    bool addr_is_str = (argv[0].kind == V_STRING);
    if (!addr_ok && !addr_is_str)
        return val_err("examine: addr must be integer or string");
    int64_t count = 0;
    bool have_count = false;
    if (argc >= 2) {
        bool ok = false;
        count = val_as_i64(&argv[1], &ok);
        if (!ok)
            return val_err("examine: count must be integer");
        have_count = true;
    }
    if (have_count) {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx %lld", (unsigned long long)addr_u, (long long)count);
        else
            n = snprintf(line, sizeof(line), "x %s %lld", argv[0].s ? argv[0].s : "", (long long)count);
    } else {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx", (unsigned long long)addr_u);
        else
            n = snprintf(line, sizeof(line), "x %s", argv[0].s ? argv[0].s : "");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("examine: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static value_t method_root_dump_tree(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    struct dump_buf b = {.buf = NULL, .cap = 0, .pos = 0, .overflow = false};
    dump_object_recursive(&b, object_root(), "");
    if (b.overflow || !b.buf) {
        free(b.buf);
        return val_err("dump_tree: out of memory");
    }
    value_t v = val_str(b.buf);
    free(b.buf);
    return v;
}

// `hd_models()` — return the known SCSI HD model catalog as a single
// JSON-encoded V_STRING (array of `{label, vendor, product, size}`).
// The web frontend's "create disk" dialog reads this list to populate
// its drive picker; returning the JSON inline retires the last
// `runCommandJSON("hd models --json")` caller in app/web/js.
static value_t method_root_hd_models(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    int count = drive_catalog_count();
    size_t cap = 64 + (size_t)count * 128;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return val_err("hd_models: out of memory");
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos, "[");
    for (int i = 0; i < count && pos + 256 < cap; i++) {
        const struct drive_model *md = drive_catalog_get(i);
        if (!md)
            continue;
        pos += (size_t)snprintf(buf + pos, cap - pos,
                                "%s{\"label\":\"%s\",\"vendor\":\"%s\",\"product\":\"%s\",\"size\":%zu}", i ? "," : "",
                                md->label, md->vendor, md->product, md->size);
    }
    if (pos + 1 < cap)
        pos += (size_t)snprintf(buf + pos, cap - pos, "]");
    value_t v = val_str(buf);
    free(buf);
    return v;
}

// `cdrom_attach(path)` — attach a CD-ROM image to the system.
static value_t method_root_cdrom_attach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("cdrom_attach: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "cdrom attach \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("cdrom_attach: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `hd_validate(path)` — true if the file passes legacy `hd validate`
// (cmd_bool 1 = valid).
static value_t method_root_hd_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("hd_validate: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "hd validate \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_validate: argument too long");
    return val_bool(shell_dispatch(line) == 1);
}

// `cdrom_validate(path)` — true if the file passes legacy `cdrom
// validate` (cmd_bool 1 = valid).
static value_t method_root_cdrom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("cdrom_validate: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "cdrom validate \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("cdrom_validate: argument too long");
    return val_bool(shell_dispatch(line) == 1);
}

// `cdrom_eject(id)` — eject the CD-ROM at the given SCSI id (default 3).
// `scsi.devices[id].eject()` already exists for per-device ejection;
// this wrapper preserves the legacy `cdrom eject [id]` shape so the
// integration scripts have a 1:1 mapping.
static value_t method_root_cdrom_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t id = 3;
    if (argc >= 1) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            id = (int64_t)argv[0].u;
    }
    char line[64];
    snprintf(line, sizeof(line), "cdrom eject %lld", (long long)id);
    return val_bool(shell_dispatch(line) == 0);
}

// `cdrom_info(id)` — print info about the CD-ROM at the given SCSI id
// (default 3). Returns true if a disc is present, false if the slot is
// empty / wrong type. The detail lines are printed via the legacy
// command's printf; scripts can also walk `scsi.devices[id].*` directly
// for structured access.
static value_t method_root_cdrom_info(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t id = 3;
    if (argc >= 1) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            id = (int64_t)argv[0].u;
    }
    char line[64];
    snprintf(line, sizeof(line), "cdrom info %lld", (long long)id);
    return val_bool(shell_dispatch(line) == 0);
}

// `image_mounts()` — list currently-mounted image paths (the legacy
// `image list` no-arg form). Returns a V_LIST<V_STRING>; scripts that
// expect the human-readable table output should use the legacy form.
static value_t method_root_image_mounts(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_bool(shell_dispatch("image list") == 0);
}

// `fd_validate(path)` — return the floppy density tag ("400K", "800K",
// "1.4MB", …) when the file is a recognised floppy image, or empty
// string otherwise. The legacy `fd validate` command prints the
// density to stdout and uses cmd_bool, so this wrapper opens the
// image directly via image_open_readonly to extract the type and
// avoid stdout-parsing.
static value_t method_root_fd_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_validate: expected (path)");
    image_t *img = image_open_readonly(argv[0].s ? argv[0].s : "");
    if (!img)
        return val_str("");
    const char *density = "";
    switch (img->type) {
    case image_fd_ss:
        density = "400K";
        break;
    case image_fd_ds:
        density = "800K";
        break;
    case image_fd_hd:
        density = "1.4MB";
        break;
    default:
        density = "";
        break;
    }
    image_close(img);
    return val_str(density);
}

// `setup_machine(model, ram_kb)` — run the legacy `setup --model X
// --ram Y` command that primes the next `rom load` for a specific
// machine profile. Renamed in the gsEval surface so the top-level
// namespace doesn't grow a generic `setup` verb.
static value_t method_root_setup_machine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("setup_machine: expected (model, [ram_kb])");
    bool ok = false;
    int64_t ram = (argc >= 2) ? (int64_t)val_as_i64(&argv[1], &ok) : 0;
    if (!ok && argc >= 2 && argv[1].kind == V_UINT)
        ram = (int64_t)argv[1].u;
    char line[256];
    int n;
    if (argc >= 2)
        n = snprintf(line, sizeof(line), "setup --model %s --ram %lld", argv[0].s ? argv[0].s : "", (long long)ram);
    else
        n = snprintf(line, sizeof(line), "setup --model %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("setup_machine: arguments too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `schedule(mode)` — set the scheduler mode (max / realtime / hardware).
static value_t method_root_schedule(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("schedule: expected (mode)");
    char line[64];
    int n = snprintf(line, sizeof(line), "schedule %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("schedule: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

// `download(path)` — trigger a browser file download via the legacy
// `download` command (WASM-only; the headless build has no equivalent
// platform plumbing).
static value_t method_root_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("download: expected (path)");
    char line[1024];
    int n = snprintf(line, sizeof(line), "download \"%s\"", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("download: argument too long");
    return val_bool(shell_dispatch(line) == 0);
}

static const arg_decl_t root_cp_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Source path"},
    {.name = "dst", .kind = V_STRING, .doc = "Destination path"},
    {.name = "flags",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional flags (e.g. -r for recursive copy)"},
};
static const arg_decl_t root_peeler_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive path"},
    {.name = "out_dir", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional extraction directory"},
};
static const arg_decl_t root_hd_create_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output path"                          },
    {.name = "size", .kind = V_STRING, .doc = "Image size (e.g. \"40M\" or \"512K\")"},
};
static const arg_decl_t root_hd_download_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Source HD image path (base + delta)"},
    {.name = "dst", .kind = V_STRING, .doc = "Destination flat-file path"         },
};
static const arg_decl_t root_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "File path"},
};
static const arg_decl_t root_path_arg_optional[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "File path; empty falls back to the currently-loaded ROM"},
};
static const arg_decl_t root_hd_attach_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "HD image path"},
    {.name = "id", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "SCSI bus index 0-6 (default 0)"},
};
static const arg_decl_t root_cdrom_id_arg[] = {
    {.name = "id", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "SCSI id 0-6 (default 3)"},
};
static const arg_decl_t root_setup_machine_args[] = {
    {.name = "model", .kind = V_STRING, .doc = "Machine model id (plus / se30 / iicx)"},
    {.name = "ram_kb", .kind = V_UINT, .flags = OBJ_ARG_OPTIONAL, .doc = "RAM size in KB"},
};
static const arg_decl_t root_schedule_args[] = {
    {.name = "mode", .kind = V_STRING, .doc = "Scheduler mode (max | realtime | hardware)"},
};
static const arg_decl_t root_find_media_args[] = {
    {.name = "dir", .kind = V_STRING, .doc = "Directory to scan"},
    {.name = "dst", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional path to copy match into"},
};
static const arg_decl_t root_checkpoint_load_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Checkpoint path; empty auto-loads the latest"},
};
static const arg_decl_t root_checkpoint_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Checkpoint output path"},
    {.name = "mode", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional 'content' or 'refs' mode"},
};
static const arg_decl_t root_register_machine_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};
static const arg_decl_t root_fd_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Floppy image path"},
    {.name = "slot", .kind = V_INT, .doc = "Drive index (0 = upper, 1 = lower)"},
    {.name = "writable", .kind = V_BOOL, .flags = OBJ_ARG_OPTIONAL, .doc = "Mount writable (default false)"},
};
static const arg_decl_t root_run_args[] = {
    {.name = "cycles", .kind = V_UINT, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional cycle budget; 0 = run until paused"},
};

static const arg_decl_t root_path_args[] = {
    {.name = "path", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Object path; empty resolves to the root"},
};
static const arg_decl_t root_help_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Path to a member or object; empty resolves to the root"},
};
static const arg_decl_t root_print_args[] = {
    {.name = "value", .kind = V_NONE, .doc = "Value to format"},
};

static const member_t emu_root_members[] = {
    {.kind = M_METHOD,
     .name = "objects",
     .doc = "List child object names at the given path (or root)",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_objects}                     },
    {.kind = M_METHOD,
     .name = "attributes",
     .doc = "List attribute names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_attributes}                  },
    {.kind = M_METHOD,
     .name = "methods",
     .doc = "List method names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_methods}                     },
    {.kind = M_METHOD,
     .name = "help",
     .doc = "Return the doc string of a resolved member (or class name)",
     .method = {.args = root_help_args, .nargs = 1, .result = V_STRING, .fn = method_root_help}                      },
    {.kind = M_METHOD,
     .name = "time",
     .doc = "Wall-clock seconds since the Unix epoch",
     .method = {.args = NULL, .nargs = 0, .result = V_UINT, .fn = method_root_time}                                  },
    {.kind = M_METHOD,
     .name = "print",
     .doc = "Format a value as a string for display",
     .method = {.args = root_print_args, .nargs = 1, .result = V_STRING, .fn = method_root_print}                    },
    // Legacy-command wrappers (M8 slice 4 — proposal §5.10).
    // Side-effect wrappers return V_BOOL (true on dispatch success) so
    // the M10b migrators can branch on the result without re-deriving
    // the legacy command's int / cmd_bool convention.
    {.kind = M_METHOD,
     .name = "cp",
     .doc = "Copy a file or directory (alias for storage.import / legacy `cp`)",
     .method = {.args = root_cp_args, .nargs = 3, .result = V_BOOL, .fn = method_root_cp}                            },
    {.kind = M_METHOD,
     .name = "peeler",
     .doc = "Extract a Mac archive (.sit/.cpt/.hqx/.bin)",
     .method = {.args = root_peeler_args, .nargs = 2, .result = V_BOOL, .fn = method_root_peeler}                    },
    {.kind = M_METHOD,
     .name = "peeler_probe",
     .doc = "True if a file is a peeler-supported archive",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_peeler_probe}                 },
    {.kind = M_METHOD,
     .name = "hd_create",
     .doc = "Create a blank SCSI hard-disk image",
     .method = {.args = root_hd_create_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_create}              },
    {.kind = M_METHOD,
     .name = "hd_download",
     .doc = "Export a hard-disk image (base + delta) to a flat file",
     .method = {.args = root_hd_download_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_download}          },
    {.kind = M_METHOD,
     .name = "rom_probe",
     .doc = "True if a file is a recognised ROM image (no arg = is a ROM loaded?)",
     .method = {.args = root_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_probe}           },
    {.kind = M_METHOD,
     .name = "rom_validate",
     .doc = "Verify a ROM file's checksum and recognised model",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_validate}                 },
    {.kind = M_METHOD,
     .name = "vrom_probe",
     .doc = "True if a file is a recognised video-ROM image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_probe}                   },
    {.kind = M_METHOD,
     .name = "vrom_validate",
     .doc = "Verify a video-ROM file's signature",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_validate}                },
    {.kind = M_METHOD,
     .name = "fd_probe",
     .doc = "True if a file is a recognised floppy-disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_fd_probe}                     },
    {.kind = M_METHOD,
     .name = "find_media",
     .doc = "Search a directory for a floppy image; copy to dst if given",
     .method = {.args = root_find_media_args, .nargs = 2, .result = V_BOOL, .fn = method_root_find_media}            },
    {.kind = M_METHOD,
     .name = "partmap",
     .doc = "Parse and print the Apple Partition Map of a disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_partmap}                      },
    {.kind = M_METHOD,
     .name = "probe",
     .doc = "Probe an image for its format (HFS / ISO / APM / ...)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_probe}                        },
    {.kind = M_METHOD,
     .name = "list_partitions",
     .doc = "List partitions cached for a mounted image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_list_partitions}              },
    {.kind = M_METHOD,
     .name = "unmount",
     .doc = "Force-close a cached auto-mount of an image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_unmount}                      },
    {.kind = M_METHOD,
     .name = "quit",
     .doc = "Exit the emulator (asks the legacy quit command to end the run)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_root_quit}                                  },
    // Checkpoint / runtime-state wrappers (M10b — checkpoint area).
    {.kind = M_METHOD,
     .name = "checkpoint_probe",
     .doc = "True if a valid checkpoint exists for the active machine",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_checkpoint_probe}                      },
    {.kind = M_METHOD,
     .name = "checkpoint_clear",
     .doc = "Remove all checkpoint files for the active machine",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_checkpoint_clear}                      },
    {.kind = M_METHOD,
     .name = "checkpoint_load",
     .doc = "Load a checkpoint (auto-loads latest when path is omitted)",
     .method = {.args = root_checkpoint_load_args, .nargs = 1, .result = V_BOOL, .fn = method_root_checkpoint_load}  },
    {.kind = M_METHOD,
     .name = "checkpoint_save",
     .doc = "Save the current machine state to a checkpoint file",
     .method = {.args = root_checkpoint_save_args, .nargs = 2, .result = V_BOOL, .fn = method_root_checkpoint_save}  },
    {.kind = M_METHOD,
     .name = "register_machine",
     .doc = "Register the active machine identity (must precede any image open)",
     .method = {.args = root_register_machine_args, .nargs = 2, .result = V_BOOL, .fn = method_root_register_machine}},
    {.kind = M_METHOD,
     .name = "running",
     .doc = "True if the scheduler is currently running",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_running}                               },
    {.kind = M_ATTR,
     .name = "auto_checkpoint",
     .doc = "Enable/disable the background auto-checkpoint loop (WASM-only)",
     .attr = {.type = V_BOOL, .get = attr_auto_checkpoint_get, .set = attr_auto_checkpoint_set}                      },
    // Drag-drop boot helpers (M10b — drop area).
    {.kind = M_METHOD,
     .name = "rom_checksum",
     .doc = "Return the 8-char hex checksum of a ROM file (empty on invalid)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_STRING, .fn = method_root_rom_checksum}               },
    {.kind = M_METHOD,
     .name = "rom_load",
     .doc = "Load a ROM file and create the matching machine",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_load}                     },
    {.kind = M_METHOD,
     .name = "fd_insert",
     .doc = "Insert a floppy image into a drive slot",
     .method = {.args = root_fd_insert_args, .nargs = 3, .result = V_BOOL, .fn = method_root_fd_insert}              },
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Start the scheduler (optionally for a cycle budget)",
     .method = {.args = root_run_args, .nargs = 1, .result = V_BOOL, .fn = method_root_run}                          },
    // url-media + config-dialog + ui boot helpers (M10b — finish line).
    {.kind = M_METHOD,
     .name = "vrom_load",
     .doc = "Set the video-ROM path for the next machine init",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_load}                    },
    {.kind = M_METHOD,
     .name = "hd_attach",
     .doc = "Attach a hard-disk image at the given SCSI id",
     .method = {.args = root_hd_attach_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_attach}              },
    {.kind = M_METHOD,
     .name = "hd_validate",
     .doc = "True if the file is a recognised hard-disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_hd_validate}                  },
    {.kind = M_METHOD,
     .name = "cdrom_validate",
     .doc = "True if the file is a recognised CD-ROM image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_validate}               },
    {.kind = M_METHOD,
     .name = "cdrom_eject",
     .doc = "Eject the CD-ROM at the given SCSI id (default 3)",
     .method = {.args = root_cdrom_id_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_eject}              },
    {.kind = M_METHOD,
     .name = "cdrom_info",
     .doc = "Print info for the CD-ROM at the given SCSI id (default 3)",
     .method = {.args = root_cdrom_id_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_info}               },
    {.kind = M_METHOD,
     .name = "image_mounts",
     .doc = "List currently-mounted image paths (table format)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_image_mounts}                          },
    {.kind = M_METHOD,
     .name = "fd_validate",
     .doc = "Floppy density tag (\"400K\" / \"800K\" / \"1.4MB\") or empty if unrecognised",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_STRING, .fn = method_root_fd_validate}                },
    {.kind = M_METHOD,
     .name = "setup_machine",
     .doc = "Prime the next `rom_load` for a specific machine model",
     .method = {.args = root_setup_machine_args, .nargs = 2, .result = V_BOOL, .fn = method_root_setup_machine}      },
    {.kind = M_METHOD,
     .name = "schedule",
     .doc = "Set the scheduler mode (max / realtime / hardware)",
     .method = {.args = root_schedule_args, .nargs = 1, .result = V_BOOL, .fn = method_root_schedule}                },
    {.kind = M_METHOD,
     .name = "download",
     .doc = "Trigger a browser file download (WASM-only)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_download}                     },
    {.kind = M_METHOD,
     .name = "cdrom_attach",
     .doc = "Attach a CD-ROM image to the SCSI bus",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_attach}                 },
    {.kind = M_METHOD,
     .name = "hd_models",
     .doc = "Return the known SCSI HD model catalog as a JSON array string",
     .method = {.args = NULL, .nargs = 0, .result = V_STRING, .fn = method_root_hd_models}                           },
    {.kind = M_METHOD,
     .name = "dump_tree",
     .doc = "Walk the live object tree and return one JSON description of every node",
     .method = {.args = NULL, .nargs = 0, .result = V_STRING, .fn = method_root_dump_tree}                           },

    // Debugging-area thin wrappers (Phase 3 final batch).
    {.kind = M_METHOD,
     .name = "info_regs",
     .doc = "Print the integer register file (legacy `info regs`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_info_regs}                             },
    {.kind = M_METHOD,
     .name = "info_fpregs",
     .doc = "Print the floating-point register file (legacy `info fpregs`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_info_fpregs}                           },
    {.kind = M_METHOD,
     .name = "info_mac",
     .doc = "Print the Mac-OS state snapshot (legacy `info mac`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_info_mac}                              },
    {.kind = M_METHOD,
     .name = "disasm",
     .doc = "Disassemble forward from PC (legacy `d [count]`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_disasm}                                },
    {.kind = M_METHOD,
     .name = "break_set",
     .doc = "Set a breakpoint at the given address (legacy `break set X`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_break_set}                             },
    {.kind = M_METHOD,
     .name = "break_list_dump",
     .doc = "Print the breakpoint table (legacy `break list`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_break_list_dump}                       },
    {.kind = M_METHOD,
     .name = "break_clear",
     .doc = "Clear all breakpoints (legacy `break clear`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_break_clear}                           },
    {.kind = M_METHOD,
     .name = "logpoint_set",
     .doc = "Install a logpoint from a spec string (legacy `logpoint <spec>`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_logpoint_set}                          },
    {.kind = M_METHOD,
     .name = "logpoint_list_dump",
     .doc = "Print the logpoint table (legacy `logpoint list`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_logpoint_list_dump}                    },
    {.kind = M_METHOD,
     .name = "logpoint_clear",
     .doc = "Clear all logpoints (legacy `logpoint clear`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_logpoint_clear}                        },
    {.kind = M_METHOD,
     .name = "log_set",
     .doc = "Set per-subsystem log level or full spec (legacy `log <subsys> <level|spec>`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_log_set}                               },
    {.kind = M_METHOD,
     .name = "stop",
     .doc = "Interrupt the scheduler (legacy `stop`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_stop}                                  },
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Single-step N instructions; default 1 (legacy `step`/`s`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_step}                                  },
    {.kind = M_METHOD,
     .name = "background_checkpoint",
     .doc = "Capture a checkpoint under the given label (legacy `background-checkpoint`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_background_checkpoint}                 },
    {.kind = M_METHOD,
     .name = "path_exists",
     .doc = "True if the path exists in the shell VFS (legacy `exists`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_path_exists}                           },
    {.kind = M_METHOD,
     .name = "path_size",
     .doc = "File size in bytes (legacy `size`)",
     .method = {.args = NULL, .nargs = 1, .result = V_UINT, .fn = method_root_path_size}                             },
    {.kind = M_METHOD,
     .name = "fd_create",
     .doc = "Create a blank floppy image (legacy `fd create`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_fd_create}                             },
    {.kind = M_METHOD,
     .name = "print_value",
     .doc = "Read a register / flag / memory cell (legacy `print <target>`)",
     .method = {.args = NULL, .nargs = 1, .result = V_UINT, .fn = method_root_print_value}                           },
    {.kind = M_METHOD,
     .name = "set_value",
     .doc = "Write a register / flag / memory cell (legacy `set <target> <value>`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_set_value}                             },
    {.kind = M_METHOD,
     .name = "examine",
     .doc = "Hex-dump memory (legacy `x` / `examine`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_examine}                               },
};

static const class_desc_t emu_root_class_real = {
    .name = "emu",
    .members = emu_root_members,
    .n_members = sizeof(emu_root_members) / sizeof(emu_root_members[0]),
};

// === Built-in alias registration ============================================
//
// Register at install time. Order matters only insofar as built-ins
// must be registered before any user can issue shell.alias.add — at
// startup that's guaranteed because user input arrives later.

static void register_builtin(const char *name, const char *path) {
    char err[160];
    if (alias_register_builtin(name, path, err, sizeof(err)) < 0)
        fprintf(stderr, "gs_classes: built-in alias '$%s' → '%s' rejected: %s\n", name, path, err);
}

static void register_cpu_aliases(void) {
    register_builtin("pc", "cpu.pc");
    register_builtin("sr", "cpu.sr");
    register_builtin("ccr", "cpu.ccr");
    register_builtin("ssp", "cpu.ssp");
    register_builtin("usp", "cpu.usp");
    register_builtin("msp", "cpu.msp");
    register_builtin("vbr", "cpu.vbr");
    register_builtin("sp", "cpu.sp");
    static const char *const dnames[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char *const anames[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    for (int i = 0; i < 8; i++) {
        char path[16];
        snprintf(path, sizeof(path), "cpu.%s", dnames[i]);
        register_builtin(dnames[i], path);
        snprintf(path, sizeof(path), "cpu.%s", anames[i]);
        register_builtin(anames[i], path);
    }
}

static void register_fpu_aliases(void) {
    register_builtin("fpcr", "cpu.fpu.fpcr");
    register_builtin("fpsr", "cpu.fpu.fpsr");
    register_builtin("fpiar", "cpu.fpu.fpiar");
    static const char *const fpnames[] = {"fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7"};
    for (int i = 0; i < 8; i++) {
        char path[24];
        snprintf(path, sizeof(path), "cpu.fpu.%s", fpnames[i]);
        register_builtin(fpnames[i], path);
    }
}

static void register_mac_aliases(void) {
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        if (!mac_global_vars[i].name)
            continue;
        char path[128];
        snprintf(path, sizeof(path), "mac.%s", mac_global_vars[i].name);
        char err[160];
        if (alias_register_builtin(mac_global_vars[i].name, path, err, sizeof(err)) < 0) {
            // Skip silently on collision: a later milestone may rename.
            // We log once per startup for diagnostics.
            fprintf(stderr, "gs_classes: skipping mac alias '$%s': %s\n", mac_global_vars[i].name, err);
        }
    }
}

// === Install / uninstall ====================================================
//
// Lifecycle invariant: stubs are tied to a specific `cfg` pointer. Two
// patterns both have to work:
//
//   1. Cold boot
//        system_create(new) -> gs_classes_install(new)
//        ... later ...
//        system_destroy(new) -> gs_classes_uninstall_if(new)
//
//   2. checkpoint --load (cmd_load_checkpoint)
//        new = system_restore(...);     // system_create(new) -> install(new)
//        global_emulator = new;
//        system_destroy(old)            // uninstall_if(old) -- no-op
//
// The danger is pattern 2 with a naive idempotent install: if install
// short-circuits when stubs already exist, the install for `new` is a
// no-op (old's stubs are still attached), and the subsequent
// destroy(old) wipes everything — leaving the object root empty and
// every gsEval('cpu.pc' / 'running' / …) returning
// `{"error":"path '...' did not resolve"}`.
//
// The fix: track which cfg the stubs were installed for. Install is
// idempotent only for the *same* cfg and otherwise uninstalls before
// reinstalling. `gs_classes_uninstall_if(cfg)` (called from
// system_destroy) only tears down when the stubs are still associated
// with `cfg` — so destroying the old config after a load is a no-op.

#define MAX_STUBS 40
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;
static struct config *g_installed_cfg = NULL;

static struct object *attach_stub(struct object *parent, const class_desc_t *cls, void *data, const char *name) {
    if (g_stub_count >= MAX_STUBS)
        return NULL;
    char err[200];
    if (!object_validate_class(cls, err, sizeof(err))) {
        fprintf(stderr, "gs_classes: class '%s' invalid: %s\n", cls->name ? cls->name : "?", err);
        return NULL;
    }
    struct object *o = object_new(cls, data, name);
    if (!o)
        return NULL;
    object_attach(parent ? parent : object_root(), o);
    g_stubs[g_stub_count++] = o;
    return o;
}

void gs_classes_install_root(void) {
    // Registers the top-level method table on the object root. Safe to
    // call repeatedly — object_root_set_class is idempotent for the
    // same class pointer.
    object_root_set_class(&emu_root_class_real);
}

void gs_classes_install(struct config *cfg) {
    // Idempotent for the SAME cfg — second-call from a redundant init
    // path keeps the existing stubs.
    if (g_stub_count > 0 && g_installed_cfg == cfg)
        return;
    // Different cfg (typically: checkpoint --load just produced a new
    // config). Tear down the old stubs before attaching new ones, so
    // child objects don't dangle pointers into freed config state and
    // the eventual `system_destroy(old_cfg)` call below doesn't end up
    // wiping the freshly installed root.
    if (g_stub_count > 0)
        gs_classes_uninstall();
    g_installed_cfg = cfg;

    // Top-level methods. Already installed by shell_init via
    // gs_classes_install_root; the call is repeated here so paths that
    // skip shell_init still get the methods.
    gs_classes_install_root();

    struct object *cpu_obj = attach_stub(NULL, &cpu_class, cfg, "cpu");
    struct object *mem_obj = attach_stub(NULL, &memory_class, cfg, "memory");
    /* scheduler */ attach_stub(NULL, &scheduler_class_desc, cfg, "scheduler");
    /* machine */ attach_stub(NULL, &machine_class, cfg, "machine");
    struct object *shell_obj = attach_stub(NULL, &shell_class_desc, cfg, "shell");
    // M8 (slice 2) — `storage` graduates from M2 stub to real class.
    // The collection of pre-allocated image entry objects below is
    // attached on demand via the indexed-child get() callback; only
    // the storage object itself and its `images` child go in g_stubs.
    struct object *storage_obj = attach_stub(NULL, &storage_class_real, cfg, "storage");
    if (storage_obj) {
        attach_stub(storage_obj, &storage_images_collection_class, cfg, "images");
        for (int i = 0; i < MAX_IMAGES; i++) {
            g_storage_image_data[i].cfg = cfg;
            g_storage_image_data[i].slot = i;
            g_storage_image_objs[i] = object_new(&storage_image_class, &g_storage_image_data[i], NULL);
        }
    }
    /* math    */ attach_stub(NULL, &math_class_desc, cfg, "math");
    /* lp      */ attach_stub(NULL, &lp_class_desc, cfg, "lp");

    // memory.peek child object — methods b/w/l for sized reads.
    if (mem_obj)
        attach_stub(mem_obj, &mem_peek_class, cfg, "peek");

    // mac is always attached — readers tolerate uninitialised RAM.
    if (build_mac_class() == 0)
        attach_stub(NULL, &g_mac_class, cfg, "mac");

    // shell.alias child object.
    if (shell_obj)
        attach_stub(shell_obj, &shell_alias_class, cfg, "alias");

    // cpu.fpu child object — only when the active CPU model has an FPU.
    if (cpu_obj && cfg && cfg->cpu && cfg->cpu->fpu)
        attach_stub(cpu_obj, &fpu_class, cfg, "fpu");

    // M6 — debugger object with breakpoints/logpoints/watches collections.
    struct object *debugger_obj = attach_stub(NULL, &debugger_class, cfg, "debugger");
    if (debugger_obj) {
        attach_stub(debugger_obj, &bp_collection_class, cfg, "breakpoints");
        attach_stub(debugger_obj, &lp_collection_class, cfg, "logpoints");
        attach_stub(debugger_obj, &watches_collection_class, cfg, "watches");
    }

    // M7a — scc with channel children. Plus has no SCC-less profile;
    // SE/30 / IIcx ditto. Attach unconditionally — getters tolerate
    // a NULL scc_t* via the early-return check in each accessor.
    if (cfg && cfg->scc) {
        struct object *scc_obj = attach_stub(NULL, &scc_class, cfg, "scc");
        if (scc_obj) {
            attach_stub(scc_obj, &scc_channel_a_class, cfg, "a");
            attach_stub(scc_obj, &scc_channel_b_class, cfg, "b");
        }
    }

    // M7b — rtc.
    if (cfg && cfg->rtc)
        attach_stub(NULL, &rtc_class, cfg, "rtc");

    // M7c — via1 (always present) and via2 (SE/30, IIcx). Each VIA's
    // port_a / port_b are attached as named children of the VIA object.
    if (cfg && cfg->via1) {
        struct object *via1_obj = attach_stub(NULL, &via1_class, cfg, "via1");
        if (via1_obj) {
            attach_stub(via1_obj, &via1_port_a_class, cfg, "port_a");
            attach_stub(via1_obj, &via1_port_b_class, cfg, "port_b");
        }
    }
    if (cfg && cfg->via2) {
        struct object *via2_obj = attach_stub(NULL, &via2_class, cfg, "via2");
        if (via2_obj) {
            attach_stub(via2_obj, &via2_port_a_class, cfg, "port_a");
            attach_stub(via2_obj, &via2_port_b_class, cfg, "port_b");
        }
    }

    // M7d — scsi controller with bus child and indexed device children.
    // The 8 device entry objects are heap-allocated here and freed in
    // gs_classes_uninstall. Indexed-child callbacks consult cfg->scsi
    // each time so the empty-slot semantics are honored even if the
    // controller's device table changes (e.g. cdrom medium ejected).
    if (cfg && cfg->scsi) {
        struct object *scsi_obj = attach_stub(NULL, &scsi_class, cfg, "scsi");
        if (scsi_obj) {
            attach_stub(scsi_obj, &scsi_bus_class, cfg, "bus");
            attach_stub(scsi_obj, &scsi_devices_collection_class, cfg, "devices");
            for (int i = 0; i < 8; i++) {
                g_scsi_dev_data[i].cfg = cfg;
                g_scsi_dev_data[i].slot = i;
                g_scsi_dev_objs[i] = object_new(&scsi_device_class, &g_scsi_dev_data[i], NULL);
            }
        }
    }

    // M7e — floppy controller with two drive entries (internal/external).
    // Same per-entry-object pattern as scsi; the drive count is fixed
    // hardware so the collection always reports 2.
    if (cfg && cfg->floppy) {
        struct object *floppy_obj = attach_stub(NULL, &floppy_class, cfg, "floppy");
        if (floppy_obj) {
            attach_stub(floppy_obj, &floppy_drives_collection_class, cfg, "drives");
            for (int i = 0; i < 2; i++) {
                g_floppy_drive_data[i].cfg = cfg;
                g_floppy_drive_data[i].slot = i;
                g_floppy_drive_objs[i] = object_new(&floppy_drive_class, &g_floppy_drive_data[i], NULL);
            }
        }
    }

    // M7f — sound (Plus PWM only; SE/30 / IIcx use ASC and leave
    // cfg->sound NULL).
    if (cfg && cfg->sound)
        attach_stub(NULL, &sound_class, cfg, "sound");

    // M8 — network.appletalk + input.mouse. AppleTalk is installed
    // unconditionally by appletalk_init() at machine boot, so the
    // tree is always reachable. Share entry objects are pre-allocated
    // here and freed at uninstall (same per-slot pattern as scsi /
    // floppy entry objects).
    {
        struct object *network_obj = attach_stub(NULL, &network_class, cfg, "network");
        if (network_obj) {
            struct object *atalk_obj = attach_stub(network_obj, &atalk_class, cfg, "appletalk");
            if (atalk_obj) {
                attach_stub(atalk_obj, &atalk_shares_collection_class, cfg, "shares");
                attach_stub(atalk_obj, &atalk_printer_class, cfg, "printer");
                int max = atalk_share_max();
                if (max > 8)
                    max = 8;
                for (int i = 0; i < max; i++) {
                    g_atalk_share_data[i].slot = i;
                    g_atalk_share_objs[i] = object_new(&atalk_share_class, &g_atalk_share_data[i], NULL);
                }
            }
        }
    }
    attach_stub(NULL, &mouse_class, cfg, "mouse");
    attach_stub(NULL, &keyboard_class, cfg, "keyboard");
    attach_stub(NULL, &screen_class, cfg, "screen");
    attach_stub(NULL, &vfs_class, cfg, "vfs");
    attach_stub(NULL, &find_class, cfg, "find");

    // Built-in aliases. Register CPU always, FPU only when present,
    // mac always (the table is size-driven and machine-independent).
    register_cpu_aliases();
    if (cfg && cfg->cpu && cfg->cpu->fpu)
        register_fpu_aliases();
    register_mac_aliases();
}

void gs_classes_uninstall(void) {
    for (int i = g_stub_count - 1; i >= 0; i--) {
        struct object *o = g_stubs[i];
        if (o) {
            object_detach(o);
            object_delete(o);
        }
        g_stubs[i] = NULL;
    }
    g_stub_count = 0;
    // M7d — release scsi device entry objects. They were never attached
    // (the indexed-child get() returns them on demand), so they don't
    // appear in g_stubs and need their own teardown.
    for (int i = 0; i < 8; i++) {
        if (g_scsi_dev_objs[i]) {
            object_delete(g_scsi_dev_objs[i]);
            g_scsi_dev_objs[i] = NULL;
        }
        g_scsi_dev_data[i].cfg = NULL;
        g_scsi_dev_data[i].slot = 0;
    }
    // M7e — same for floppy drive entry objects.
    for (int i = 0; i < 2; i++) {
        if (g_floppy_drive_objs[i]) {
            object_delete(g_floppy_drive_objs[i]);
            g_floppy_drive_objs[i] = NULL;
        }
        g_floppy_drive_data[i].cfg = NULL;
        g_floppy_drive_data[i].slot = 0;
    }
    // M8 — appletalk share entry objects.
    for (int i = 0; i < 8; i++) {
        if (g_atalk_share_objs[i]) {
            object_delete(g_atalk_share_objs[i]);
            g_atalk_share_objs[i] = NULL;
        }
        g_atalk_share_data[i].slot = 0;
    }
    // M8 (slice 2) — storage image entry objects.
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_storage_image_objs[i]) {
            object_delete(g_storage_image_objs[i]);
            g_storage_image_objs[i] = NULL;
        }
        g_storage_image_data[i].cfg = NULL;
        g_storage_image_data[i].slot = 0;
    }
    // M8 (slice 3) — restore the namespace-only root class so a fresh
    // object_root() call after uninstall doesn't surface stale members.
    object_root_set_class(NULL);
    alias_reset();
    free_mac_class();
    g_installed_cfg = NULL;
}

// Conditional uninstall used by `system_destroy(cfg)`: only tears down
// when the stubs are still associated with `cfg`. After
// `checkpoint --load`, the order is `system_create(new)` → install(new)
// → `system_destroy(old)`; without this gate the destroy would wipe the
// freshly installed `new` root.
void gs_classes_uninstall_if(struct config *cfg) {
    if (g_installed_cfg == cfg)
        gs_classes_uninstall();
}
