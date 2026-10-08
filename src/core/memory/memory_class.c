// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory_class.c
// Object-model glue for the memory map: the machine.memory node (sizes,
// slow-path diagnostics, read_cstring / dump / translate) and its peek and
// poke children.  Kept out of memory.c so the page table and its slow paths
// sit in a TU of their own; everything here goes through the public memory
// API and the fast-path aliases.

#include "memory_class.h"

#include "addr_format.h"
#include "cpu.h"
#include "debug.h"
#include "gs_out.h"
#include "lisa_mmu.h"
#include "memory.h"
#include "mmu.h"
#include "object.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// instance_data on the memory node is the memory_map_t* itself.
// Lifetime is tied to memory_map_init / memory_map_delete.

static DEF_GETTER(attr_mem_ram_size) {
    memory_map_t *mem = (memory_map_t *)object_data(self);
    return val_uint(4, memory_ram_size(mem));
}

static DEF_GETTER(attr_mem_rom_size) {
    memory_map_t *mem = (memory_map_t *)object_data(self);
    return val_uint(4, memory_rom_size(mem));
}

// === memory.read_cstring ====================================================
//
// Read a NUL-terminated string from guest memory at addr, escaping
// non-printable bytes. Used to migrate the legacy `$str.<src>`
// vocabulary onto the unified ${...} interpolator.

static DEF_METHOD(method_mem_read_cstring) {
    uint32_t addr = (uint32_t)argv[0].u;
    int max_chars = 96;
    if (argc >= 2) {
        int64_t mc = argv[1].i;
        if (mc > 0 && mc <= 4096)
            max_chars = (int)mc;
    }
    // Worst case: every byte escaped (4 chars) plus two quotes and the NUL.
    // Sized to the request on the heap rather than a fixed 8 KB stack buffer
    // that truncated a fully-escaped 4096-char read.
    size_t buf_size = (size_t)max_chars * 4 + 3;
    char *buf = (char *)malloc(buf_size);
    if (!buf)
        return val_err("memory.read_cstring: out of memory");
    size_t out = 0;
    buf[out++] = '"';
    for (int i = 0; i < max_chars; i++) {
        uint8_t b = memory_debug_read_uint8(addr + (uint32_t)i);
        if (b == 0)
            break;
        if (b >= 0x20 && b <= 0x7E) {
            buf[out++] = (char)b;
        } else {
            int n = snprintf(buf + out, buf_size - out, "\\x%02X", b);
            if (n < 0)
                break;
            out += (size_t)n;
        }
    }
    buf[out++] = '"';
    buf[out] = '\0';
    value_t v = val_str(buf);
    free(buf);
    return v;
}

static const value_t mem_def_max_chars = {.kind = V_INT, .i = 96};
static const arg_decl_t mem_read_cstring_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
    {.name = "max_chars",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &mem_def_max_chars,
     .doc = "max chars to read (1..4096)"},
};

// `memory.dump(addr, [count])` — hex-dump `count` bytes from `addr`.
// Replaces the legacy gdb-style `x` / `examine` command. `addr` accepts an
// integer or a string (alias / register name / expression resolved by the
// rich-parser). Output goes to stdout in the legacy `x` layout; the method
// returns true on dispatch success.
static DEF_METHOD(method_mem_dump) {
    // addr is V_NONE-kind: an integer, or a string symbol/alias the
    // address parser resolves (parse_address handles $hex / 0x / symbol).
    uint32_t addr = 0;
    bool addr_ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &addr_ok);
    if (addr_ok) {
        addr = (uint32_t)addr_u;
    } else if (argv[0].kind == V_STRING && argv[0].s) {
        addr_space_t sp;
        if (!parse_address(argv[0].s, &addr, &sp))
            return val_err("memory.dump: cannot resolve address '%s'", argv[0].s);
    } else {
        return val_err("memory.dump: addr must be an integer or a symbol name");
    }

    uint32_t nbytes = 64;
    if (argc >= 2) {
        int64_t count = argv[1].i;
        if (count <= 0)
            return val_err("memory.dump: byte count must be > 0");
        nbytes = (uint32_t)count;
    }
    if (nbytes > 512)
        nbytes = 512;

    // Classic hex + ASCII layout, 16 bytes per row. Uses the
    // side-effect-free debug read so a dump across unmapped pages can't
    // latch a spurious guest bus error.
    for (uint32_t i = 0; i < nbytes; i += 16) {
        gs_outf("$%08X  ", addr + i);
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes)
                gs_outf("%02x ", memory_debug_read_uint8(addr + i + j));
            else
                gs_outf("   ");
        }
        gs_outf(" ");
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes) {
                uint8_t byte = memory_debug_read_uint8(addr + i + j);
                gs_outf("%c", (byte >= 0x20 && byte <= 0x7e) ? byte : '.');
            }
        }
        gs_outf("\n");
    }
    return val_none();
}

