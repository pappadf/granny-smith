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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alias.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "fpu.h"
#include "machine.h"
#include "memory.h"
#include "object.h"
#include "scc.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

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

#define MAX_STUBS 24
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;

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

void gs_classes_install(struct config *cfg) {
    if (g_stub_count > 0)
        return; // idempotent

    struct object *cpu_obj = attach_stub(NULL, &cpu_class, cfg, "cpu");
    struct object *mem_obj = attach_stub(NULL, &memory_class, cfg, "memory");
    /* scheduler */ attach_stub(NULL, &scheduler_class_desc, cfg, "scheduler");
    /* machine */ attach_stub(NULL, &machine_class, cfg, "machine");
    struct object *shell_obj = attach_stub(NULL, &shell_class_desc, cfg, "shell");
    /* storage */ attach_stub(NULL, &storage_class_desc, cfg, "storage");
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
    alias_reset();
    free_mac_class();
}
