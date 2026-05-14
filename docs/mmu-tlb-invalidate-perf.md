# Why SE/30 Boots Slowly: The MMU TLB Invalidate Hotspot

This document is a deep dive on a single, surprising performance finding: the
emulator's SE/30 cold boot runs at roughly **13 MIPS**, while a Macintosh Plus
cold boot on the same host runs at roughly **95 MIPS** — a ~7× gap. The cause
is almost entirely in one function (`mmu_invalidate_tlb`) and one loop. This
note explains, from the ground up, what that function does, why it exists, and
why it ends up dominating boot time on a 32-bit machine.

It is intentionally pedagogic: it assumes you know C and have some idea what
"page table" means, but it does not assume you know this codebase or the
patterns it uses.

---

## 1. The two universes: guest and host

We are writing an emulator. There are two completely separate machines involved
in every memory access:

- The **guest machine** — the emulated Macintosh. It has a 68000 or 68030 CPU,
  some RAM, some ROM, and some memory-mapped devices (VIA, SCC, SCSI…). The
  guest CPU executes guest code that reads and writes guest addresses.
- The **host machine** — your actual computer running the emulator binary. It
  has its own CPU, its own RAM, and a virtual address space. The emulator
  allocates buffers in host memory to back the guest's RAM, ROM, etc.

A guest memory access is, at bottom, "convert a guest address into a host
pointer (or a device callback) and dispatch to it." Doing this on every single
guest instruction is what we have to make fast.

### The 68030 PMMU is the *guest's* MMU, not ours

When the SE/30's ROM or operating system writes to the 68030's PMMU registers
(CRP, SRP, TC, TT0, TT1), it is configuring the **guest** CPU's translation
hardware. The guest builds page tables in *guest RAM*, points CRP at them, and
expects the 68030 to translate logical → physical on every fetch and data
access.

We emulate that faithfully. But "we emulate the 68030 PMMU" doesn't mean we
implement it as a multi-level walker on every guest access — that would be
absurdly slow. Instead, we maintain a **host-side cache** of the guest's
translation: an array indexed by `guest_address >> 12` that gives us, for each
4 KB guest page, the host pointer (or device handler) to use. On a cache miss,
*then* we walk the guest's actual page tables. On a cache hit (the overwhelming
majority of accesses), we just index, load, dispatch.

That host-side cache is what this document is about.

> Terminology aside: in most of the source it's called "the SoA fast path" or
> "the software TLB" or just "the TLB." All three mean the same thing — the
> emulator's host-side cache of guest translations.

---

## 2. AoS and SoA: two ways to lay out an array of records

This codebase uses both, deliberately. The terminology comes from data-oriented
design:

- **AoS — Array of Structures.** You have one struct per element, and an array
  of those structs. Looks natural in C:

  ```c
  struct page_entry { uint8_t *host_base; void *dev; void *dev_ctx; ... };
  struct page_entry table[N];  // table[i] holds all fields for page i, packed together
  ```

  Accessing one field of one element is one indexed load — good when you
  consume many fields of the same element together. Bad when you stream
  through the array touching only *one* field per element: you load whole
  cache lines and use only a fraction of each.

- **SoA — Structure of Arrays.** Same logical record, but each field lives in
  its own dense array:

  ```c
  uint8_t *host_base[N];
  void    *dev[N];
  void    *dev_ctx[N];
  ```

  Streaming through one field (say `host_base[0..N]`) packs cache lines
  perfectly. Touching multiple fields of the same element means multiple
  separate indexed loads, but each from a small, dense array.

For our fast path we only need **one** value per page: a precomputed offset such
that `(uintptr_t)host_pointer = entry + guest_address`. So we keep that single
value in a dense `uintptr_t[]` (the SoA) and consult it on every access. The
richer per-page metadata (device handler, writable flag, base address for
subtracting before the device callback) only matters on the slow path — so it
lives in a separate AoS, consulted only on a miss.

The result is two structures that hold related but differently-shaped views of
the same underlying truth.

---

## 3. The actual structures, end to end

There are **five** memory-resident structures we have to keep in our heads.
Four of them are big flat arrays; one is a tiny side list. There is also a
linked list of region descriptors, but it's not in any hot path — it exists
for printing and removal.

### 3.1 `g_page_table` — the AoS, the "cold" canonical map