static const value_t mem_def_dump_count = {.kind = V_INT, .i = 64};
static const arg_decl_t mem_dump_args[] = {
    {.name = "addr",
     .kind = V_NONE,
     .validation_flags = OBJ_ARG_POLY,
     .doc = "guest memory address: an integer, or a symbol / alias name"},
    {.name = "count",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &mem_def_dump_count,
     .doc = "byte count (max 512)"},
};

// `memory.translate(addr)` — report the debug-path translation of a logical
// address: MMU enabled state, supervisor/user walk results (validity +
// physical address), and the page-table backing of the physical page.
// Diagnostic aid for when memory.peek/find results look wrong: the debug
// read path is only trustworthy where this reports a valid mapping.
static DEF_METHOD(method_mem_translate) {
    uint32_t addr = (uint32_t)argv[0].u & g_address_mask;
    char buf[256];
    // No 68K PMMU/040 state, but the CPU translates by other means (the
    // PowerPC MMU, the Lisa's segment MMU): ask it, through the debug
    // interface.  This used to report "mmu=off" and the address itself.
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!g_mmu && dif && dif->translate && (g_lisa_mmu || dif->translate_mac)) {
        bool ok = false;
        uint32_t pa = dif->translate(dif->ctx, addr, &ok);
        snprintf(buf, sizeof(buf), "mmu=%s phys=0x%08x %s (machine.cpu.mmu.translate gives the typed form)",
                 dif->arch ? dif->arch : "cpu", pa, ok ? "valid" : "INVALID");
        return val_str(buf);
    }
    if (!g_mmu || !g_mmu->enabled) {
        uint32_t page = addr >> PAGE_SHIFT;
        const char *backing = "unmapped";
        if (page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            backing = pe->host_base ? "ram/rom" : (pe->dev ? "device" : "unmapped");
        }
        snprintf(buf, sizeof(buf), "mmu=off phys=0x%08x backing=%s", addr, backing);
        return val_str(buf);
    }
    uint32_t pa_s = 0, pa_u = 0;
    bool ok_s = mmu_translate_checked(g_mmu, addr, true, &pa_s);
    bool ok_u = mmu_translate_checked(g_mmu, addr, false, &pa_u);
    const char *backing_s = "unmapped";
    uint32_t page_s = pa_s >> PAGE_SHIFT;
    if (ok_s && page_s < g_page_count) {
        page_entry_t *pe = &g_page_table[page_s];
        backing_s = pe->host_base ? "ram/rom" : (pe->dev ? "device" : "unmapped");
    }
    snprintf(buf, sizeof(buf), "mmu=on super=%s phys_s=0x%08x user=%s phys_u=0x%08x backing_s=%s",
             ok_s ? "valid" : "INVALID", pa_s, ok_u ? "valid" : "INVALID", pa_u, backing_s);
    return val_str(buf);
}

static const arg_decl_t mem_translate_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "logical guest address"},
};

static DEF_GETTER(attr_mem_slowpath_count) {
    return val_uint(8, g_mem_slowpath_count);
}

static DEF_GETTER(attr_mem_slowpath_hist) {
    char buf[1024];
    size_t off = 0;
    for (int i = 0; i < 32; i++)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s$%02X:%llu", i ? " " : "", i,
                                (unsigned long long)g_mem_slowpath_hist[i]);
    return val_str(buf);
}

