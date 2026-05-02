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
#include <time.h>

#include "alias.h"
#include "appletalk.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "debug_mac.h"
#include "floppy.h"
#include "fpu.h"
#include "machine.h"
#include "memory.h"
#include "object.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "sound.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
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
    bool ok = true;
    uint64_t s = val_as_u64(&in, &ok);
    value_free(&in);
    if (!ok)
        return val_err("rtc.time: value is not numeric");
    rtc_set_seconds(rtc, (uint32_t)s);
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

// --- input.mouse ----------------------------------------------------------

static value_t input_mouse_method_move(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("input.mouse.move: expected (x, y)");
    bool okx = true, oky = true;
    int64_t x = val_as_i64(&argv[0], &okx);
    int64_t y = val_as_i64(&argv[1], &oky);
    if (!okx || !oky)
        return val_err("input.mouse.move: x and y must be integers");
    debug_mac_set_mouse((long)x, (long)y);
    return val_none();
}

static value_t input_mouse_method_click(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool down = (argc >= 1) ? val_as_bool(&argv[0]) : true;
    // Routes through the per-platform mouse path (ADB or quadrature) —
    // same as legacy `mouse-button down|up` without --global.
    extern void system_mouse_update(bool button, int dx, int dy);
    system_mouse_update(down, 0, 0);
    return val_none();
}

static value_t input_mouse_method_trace(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("input.mouse.trace: expected (enabled)");
    debug_mac_set_trace_mouse(val_as_bool(&argv[0]));
    return val_none();
}

static const arg_decl_t input_mouse_move_args[] = {
    {.name = "x", .kind = V_INT, .doc = "Target X coordinate"},
    {.name = "y", .kind = V_INT, .doc = "Target Y coordinate"},
};
static const arg_decl_t input_mouse_click_args[] = {
    {.name = "down", .kind = V_BOOL, .flags = OBJ_ARG_OPTIONAL, .doc = "true = press, false = release (default true)"},
};
static const arg_decl_t input_mouse_trace_args[] = {
    {.name = "enabled", .kind = V_BOOL, .doc = "true = log mouse position once per second"},
};

static const member_t input_mouse_members[] = {
    {.kind = M_METHOD,
     .name = "move",
     .doc = "Set mouse position (per-platform default route)",
     .method = {.args = input_mouse_move_args, .nargs = 2, .result = V_NONE, .fn = input_mouse_method_move}  },
    {.kind = M_METHOD,
     .name = "click",
     .doc = "Press or release the mouse button via the hardware path",
     .method = {.args = input_mouse_click_args, .nargs = 1, .result = V_NONE, .fn = input_mouse_method_click}},
    {.kind = M_METHOD,
     .name = "trace",
     .doc = "Toggle the 1 Hz mouse-position trace logger",
     .method = {.args = input_mouse_trace_args, .nargs = 1, .result = V_NONE, .fn = input_mouse_method_trace}},
};

static const class_desc_t input_mouse_class = {
    .name = "mouse",
    .members = input_mouse_members,
    .n_members = sizeof(input_mouse_members) / sizeof(input_mouse_members[0]),
};

static const class_desc_t input_class = {
    .name = "input",
    .members = NULL,
    .n_members = 0,
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

static value_t storage_method_import(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_err("storage.import(): deferred — image-persist plumbing lands in a "
                   "later M8 sub-commit");
}

static const arg_decl_t storage_import_args[] = {
    {.name = "host_path", .kind = V_STRING, .doc = "Host path to read"           },
    {.name = "dst_path",  .kind = V_STRING, .doc = "Destination path under MEMFS"},
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

static const member_t storage_members[] = {
    {.kind = M_METHOD,
     .name = "import",
     .doc = "Persist a host file under /images/ (deferred — see proposal §5.7)",
     .method = {.args = storage_import_args, .nargs = 2, .result = V_STRING, .fn = storage_method_import}},
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
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_objects}   },
    {.kind = M_METHOD,
     .name = "attributes",
     .doc = "List attribute names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_attributes}},
    {.kind = M_METHOD,
     .name = "methods",
     .doc = "List method names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_methods}   },
    {.kind = M_METHOD,
     .name = "help",
     .doc = "Return the doc string of a resolved member (or class name)",
     .method = {.args = root_help_args, .nargs = 1, .result = V_STRING, .fn = method_root_help}    },
    {.kind = M_METHOD,
     .name = "time",
     .doc = "Wall-clock seconds since the Unix epoch",
     .method = {.args = NULL, .nargs = 0, .result = V_UINT, .fn = method_root_time}                },
    {.kind = M_METHOD,
     .name = "print",
     .doc = "Format a value as a string for display",
     .method = {.args = root_print_args, .nargs = 1, .result = V_STRING, .fn = method_root_print}  },
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

#define MAX_STUBS 40
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

    // M8 (slice 3) — register top-level methods on the root.
    object_root_set_class(&emu_root_class_real);

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
    {
        struct object *input_obj = attach_stub(NULL, &input_class, cfg, "input");
        if (input_obj)
            attach_stub(input_obj, &input_mouse_class, cfg, "mouse");
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
}