```c
typedef struct page_entry {
    uint8_t *host_base;            // non-NULL: direct access (RAM/ROM/VRAM)
    const memory_interface_t *dev; // non-NULL: device-mapped I/O
    void *dev_context;
    uint32_t base_addr;
    bool writable;
} page_entry_t;                    // sizeof == 32 bytes (with padding)

extern page_entry_t *g_page_table; // size = g_page_count entries
extern int g_page_count;           // SE/30: 1,048,576;  Plus: 4,096
```

One entry per 4 KB page of the guest's physical/logical address space. Indexed
directly by `(addr & g_address_mask) >> PAGE_SHIFT`.

- For the **Macintosh Plus** the address bus is 24 bits → 16 MB → 4,096 pages
  → AoS is 128 KB total. Fits comfortably in any L2.
- For the **SE/30 (and any 030 machine)** the address bus is 32 bits → 4 GB
  → 1,048,576 pages → AoS is **32 MB total**. Way bigger than any L1 or L2,
  bigger than the L3 in many cloud VMs.

Mostly empty. A real boot only populates ~1500 entries (one per page of host
RAM, ROM, VRAM, plus device pages for the VIA/SCC/SCSI/etc.). Everything else
is zero — meaning "unmapped guest address, accessing it should bus-error or
return $FF."

`g_page_table` is populated **once** at machine init by `memory_map_add` and
related setup. After that it changes only when devices are added or removed.
It is **never** written to by `mmu_invalidate_tlb`.

### 3.2 The four SoA arrays — the "hot" software TLB

```c
extern uintptr_t *g_supervisor_read;  // size = g_page_count entries
extern uintptr_t *g_supervisor_write;
extern uintptr_t *g_user_read;
extern uintptr_t *g_user_write;
```

Each is `g_page_count` × 8 bytes. For SE/30 that's 4 × 8 MB = **32 MB of SoA
across all four**. (Same order of magnitude as the AoS — not a coincidence;
both are sized to cover the full address space at page granularity.)

Each entry holds a single `uintptr_t`. The convention is:

- **Zero** means "not cached / take the slow path." Either the page isn't
  resolved yet (TLB miss), or it's a device page that must dispatch to a
  callback, or it's covered by a memory logpoint that must be observed.
- **Non-zero** holds a pre-adjusted value such that the hot path can do
  `(uint8_t *)(soa_entry + guest_address)` to get the host pointer for the
  byte:

  ```c
  uintptr_t adjusted = (uintptr_t)host_base - (page_index << PAGE_SHIFT);
  // then:  host_ptr == adjusted + guest_addr
  ```

  The subtraction collapses two indexing steps (page lookup, then
  base + offset) into one addition.

Why four arrays?

- **Read vs write**: ROM pages are mapped only in the read tables; their write
  entries stay zero. Writes to ROM thus hit the slow path which silently drops
  them.
- **Supervisor vs user**: when the 68030 MMU is enabled, the same logical
  page can resolve to different physical pages (or be inaccessible) depending
  on the CPU's S-bit at the moment of the access. We materialize both views
  ahead of time and let the dispatcher switch which pair is active per sprint
  via two pointers:

  ```c
  extern uintptr_t *g_active_read;   // points to g_supervisor_read OR g_user_read
  extern uintptr_t *g_active_write;  // points to g_supervisor_write OR g_user_write
  ```

  When the MMU is disabled, the CPU identity-translates regardless of S-bit,
  so all four SoA arrays hold the *same* mapping for every host-backed page.

### 3.3 `g_tlb_track[]` — the "what we put in the SoA" side list

```c
#define TLB_TRACK_MAX 8192
static uint32_t g_tlb_track[TLB_TRACK_MAX]; // populated page indices
static int g_tlb_track_count = 0;
static bool g_tlb_track_overflow;
```

A tiny static array (max 8192 entries × 4 bytes = 32 KB). It records the
**page indices** for which we have at some point written a non-zero value
into any of the four SoA arrays, since the last full invalidation.

It exists for one reason: on invalidation we need to zero those SoA entries
back out. Without this list we'd have to `memset` all 32 MB of SoA on every
invalidate to be safe; with it, we walk a list of ~1500 indices and zero
exactly the entries that need zeroing.

If we ever install more than 8192 entries between invalidations, the
overflow flag flips and the next invalidate falls back to full `memset`.

`g_tlb_track` is **not** a list of "AoS pages with `host_base != 0`." It's a
list of "SoA slots currently believed to be non-zero." Those sets overlap a
lot in practice but they are conceptually different.