static const member_t memory_members[] = {
    {.kind = M_ATTR,
     .name = "ram_size",
     .doc = "Installed RAM in bytes, as the machine's memory map reports it",
     .attr = {.type = V_UINT, .get = attr_mem_ram_size, .set = NULL}},
    {.kind = M_ATTR,
     .name = "slowpath_count",
     .flags = M_CAT_ADVANCED,
     .doc = "CPU memory accesses taken through the slow path since process start (diagnostic)",
     .attr = {.type = V_UINT, .get = attr_mem_slowpath_count, .set = NULL}},
    {.kind = M_ATTR,
     .name = "slowpath_hist",
     .flags = M_CAT_ADVANCED,
     .doc = "Slow-path accesses bucketed by MB of (masked) address (diagnostic)",
     .attr = {.type = V_STRING, .get = attr_mem_slowpath_hist, .set = NULL}},
    {.kind = M_ATTR,
     .name = "rom_size",
     .doc = "Size in bytes of the loaded ROM image",
     .attr = {.type = V_UINT, .get = attr_mem_rom_size, .set = NULL}},
    {.kind = M_METHOD,
     .name = "read_cstring",
     .examples = EXAMPLES("machine.memory.read_cstring 0x910"),
     .doc = "Read a quoted, escape-encoded C string at addr",
     .method = {.result_doc = "the string, quoted, with non-printable bytes escaped as \\xNN",
                .args = mem_read_cstring_args,
                .nargs = 2,
                .result = V_STRING,
                .fn = method_mem_read_cstring}},
    {.kind = M_METHOD,
     .name = "dump",
     .examples = EXAMPLES("machine.memory.dump 0x400", "machine.memory.dump $pc 32"),
     .doc = "Hex-dump count bytes at addr",
     .method = {.args = mem_dump_args, .nargs = 2, .result = V_NONE, .fn = method_mem_dump}},
    {.kind = M_METHOD,
     .name = "translate",
     .examples = EXAMPLES("machine.memory.translate 0x400"),
     .doc = "Show the debug-path MMU translation of a logical address",
     .method = {.result_doc = "one line: MMU state, the supervisor and user walks, and the physical page's backing",
                .args = mem_translate_args,
                .nargs = 1,
                .result = V_STRING,
                .fn = method_mem_translate}},
};

static const class_desc_t memory_class = {
    .name = "memory",
    .members = memory_members,
    .n_members = sizeof(memory_members) / sizeof(memory_members[0]),
    .doc = "Guest memory: map, peek and poke",
};

// === memory.peek child class ================================================
//
// Three methods (b/w/l) that read sized values from guest memory at a
// caller-supplied address. Used by ${...} interpolation in logpoint
// messages and any expression that needs a peek.

