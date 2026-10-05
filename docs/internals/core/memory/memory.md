# Memory Subsystem

The memory subsystem provides address decoding and data access for the emulated
CPU. It maps the guest address space onto host memory (RAM/ROM) and device I/O
handlers through a page table, delivering fast inline access for the common case
while supporting device-mapped regions and cross-page accesses.

## Page Table Architecture

The memory map is organized as a flat array of **page entries**, one per 4 KB
page of the guest address space. Each page entry records how accesses to that
page should be handled:

```c
typedef struct page_entry {
    uint8_t *host_base;          // non-NULL: direct host pointer for RAM/ROM
    const memory_interface_t *dev; // non-NULL: device-mapped I/O handler
    void *dev_context;           // opaque context passed to device callbacks
    uint32_t base_addr;          // subtracted from addr before calling device
    bool writable;               // true for RAM, false for ROM and devices
} page_entry_t;
```

The page table is indexed by `addr >> PAGE_SHIFT` (where `PAGE_SHIFT = 12`,
giving 4 KB pages). For a 24-bit address space (Plus), this yields 4,096
entries; for a 32-bit address space (IIcx), up to 1,048,576 entries.

### Address Masking

All memory accesses are masked by `g_address_mask` before page table lookup:

- **Plus (24-bit):** `g_address_mask = 0x00FFFFFF` — upper 8 bits discarded
- **IIcx (32-bit):** `g_address_mask = 0xFFFFFFFF` — full 32-bit addressing

## Access Paths

### Fast Path (Inline)

The most common case — accessing RAM or ROM within a single page — is handled
entirely inline in `memory.h` with no function call overhead:

```c
static inline uint8_t memory_read_uint8(uint32_t addr)
{
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_read[masked >> PAGE_SHIFT];
    if (__builtin_expect(base != 0, 1))
        return LOAD_BE8((uint8_t *)(base + masked));
    return memory_read_uint8_slow(masked);
}
```

The fast path reads a **structure-of-arrays** table, not the `page_entry_t`
array: `g_active_read` and `g_active_write` hold one `uintptr_t` per page, and
a **zero entry means "take the slow path"** — device window, unmapped page, or
a page covered by a memory logpoint. The stored value is a host pointer
pre-biased by the guest address, so the fast path indexes it directly with no
second add.

`g_active_read`/`g_active_write` point at the supervisor or user table
according to the current privilege level; `write_sr` and the exception paths
repoint them. That is what makes a write-protected or supervisor-only page fall
to the slow path without any per-access permission test.

For 16-bit and 32-bit reads, the inline accessor additionally checks that the
access does not cross a page boundary (`(masked & PAGE_MASK) <= MEM_PAGE_SIZE -
N`); a failing check falls through to the slow path. Instruction prefetch uses
`memory_read_prefetch32`, which is the same fast path but returns only the
opcode word when the 32-bit read would cross into the next page — real hardware
aligns its prefetch DOWN and never reads past the page (MC68030UM §7.2).

There is no `pe->writable` test on this path: write protection is expressed by
leaving the page's `g_active_write` entry zero.

### Slow Path

The slow-path functions (`memory_read_uint16_slow`, etc.) handle two cases:

1. **Device I/O:** The page has a device handler (`pe->dev != NULL`). The
   device's read/write callback is invoked with the address adjusted by
   `pe->base_addr` so the device sees offsets relative to its own address range.

2. **Cross-page access:** A 16-bit or 32-bit access spans two pages. The slow
   path splits it into byte-sized reads/writes across the page boundary.

### Byte Order

The emulated Motorola 68000 is big-endian. The `LOAD_BE*` and `STORE_BE*`
macros use `__builtin_bswap*` intrinsics to convert between the host byte order
(little-endian on x86/ARM) and big-endian storage in the guest memory buffer.

## Page Table Population

### RAM and ROM

During initialization, `memory_map_init()` allocates the flat RAM buffer and
page table, then `populate_ram_rom_pages()` fills entries for:

- **RAM pages:** `host_base` points into the RAM buffer, `writable = true`
- **ROM pages:** `host_base` points into the ROM area (mirrored), `writable = false`

### Device Regions

When a device registers itself via `memory_map_add()`, the corresponding page
table entries are populated with:

- `host_base = NULL` (not direct-mapped)
- `dev` = pointer to the device's `memory_interface_t` (read/write callbacks)
- `dev_context` = opaque device pointer
- `base_addr` = the device's base address (subtracted from absolute addresses
  before invoking callbacks)

### Host-Backed Card Regions

A NuBus card registers its VRAM and declaration ROM on the bus map with
`memory_map_host_region()` (and `memory_map_host_region_alias()` for a second
window onto the same bytes).  Where those pages land depends on who owns the
machine's physical view:

- **68k machines** keep the window in the MMU's host-region list, which the
  table walk resolves through and the layout code fills from.
- **PowerPC machines** have no `mmu_state_t` at all, so they install a
  page-fill hook (`g_mem_host_fill`, set to the family's own page filler) and
  each window is filled straight into the page table through it.  The hook —
  not "is there an MMU" — is the discriminator, because on a checkpoint
  restore the outgoing machine's MMU is still installed while the incoming
  one builds its cards.

Card *register* windows go through `memory_map_add()` like any other device,
and are registered after the host regions so a device page wins its page.

### Device-Signalled Bus Errors

A device region can answer an access with a bus error instead of data by
calling `memory_signal_bus_error()`: the PDM's BART windows use it so an
empty NuBus slot faults a Slot Manager probe the way the bridge's bus
timeout does (docs/internals/machines/pdm/bart.md; the hardware fault and
timeout behavior is carried by [bart.md](../../../reference/machines/pdm/bart.md)
section 3.5).  The fault is latched and
delivered by the CPU seam at the sprint boundary, exactly like the unmapped
faults the slow paths raise.

### Inspection reads: `peek_*`

`memory_interface_t` carries `peek_uint8/16/32` beside the guest's
`read_*`.  A peek returns what the matching read would return now and changes
nothing in the device.  The debugger's reads (`memory.peek`, `find.*`,
`machine.cpu.mmu.peek`, the Mac-globals and disassembly views — everything on
`memory_debug_read_*`) dispatch to `peek_*`, falling back to `read_*` where it
is NULL, and a missing width composes from the narrower peek.  So **a device
whose read has any side effect implements `peek_*`**: a VBL toggle, a RAMDAC
RGB phase or auto-incrementing index, a FIFO pop, a flag cleared on read, and
signalling a bus error (`memory_signal_bus_error`) all count.  The usual shape
is one register-read function taking `bool peek`, with each side effect
guarded by `if (!peek)`, so the two paths cannot drift apart (#159).

An I/O island that fronts several chips (the mac030 island engine, PDM's
AMIC, TNT's Grand Central, Gossamer's Heathrow) has a peek of its own that
routes an inspection to each chip's peek: `memory_iface_read8` for a chip
behind a `memory_interface_t`, the chip's `_peek` sibling function
(`scsi_53c96_peek`, `mesh_peek`, `swim3_peek`, ...) otherwise, and on the
mac030 engine a handler row's `peek_fn`, set wherever its `read_fn` has a
side effect.

`memory.poke` is a real write, with the write's effect on the device, but it
is not a guest bus cycle: a transfer error the device answers it with is
discarded rather than delivered to the guest.

### Per-Instance Ownership

Each `memory_map_t` instance stores its own `page_table` and `page_count`.
The global `g_page_table` pointer is set to the active instance's table during
initialization. During checkpoint restore, when a new memory map replaces the
old one, `memory_map_delete()` only clears the globals if the instance being
destroyed owns the currently active page table. This prevents use-after-free
during the teardown/rebuild sequence.

## MMU Integration (IIcx / 68030)

For the Macintosh IIcx, the 68030's built-in PMMU translates logical addresses
to physical addresses. The page table serves as the translation layer:

- When the guest OS writes to MMU registers via `PMOVE` or executes `PFLUSH`,
  the emulator **invalidates** the affected SoA entries. It does **not** rebuild
  them: an earlier design walked the guest's tables eagerly on every
  invalidation and that loop alone cost **37% of an SE/30 boot**
  (`docs/notes/mmu-tlb-invalidate-perf.md`). Entries are refilled lazily, one
  page per fault.
- The fault path is `mmu_handle_fault`: a zero SoA entry takes the slow path,
  which walks the guest's translation tables, maintains the architectural U/M
  history bits in them, and fills the entry. A page becomes writable through
  the SoA only once its descriptor is marked modified, so the first write to a
  clean page deliberately re-faults — that is the hardware's modified-bit
  protocol, and it is what a guest's VM dirty-page accounting depends on.
- During normal execution, a translated access uses the same inline fast path
  with **zero additional overhead** — the translation is baked into the SoA
  entry.
- **With the MMU enabled, the slow path dispatches device I/O on the
  *physical* page from the table walk, never the logical page-table entry.**
  A user-virtual address whose upper byte coincides with a host MMIO range
  (e.g. virtual `$47F01000` landing in the IIfx ROM window
  `$40000000`–`$4FFFFFFF`) must fault, not silently return the device's
  value — A/UX's `copyin` depends on that fault to demand-page user pages. A
  transparent-translation (TT) match means logical == physical, so there the
  logical entry is the right dispatch and the walk is skipped (kernel I/O is
  typically TT-mapped). Cross-page accesses split into byte halves, each of
  which goes through this path.
- For the Plus (no MMU), the SoA is populated lazily from the identity map and
  changes only when a device window or logpoint is installed.

The 68030 PMMU is `src/core/memory/mmu.c` (with `mmu.h`, a full interface, not
a stub); the 68040's on-chip MMU is `mmu040.c`; the Lisa's segment MMU is
`lisa_mmu.c` (the Lisa's segment MMU hardware is carried by
[mmu.md](../../../reference/machines/lisa/mmu.md)); and the PowerPC BAT/HTAB translation is `ppc_mmu.c`. The 68030
walker also caches early-termination descriptors in a small block cache that
models the real ATC's residency — see the comments around `atc_record`. An
early-termination page frame is aligned only to the page size, not to the
range the descriptor covers: the physical address is the frame *plus* the
unwalked logical bits (MC68030UM 9.5.3.1). The IIci ROM relies on this — its
level-A descriptor `$00050019` places logical 0 at physical `$50000`, past
the RBV's in-RAM screen buffer.

Each of these MMUs also answers the debugger's `machine.cpu.mmu.translate` /
`walk` / `map` / `descriptor` through one side-effect-free translation
function (`mmu_debug_translate`, `mmu040_debug_translate`,
`ppc_mmu_debug_translate`, `lisa_mmu_debug_translate`) that fills an
`mmu_xlate_t` and, for `walk`, records its steps into an `mmu_trace_t`
(`mmu_trace.h`) from inside the real walker — see
[debug_mmu.md](../debug/debug_mmu.md).

## Memory Logpoints (Fast-Path-Preserving Watchpoints)

The shell command `debug.logpoints.add addr=<addr> mode=read|write` installs a memory logpoint
that streams a log line on every access without halting execution. The
implementation must not slow down unrelated accesses, so the design is:

- A per-page reference-count array `g_mem_logpoint_page_count[]` (one 16-bit
  counter per 4 KB page) tracks how many memory logpoints currently cover each page.
  Allocated alongside the SoA arrays; zeroed when no memory logpoints are set.
- When a logpoint is installed, every page it covers has its reference count
  incremented and **its SoA fast-path entries (`g_supervisor_read`,
  `g_supervisor_write`, `g_user_read`, `g_user_write`) zeroed**. Subsequent
  accesses to those pages take the slow path.
- `mmu_fill_soa_entry()` (called from MMU TLB misses) checks the same refcount
  and skips the fill, so the page stays slow-path even after the MMU re-walks.
- The slow-path functions detect this case (`pe->host_base != NULL` and the
  refcount is non-zero), perform the access directly via `pe->host_base`, then
  invoke the registered logpoint hook. The hook lives in `debug.c` and walks
  the logpoint list to emit matching events through the standard `LOG_WITH`
  pipeline.
- When a logpoint is removed, `memory_logpoint_uninstall()` decrements the
  refcount; if it reaches zero, the SoA entry is rebuilt from the cold-path
  page entry (or left zero so the next access re-fills via MMU).
- **Device pages are covered too.** A page that dispatches to a device has no
  host backing, so the route above — which reads through `pe->host_base` —
  cannot see it. The device dispatch sites therefore go through the
  `dev_read8/16/32` / `dev_write8/16/32` wrappers, which notify the hook with
  the value the device answered (or was given). Device pages never sit in the
  SoA fast path anyway, so this costs the nothing-armed load and nothing more.
  Without it a logpoint on an I/O register reported zero events forever, which
  reads exactly like "the guest never touches this register".

The fast-path inline accessors in `memory.h` are unchanged — there are no extra
branches or memory loads on the hot path. Only pages with active logpoints
incur the slow-path cost.

Hooks and helpers:
- `g_mem_logpoint_page_count` — per-page refcount (in `memory.h`)
- `g_mem_logpoint_hook` — function pointer set by debug.c (`debug_memory_logpoint_hook`)
- `memory_logpoint_install(start_page, end_page)` / `..._uninstall(...)` — page refcount helpers
- `dev_read8/16/32`, `dev_write8/16/32` (memory.c) — device dispatch + hook notify

## Code-Page Coherence

The predecoded executors (`docs/internals/core/cpu/predecode.md`) cache decoded
instructions per host page; the memory layer keeps those caches coherent
without a check on the fast path.

- **Marks.** `memory_code_page_mark(host)` sets a per-page mark for the
  host page (the selected map's flat RAM+ROM image is region 0 of
  `g_mem_code_regions`, re-registered by `memory_map_select` whenever a
  different map is selected; a card's host-backed window may register as
  another) and zeroes the
  page's write entries in every SoA table it has aliases in.  Aliases are
  found by a reverse scan over the 256-page chunks flagged in
  `g_mem_soa_chunk` (the selected map's chunk table).  `memory_code_page_unmark` clears the mark; the
  entries come back on the next refill.
- **One planter.** Every site that plants a write entry —
  `rebuild_soa_page`, `memory_populate_pages`, `memory_populate_ram_mirror`,
  `mmu_fill_soa_entry` (68030), `user_soa_fill` (PowerPC), the
  PDM/TNT/Gossamer glue — goes through `memory_write_fill(page, host, adjusted)`, which
  sets the chunk flag and **declines on a marked page** (the store then
  takes the slow path).  Do not write `g_*_write[]` directly.
- **Slow-path notification.** The 8/16/32-bit slow write paths call
  `memory_host_written(host, len)` when the target is marked (before the
  store lands, so a block executing the page is invalidated before the
  new word can be fetched).
- **Direct host writers** — anything that writes the image without going
  through the guest store paths — call `memory_host_written` themselves.
  The inventory:

| Writer | Where | Hook call |
|---|---|---|
| `memory.write` / debug pokes | `memory_debug_write_*` (memory.c) | after the store |
| ROM installation | `memory_map_init` (memory.c), before the map's first instruction | a new map is a new code region (`memory_map_select`) |
| Checkpoint restore of the image | `memory_map_init` + the restore path | generation bump (`g_mem_map_generation`) resets the pool |
| PowerPC HTAB R/C write-back | `ppc_mmu.c` | `memory_host_written` |
| AMIC DMA (SCSI, SCC, sound, floppy) | `amic.c` byte writers | `memory_host_written` |
| DBDMA channels | `tnt.c`, `gossamer.c` memcpy paths | `memory_host_written` |
| 53C8xx SCRIPTS engine | `scripts53c8xx.c` | `memory_host_written` |
| IIfx / IOP DMA | `iifx.c`, `iop_swim.c` | `memory_host_written` |
| Mac II / 030 glue mirrors | `mac030_glue.c` | `memory_write_fill` for the aliases; stores notify |

- **PowerPC user mode.** The user SoA tables are filled *logically*
  (`user_soa_fill`), and a store whose write fill the marks refuse falls
  back to a physical address that the inline accessor then uses to index
  those logically-filled tables.  `user_phys_fallback` (ppc_mmu.c) keeps
  the slot at that physical index empty — on the walk and on the
  translation-TLB hit — so the access takes the physical slow path instead
  of landing in whatever logical page sits at that index.  The evicted
  page refills on its next slow access.

A new device that DMA-writes guest memory through a host pointer must
call `memory_host_written(host, len)` after the copy.  The debug-build
audit in the executors (`PD_AUDIT_*`) traps on the first stale entry a
missing call would leave behind.

Counters: `g_mem_code_write_count` (stores that reached a marked page),
`g_mem_slowpath_count` (every slow-path access; the elision level-2
guard), both visible through `machine.memory`.

## Key Files

| File | Purpose |
|------|---------|
| `src/core/memory/memory.h` | Page table types, inline accessors, public API |
| `src/core/memory/memory.c` | Page table allocation, population, slow-path handlers |
| `src/core/memory/mmu.h` | 68030 PMMU state struct and API (`mmu.c`) |
| `src/core/memory/mmu_trace.h` | The debugger's translation answer and walk trace, shared by every MMU kind |

---

## Hardware Details (Macintosh Plus)

The ROM construction and the overlay decode below are carried authoritatively
by the compact Macintosh reference pages — [compact.md](../../../reference/machines/compact/compact.md)
section 2.5 (ROM) and section 2.8 (reset and the ROM overlay); see also
[plus.md](../../../reference/machines/compact/plus.md).

### ROM

The Macintosh Plus uses two 512K-bit (64K × 8) ROM chips, providing a total of 128 KB of ROM storage. Each chip has 8 data pins, handling one half of the 16-bit system data bus:

* The "low" ROM is connected to data lines D0–D7.
* The "high" ROM is connected to data lines D8–D15.

Each ROM chip has 16 address pins, which connect to address lines A1 through A16 of the system bus. Since the Macintosh Plus uses a 16-bit data bus, A0 of the system bus is not used in this connection. In addition, address line A17 from the system bus is also wired to the ROM socket.

In theory, this design allows the ROM sockets to accommodate chips up to 1M-bit (128K × 8). However, in practice, the Macintosh Plus uses A17 as the **output enable** (/OE) signal for both ROM chips. This means that whenever A17 is high, the ROM chips are disabled and do not drive the bus.

The location of the ROM within the overall memory map is controlled by the /ROMCE signal that is provided by the "CAS" PAL (20L8A). Effectively, the ROM normally occupies the address range **0x00400000–0x004FFFFF**, but is also mapped into **0x00000000–0x000FFFFF** when overlay is enabled. Within this region:

* **0x00000–0x1FFFF**: The standard 128 KB ROM is accessible.
* **0x20000–0x3FFFF**: This range is blank, as A17 is high and the ROM is disabled.
* **0x40000–0x5FFFF**: The ROM reappears as an alias of the original, since only A1–A16 are decoded by the ROM chips.
* This pattern continues, with alternating ROM images and gaps, repeating until **0xFFFFF**.

The bootstrap code actually tests this layout, reading two long words from 0x00420000 and 0x00440000 respectively and comparing them. They should not be the same, as 0x00420000 would be the beginning of a "gap", whereas 0x00440000 is the beginning of a new ROM alias image.