### 3.4 `mem->map` — the linked list of region descriptors

```c
typedef struct mapping {
    const char *name;
    uint32_t addr, size;
    memory_interface_t memory_interface;
    void *device;
    struct mapping *next;
} mapping_t;
```

One node per call to `memory_map_add`. Maybe 10–20 nodes total: "RAM at
$0..$400000", "ROM at $40000000..$40100000", "VRAM at …", "VIA at …", "SCC
at …", and so on.

It's used by `memory_map_print` (for the `memory.map` shell command) and by
`memory_map_remove`. **It is not consulted on any hot path.** It is also
*not* the list of populated page indices — its granularity is "region," not
"page."

### 3.5 The relationship, in one diagram

```
                    canonical, persistent              fast-path cache
                    (set once at machine init)         (rebuilt on invalidate)

guest page 0 --->   g_page_table[0]                    g_supervisor_read[0]
                    { host_base = ptr_to_RAM[0],       = (uintptr_t)host_base
                      dev=NULL, ... }                  g_user_read[0]
                                                       = same
                                                       g_supervisor_write[0]
                                                       = same (RAM is writable)
                                                       g_user_write[0]
                                                       = same
...
guest page X --->   g_page_table[X]                    g_*_read[X]  = 0
                    { host_base=NULL, dev=NULL }       g_*_write[X] = 0
                    (unmapped — most of the 1M slots)
...
guest page Y --->   g_page_table[Y]                    g_*_read[Y]  = 0 (forced slow path)
                    { host_base=NULL,                  g_*_write[Y] = 0
                      dev = via_iface, ... }
...

                                          plus, on the side:
                                          g_tlb_track = { 0, 0x100, 0x400, ... }
                                          — the list of indices currently
                                            non-zero in the SoA arrays.
```

---

## 4. The hot path, in 4 lines

This is what executes on (almost) every guest memory read:

```c
uintptr_t base = g_active_read[guest_addr >> PAGE_SHIFT];
if (__builtin_expect(base != 0 && (guest_addr & PAGE_MASK) <= 4092, 1))
    return LOAD_BE32(base + guest_addr);     // hot path: 1 shift, 1 load, 1 add
return memory_read_uint32_slow(guest_addr);  // cold: device, MMU miss, unmapped
```

The fast path needs the SoA entry to be filled in. Any non-zero value is a
"yes, this page is direct host-backed memory, read from `base + addr`." Zero
means "I don't have it; go figure it out."

---

## 5. The slow path's two regimes

`memory_read_uint*_slow` is the function called when the SoA entry is zero. It
has different behavior depending on whether the guest MMU is enabled:

- **MMU disabled (TC.E == 0).** The 68030 identity-translates. Any access
  whose SoA entry is zero is one of:
    1. A device page → consult `g_page_table[page].dev` and call its handler.
    2. A logpoint page → translate and notify the logpoint hook.
    3. Unmapped → bus-error or return $FF.
  In the current code, a host-backed RAM/ROM page **cannot** reach the slow
  path while the MMU is disabled, because the eager-fill step in
  `mmu_invalidate_tlb` guarantees its SoA entry was pre-populated. (That's
  what makes the eager fill load-bearing.)

- **MMU enabled (TC.E == 1).** The 68030 walks guest page tables. The slow
  path calls `mmu_handle_fault`, which either does a transparent-translation
  match or a multi-level walk of the guest's *real* page tables (the ones
  rooted at the guest's CRP / SRP, living in guest RAM). On success it fills
  the SoA entry for the relevant access mode (read/write × user/super) and
  appends to `g_tlb_track`. The retry then hits the fast path.

So **when the MMU is enabled, the SoA is populated lazily** — only pages the
guest actually touches get filled in. That's cheap.

The painful case is the **transition from MMU-enabled back to MMU-disabled**
(or any invalidate while the MMU is disabled), where we must restore identity
mappings for *all* host-backed pages eagerly. That's where the 32 MB scan
comes from.

---

## 6. What `mmu_invalidate_tlb` actually does

The function lives in `src/core/memory/mmu.c`. It runs in response to any
PMMU register change that could change translations — PMOVE to TC, SRP, CRP,
TT0, TT1, and explicit PFLUSH/PFLUSHA instructions. It is also implicitly
invoked when `_SwapMMUMode` toggles the MMU on/off, which the SE/30 boot ROM
does **2467 times** during a 4 MB-RAM SSW-6.0.8 cold boot (measured).