static DEF_METHOD(method_mem_peek_b) {
    value_t v = val_uint(1, memory_debug_read_uint8((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}
static DEF_METHOD(method_mem_peek_w) {
    value_t v = val_uint(2, memory_debug_read_uint16((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}
static DEF_METHOD(method_mem_peek_l) {
    value_t v = val_uint(4, memory_debug_read_uint32((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}

// `memory.peek.bytes(addr, count)` — bulk byte read. Returns a
// V_BYTES blob, capped to 4 KB so the bridge output slot can hold the
// JSON-encoded payload. Replaces the per-byte fan-out the debug UI's
// memory pane used to do (128 separate gsEval calls → 128 bridge
// round-trips → noticeable lag while stepping). One call now suffices.
// One byte for peek.bytes with an explicit space.  "physical" reads the
// physical page table; "logical" reads through the CPU's own translation on
// every architecture -- the 68K MMU (or the Lisa's) inside
// memory_debug_read_uint8, and on a PowerPC machine, where the plain debug
// read is physical, the core's data-side translation first.
static uint8_t peek_byte_in_space(uint32_t addr, bool physical) {
    if (physical) {
        bool ok;
        uint32_t v = memory_debug_read_phys(addr, 1, &ok);
        return ok ? (uint8_t)v : 0xFF;
    }
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!g_mmu && !g_lisa_mmu && dif && dif->translate_mac && dif->translate) { // PowerPC
        bool ok;
        uint32_t pa = dif->translate(dif->ctx, addr, &ok);
        return ok ? memory_debug_read_uint8(pa) : 0xFF;
    }
    return memory_debug_read_uint8(addr);
}

static DEF_METHOD(method_mem_peek_bytes) {
    uint32_t addr = (uint32_t)argv[0].u;
    uint64_t count = argv[1].u;
    // `space` omitted keeps the historical meaning (through the 68K MMU;
    // physical on PowerPC); given, it means the same on every architecture.
    bool have_space = argc >= 3 && argv[2].kind == V_ENUM;
    bool physical;
    if (!debug_parse_space(argc, argv, 2, &physical))
        return val_err("memory.peek.bytes: space must be \"logical\" or \"physical\"");
    if (have_space && physical && g_lisa_mmu)
        return val_err("memory.peek.bytes: the Lisa has three physical spaces (RAM, I/O, ROM); read a logical address");
    if (count == 0)
        return val_bytes(NULL, 0);
    // Cap at 4 KB. The bridge serialises V_BYTES as a base64-ish JSON
    // string; 4 KB × 4/3 ≈ 5.5 KB, well under the mailbox result limit (GS_MBX_RESULT_MAX).
    if (count > 4096)
        count = 4096;
    uint8_t *buf = (uint8_t *)malloc(count);
    if (!buf)
        return val_err("memory.peek.bytes: out of memory");
    for (uint64_t i = 0; i < count; i++)
        buf[i] = have_space ? peek_byte_in_space((uint32_t)(addr + i), physical)
                            : memory_debug_read_uint8((uint32_t)(addr + i));
    value_t v = val_bytes(buf, (size_t)count);
    free(buf);
    return v;
}

static const arg_decl_t mem_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
};

static const arg_decl_t mem_peek_bytes_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
    {.name = "count", .kind = V_UINT, .doc = "byte count (max 4096)"},
    {.name = "space",
     .kind = V_ENUM,
     .enum_values = debug_space_values,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "\"logical\" (through the CPU's translation) or \"physical\"",
     .default_doc = "68K logical, PowerPC physical"},
};

static const member_t mem_peek_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .examples = EXAMPLES("machine.memory.peek.b 0x12f"),
     .doc = "Read 1 byte at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_b}           },
    {.kind = M_METHOD,
     .name = "w",
     .examples = EXAMPLES("machine.memory.peek.w 0x28e"),
     .doc = "Read 2 bytes (big-endian word) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_w}           },
    {.kind = M_METHOD,
     .name = "l",
     .examples = EXAMPLES("machine.memory.peek.l 0x16a"),
     .doc = "Read 4 bytes (big-endian long) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_l}           },
    {.kind = M_METHOD,
     .name = "bytes",
     .examples = EXAMPLES("machine.memory.peek.bytes 0x400 16"),
     .doc = "Read count bytes at addr (bulk; max 4096 bytes per call)",
     .method = {.args = mem_peek_bytes_args, .nargs = 3, .result = V_BYTES, .fn = method_mem_peek_bytes}},
};

static const class_desc_t mem_peek_class = {
    .name = "peek",
    .doc = "Side-effect-free reads of guest memory by width",
    .members = mem_peek_members,
    .n_members = sizeof(mem_peek_members) / sizeof(mem_peek_members[0]),
};

// === memory.poke child class ================================================
//
// Three methods (b/w/l) that write sized values to guest memory at a
// caller-supplied address. Pairs with memory.peek, replacing the legacy
// `set <addr>.<size> <value>` shell form.

static DEF_METHOD(method_mem_poke_b) {
    memory_debug_write_uint8((uint32_t)argv[0].u, (uint8_t)argv[1].u);
    return val_none();
}
static DEF_METHOD(method_mem_poke_w) {
    memory_debug_write_uint16((uint32_t)argv[0].u, (uint16_t)argv[1].u);
    return val_none();
}
static DEF_METHOD(method_mem_poke_l) {
    memory_debug_write_uint32((uint32_t)argv[0].u, (uint32_t)argv[1].u);
    return val_none();
}

static const arg_decl_t mem_poke_args[] = {
    {.name = "addr",  .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
    {.name = "value", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "value to write"      },
};

static const member_t mem_poke_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .examples = EXAMPLES("machine.memory.poke.b 0x12f 0"),
     .doc = "Write 1 byte at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_b}},
    {.kind = M_METHOD,
     .name = "w",
     .examples = EXAMPLES("machine.memory.poke.w 0x28e 0x3fff"),
     .doc = "Write 2 bytes (big-endian word) at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_w}},
    {.kind = M_METHOD,
     .name = "l",
     .examples = EXAMPLES("machine.memory.poke.l 0x16a 0"),
     .doc = "Write 4 bytes (big-endian long) at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_l}},
};

static const class_desc_t mem_poke_class = {
    .name = "poke",
    .doc = "Writes to guest memory by width",
    .members = mem_poke_members,
    .n_members = sizeof(mem_poke_members) / sizeof(mem_poke_members[0]),
};

struct object *memory_object_new(memory_map_t *mem, struct object **peek_out, struct object **poke_out) {
    *peek_out = *poke_out = NULL;
    struct object *obj = object_new(&memory_class, mem, "memory");
    if (!obj)
        return NULL;
    object_set_label(obj, "Memory");
    object_set_order(obj, 20);
    object_attach(machine_object(), obj);
    // memory.peek's accessors call into the global memory_read_* helpers
    // directly, so its instance_data is unused (likewise poke).
    *peek_out = object_new(&mem_peek_class, NULL, "peek");
    if (*peek_out)
        object_attach(obj, *peek_out);
    *poke_out = object_new(&mem_poke_class, NULL, "poke");
    if (*poke_out)
        object_attach(obj, *poke_out);
    return obj;
}

void memory_object_delete(struct object *obj, struct object *peek, struct object *poke) {
    struct object *nodes[3] = {peek, poke, obj}; // children first
    for (int i = 0; i < 3; i++) {
        if (!nodes[i])
            continue;
        object_detach(nodes[i]);
        object_delete(nodes[i]);
    }
}