Logically, on every call, it does three things:

### Step A — Decide whether we can early-out.

If the MMU is currently disabled, and was *also* disabled at the previous
invalidate, then nothing observable has changed — the SoA still holds the
correct identity mappings. Return immediately. About 33% of calls (824 / 2467
in the measured boot) take this fast exit.

### Step B — Zero the previously-installed SoA entries.

Walk `g_tlb_track[0..count]` (~1500 entries). For each `p`, write 0 to
`g_supervisor_read[p]`, `g_user_read[p]`, `g_supervisor_write[p]`,
`g_user_write[p]`. Reset `g_tlb_track_count = 0`.

This is purely SoA writes. The AoS (`g_page_table`) is untouched. After this
step, **all 4 × 8 MB SoA arrays are entirely zero** (or at least, every entry
we previously installed is zero — if we never installed a particular slot,
it was already zero).

### Step C — If MMU is now disabled, rebuild identity mappings.

This is the step that costs 32 MB of memory traffic per call.

The intent: every host-backed page (RAM, ROM, VRAM) needs its SoA entries
filled with the identity mapping, so that the boot ROM and OS can read/write
those pages via the fast path. Without this, the very next guest memory read
to RAM would hit a zero SoA entry, fall to the slow path, and find no
mechanism to fill itself in (see §5 — the slow path doesn't lazy-install for
the MMU-disabled regime today). Things would grind.

The current implementation is a brute linear scan:

```c
for (int p = 0; p < g_page_count; p++) {           // 1,048,576 iterations on SE/30
    page_entry_t *pe = &g_page_table[p];           // load 32 bytes from AoS
    if (pe->host_base && !pe->dev) {               // branch — almost always false
        uint32_t guest_base = (uint32_t)p << PAGE_SHIFT;
        uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
        tlb_track_page(p);
        g_supervisor_read[p] = adjusted;
        g_user_read[p]       = adjusted;
        if (pe->writable) {
            g_supervisor_write[p] = adjusted;
            g_user_write[p]       = adjusted;
        }
    }
}
```

The loop body is trivial. The expense is the **search** — we don't know
*which* of the 1,048,576 AoS slots have `host_base` set, so we touch all of
them. The 1500-ish that match get installed; the rest just contribute a
cache-line load and a not-taken branch.

Quantitatively, per call:
- 1 M loop iterations.
- 1 M × 32 B = 32 MB of AoS bytes streamed (32 B is `sizeof(page_entry_t)` on
  x86-64; one cache line is 64 B, so two adjacent AoS entries share a line).
- ~1500 × 4 = 6000 SoA stores. Negligible.

Per boot (821 disabled-mode invalidates):
- ~1 G iterations.
- ~26 GB of AoS reads — most going through DRAM since the AoS doesn't fit in
  cache and is evicted between scans by the rest of the working set.

That's why `mmu_invalidate_tlb` ends up at 37% of total CPU time in the
profile. Not because it does a lot of *work* — it does a lot of *searching*
to find a small amount of work.

### Step D — Bookkeeping.

Save `mmu->tlb_was_enabled = now_enabled` for next call's early-out test.

---

## 7. Why the Plus doesn't have this problem

The Plus has no MMU (the 68000 doesn't have one) and `mmu_invalidate_tlb` is
never called on a Plus machine. Even if it *were* called, the AoS would be
4,096 entries × 32 B = **128 KB total** — fits in L2 and one scan costs
microseconds. The 256× size ratio (1 M vs 4 K) is the entire reason this
finding is SE/30-specific.

---

## 8. Implementation status

**Fix 2 (lazy install) was implemented** in the same commit that introduced
this doc. The eager-repop loop in `mmu_invalidate_tlb` is gone; the slow
paths in `memory.c` now lazy-install identity SoA entries via
`rebuild_soa_page` on first access to a host-backed page after each
invalidate.

Measured boot-matrix MIPS, before vs after:

| Row | Before | After |
|---|---|---|
| ssw-2.0 / Plus 1 MB | 88 | 75 |
| ssw-3.2 / Plus 2 MB | 89 | 78 |
| ssw-4.2 / Plus 2.5 MB | 92 | 90 |
| ssw-6.0.8 / Plus 4 MB | 86 | 84 |
| **ssw-6.0.8 / SE/30 4 MB** | **16** | **34–35** |

Plus rows are unchanged within run-to-run variance (the Plus has no MMU, so
neither `mmu_invalidate_tlb` nor the slow-path lazy-install runs in steady
state for Plus). SE/30 boot is ~2.2× faster. The residual gap vs Plus comes
from genuine 68030 costs (table walks via `mmu_handle_fault`, I/O penalties,
the Universal ROM's 13 s boot-drive-discovery poll loop) — separate issues
not addressed by this change.

## 9. The two reasonable fixes (as originally considered)

Both eliminate the 32 MB scan. They differ in how invasive they are.

### Fix 1 — Maintain a side-list of populated AoS indices.

Add a parallel list to `g_page_table`:

```c
static uint32_t g_aos_populated[MAX_POPULATED]; // page indices with host_base != 0
static int g_aos_populated_count = 0;
```

Maintain it in `memory_map_add` and any other site that sets a `host_base`
(RAM/ROM/VRAM setup in `memory.c`), and in `memory_map_remove`. The
repopulate step in `mmu_invalidate_tlb` then walks this list (~1500 entries)
instead of all 1 M AoS slots.

- ✅ Smallest behavior change. Same algorithm, smaller iteration set.
- ✅ AoS scan cost goes from ~1.6 ms to ~10 µs per invalidate.
- ⚠️ One more piece of state to keep in sync. Wrong-list bugs would manifest
   as silent missing mappings after invalidate.

### Fix 2 — Lazy install (delete Step C entirely).

Stop pre-filling the SoA after invalidation. The SoA is empty after Step B,
period. Extend the slow path so that on MMU-disabled access to a page with
`pe->host_base != 0 && !pe->dev` (and no logpoint), it installs the identity
SoA entry, appends to `g_tlb_track`, and retries.

```c
// pseudo, inserted near the top of each *_slow path:
if ((!g_mmu || !g_mmu->enabled) && pe->host_base && !pe->dev
    && !logpoint_covers_page(page)) {
    uint32_t guest_base = page << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
    g_supervisor_read[page] = adjusted;
    g_user_read[page]       = adjusted;
    if (pe->writable) {
        g_supervisor_write[page] = adjusted;
        g_user_write[page]       = adjusted;
    }
    tlb_track_page(page);
    return LOAD_BE32(adjusted + addr);   // retry as direct host load
}
```

- ✅ No new state to maintain.
- ✅ Symmetric with the MMU-enabled regime (which is already lazy via
   `mmu_handle_fault`).
- ✅ Cold pages never cost anything.
- ✅ AoS scan cost goes to zero. Hot pages take a slow-path detour exactly
   once per invalidate.
- ⚠️ Slow path now mutates SoA state in two regimes instead of one. Need to
   review the same edge cases (logpoints, ROM vs RAM, supervisor-only pages
   when MMU happens to be transiently disabled) that the eager fill already
   handles.
- ⚠️ Slightly higher per-access cost on the very first touch of each page
   after each invalidate. Profile-equivalent to the existing
   `mmu_handle_fault` cost in the enabled regime, which is small.

### Which we picked

We picked Fix 2. The mem->map unification (initially considered as a third
option) turned out to be complicated by the SE/30 ROM overlay, which
dynamically reassigns pages 0..rom_pages between RAM and ROM at boot time
via VIA1 PA4 — that isn't a static region and would have needed special
machinery to keep `mem->map` in sync. Lazy install handles it for free
because the slow path just consults whatever the current `g_page_table[p]`
says, which is already updated by the overlay code (`se30_set_rom_overlay`).

The implemented change:

- Deleted lines 524–546 of `src/core/memory/mmu.c` (the 32 MB scan).
- Extended `rebuild_soa_page` in `src/core/memory/memory.c` to (a) call
  `tlb_track_page` so the next invalidate zeroes it, (b) skip device and
  logpoint pages so the slow-path-required pages stay slow.
- Exposed `tlb_track_page` via `src/core/memory/mmu.h` (was `static`).
- Added a `can_lazy_install(page, pe)` helper plus a 3-line lazy-install
  block at the top of each of the six `memory_*_slow` functions (read/write
  × 8/16/32 bit). For 16/32-bit, the install is gated on "in-page" — the
  cross-page split falls through to the existing byte-by-byte decomposition,
  which lazy-installs via the 8-bit path on each side.

---

## 9. How the finding was measured

For posterity, the methodology:

1. Built a gprof-instrumented binary:
   `EXTRA_CFLAGS="-pg -fno-omit-frame-pointer" LDFLAGS_MODE="-pg" make -f Makefile.headless`
2. Ran a script that boots SE/30 + SSW 6.0.8 / 4 MB to a stable Finder
   (~75 M instructions) and quits.
3. `gprof -b build/headless/gs-headless gmon.out` — `mmu_invalidate_tlb`
   tops the flat profile at 37.4% self time.
4. Patched in four counters inside `mmu_invalidate_tlb` (total calls, fast
   exits, full-repopulate calls, pages-touched), dumped at process exit via
   a `__attribute__((destructor))`. Re-ran. Output:
   `calls=2467  fast=824  repop=821  pages_touched=860,880,896`.
5. `821 × 1,048,576 = 860,832,256` — confirms the dominant cost is the 1 M
   per-invalidate scan.

The Plus comparison comes from the boot-matrix integration test
(`tests/integration/boot-matrix/test.script`), which already reports MIPS for
each row.

---

## 10. Glossary

- **AoS — Array of Structures.** A flat array where each element is a struct
  with several fields. Good for code that touches several fields of one
  element together; bad for streaming one field across all elements.
- **SoA — Structure of Arrays.** Each field of the logical record lives in
  its own dense flat array. Good for streaming one field. Used by the
  emulator's fast-path lookup tables.
- **TLB — Translation Lookaside Buffer.** Hardware concept: a small cache of
  recent virtual→physical translations. In this codebase we use "software
  TLB" loosely to refer to the SoA arrays, which serve the same role.
- **PMMU.** The 68030's Paged Memory Management Unit. Walks guest-supplied
  page tables to translate logical addresses to physical ones.
- **CRP / SRP.** CPU/Supervisor Root Pointers — guest registers holding
  pointers to the root of the guest's user and supervisor page tables.
- **TC.** Translation Control register. Bit 31 (`TC.E`) enables/disables the
  PMMU. The Universal ROM toggles `TC.E` thousands of times during slot
  scanning, which is *the* reason `mmu_invalidate_tlb` is called so often.
- **Transparent Translation (TT0/TT1).** PMMU registers that identity-map
  large chunks of address space (8-bit-aligned ranges) without consulting
  the guest page tables. The Universal ROM uses TT1 to identity-map the
  supervisor view of low RAM.
- **g_page_table.** The AoS canonical map. One `page_entry_t` per 4 KB page
  of guest address space. Set up once at init.
- **g_supervisor_read / g_user_read / g_supervisor_write / g_user_write.**
  The four SoA arrays — the fast-path cache. Each entry is a pre-adjusted
  `uintptr_t` such that `entry + addr` is the host pointer for the byte.
- **g_active_read / g_active_write.** Per-sprint pointers selecting which of
  the two read/write SoA pairs (user vs supervisor) is currently active,
  based on the CPU's S-bit at sprint start.
- **g_tlb_track.** Side list of page indices currently non-zero in any SoA
  array. Used to make the *zero* phase of invalidation cheap. Caps at 8192
  with `memset` fallback on overflow.
- **Sprint.** The emulator's unit of CPU dispatch — a bounded chunk of
  instructions executed before the scheduler checks events. The SoA's
  active-pair pointers are swapped at sprint boundaries on S-bit change.

---

## 11. Pointers into the code

- `src/core/memory/memory.h:120-149` — `page_entry_t`, `g_page_table`, the
  four SoA externs, `g_active_read/write`.
- `src/core/memory/memory.h:271-318` — the inline hot-path read/write
  functions (the 4 lines from §4 live here).
- `src/core/memory/memory.c:572-612` — `memory_map_add`, where AoS entries
  are populated.
- `src/core/memory/memory.c:207-…` — `memory_read_uint8_slow` and friends;
  the slow path that handles devices, MMU misses, and unmapped accesses.
- `src/core/memory/mmu.c:24-50` — `g_tlb_track`, `tlb_track_page`.
- `src/core/memory/mmu.c:476-547` — `mmu_invalidate_tlb` (the function this
  whole document is about).
- `src/core/memory/mmu.c:549-…` — `mmu_handle_fault`, the lazy-install path
  used in the MMU-enabled regime.
- `src/core/cpu/cpu_68030.c:85-…` — PMOVE/PFLUSH/PTEST handling, where most
  calls to `mmu_invalidate_tlb` originate.
