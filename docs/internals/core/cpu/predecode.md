# Predecoded interpreter cores

The 68K and PowerPC cores each have two executors behind one entry point
(`cpu_run_68000/68030/68040`, `ppc_run`): the original big-switch decoder
(`cpu_run_*_switch`, the `while` loop in `ppc_run`) and the **predecoded
executor** this document describes.  `predecode.enabled` selects between
them at run time (default 1, with `predecode.elide` at 2); both are always
built, and the switch cores remain the reference that the differential
tests compare against.

Source: `src/core/cpu/predecode.[ch]` (the shared page pool),
`cpu_pd_ids.h` / `cpu_pd_classify.h` / `cpu_pd_run.h` (68K, instantiated by
`cpu_68000.c`, `cpu_68030.c`, `cpu_68040.c`), `ppc/ppc_pd_ids.h` /
`ppc/ppc_pd_classify.h` / `ppc/ppc_pd_run.h` (PowerPC, instantiated by
`ppc/ppc_run.c`), `scripts/gen_pd_cases.py` + `src/core/cpu/pdgen.mk` (the
generated T1 headers under `build/gen/`), and the code-page half of
`src/core/memory/memory.[ch]`.

## Contents

1. Responsibilities & design
   - 1.1 What it is, and what it is not
   - 1.2 Design rules
   - 1.3 Where it is used
2. Key types & files
   - 2.1 The entry
   - 2.2 The block
   - 2.3 The id space and the three tiers
   - 2.4 Files and how the templates are instantiated
3. Behaviour & algorithms
   - 3.1 The block pool: lookup, allocation, eviction
   - 3.2 Lazy decode
   - 3.3 The 68K classifier
   - 3.4 Generating T1 from the decode tree
   - 3.5 The 68K sprint loop
   - 3.6 68K handler conventions
   - 3.7 PC discipline, exceptions and bus errors (68K)
   - 3.8 Flag-liveness elision (68K)
   - 3.9 The PowerPC classifier
   - 3.10 The PowerPC sprint loop
   - 3.11 Coherence: code-page marks
   - 3.12 Invalidation and the look-back window
   - 3.13 Thrash demotion
   - 3.14 Host-page aliasing
   - 3.15 The refused-write record (68K MMU)
   - 3.16 The debug-build audit
4. Object-model / shell surface
5. Checkpointing
6. Testing
7. Known debts
8. See also
- Appendix A. Adding a shape
- References

## 1. Responsibilities & design

### 1.1 What it is, and what it is not

The predecoded executor is a **decode cache keyed by host page**.  The
switch executor decodes every instruction every time it runs: it fetches the
opcode, walks the decode tree (`cpu_decode.h` / `ppc_decode.h`), then
resolves the effective address from the raw bits.  The predecoded executor
does that work once per instruction position.  For every 4 KB guest page
that has executed, it keeps one 8-byte entry per possible instruction
start: 2048 entries for the 68K's 16-bit words, 1024 for the PowerPC's
32-bit words.  Each entry holds a handler id and the operands that handler
needs, already extracted.

It is **not** a JIT and **not** a basic-block cache:

- there is no generated machine code: every instruction still runs a C
  handler inside one flat `switch`;
- there are no block boundaries and no chaining: execution walks a cursor
  through the page's entry array and dispatches entry by entry;
- there is no cross-instruction scheduling.  The only cross-instruction
  analysis is 68K flag-liveness elision (§3.8), which looks at one
  successor.

The cache saves three things: the decode-tree walk, the effective-address
decode, and the fetch, which becomes an array index.  Everything the
handlers do to architectural state is the same as on the switch core.

### 1.2 Design rules

These invariants keep the two executors byte-identical (§6):

1. **One source of truth for decoding.** The classifier is the same decode
   tree used by the executor and the disassembler, instantiated a third
   time with each `OP_` macro rebound to "return an id" (§3.3, §3.9).  An
   instruction the tree calls illegal is illegal in the cache, by
   construction.
2. **One source of truth for semantics.**
   - A T1 entry runs the core's own `OP_` body (§3.4).
   - A T2 step runs the unmodified tree.
   - A T0 handler uses the same condition-code macros, memory accessors
     and restart rules as the switch core, so raw flag words, register
     roll-back on fault, and access order all match.
3. **The cache is derived state.** It is never checkpointed (§5).  Any
   doubt about a block (memory map change, elision-level change, a store
   into the page) ends with the block, or the affected entries, being
   dropped and decoded again.
4. **Coherence costs nothing on the fast path** (§3.11).  The memory layer
   takes away the write fast path for any page that has a block.  A store
   into code therefore always reaches a slow path, and that slow path
   invalidates the block.  The executor never compares memory against the
   cache in release builds.
5. **Sprint boundaries are exact.** Every place outside code can observe the
   CPU sees the same state as on the switch core: sprint exit, single step,
   breakpoints, logpoints, checkpoints, and exception frames.  The 68K last
   slot never elides (§3.8); the PowerPC last slot never folds (§3.10).

### 1.3 Where it is used

| Core | Predecoded? | Notes |
|---|---|---|
| 68000 (Plus, SE, Portable…) | yes | not when the Lisa segment MMU is installed (`g_lisa_mmu != NULL`), see §7 |
| 68020/030 (II family, SE/30, IIfx…) | yes | 030 PMMU: §3.5, §3.15 |
| 68040 (Quadra, 840AV) | yes | shares the 030 template (`CPU_DECODER_IS_68030` is set for both) |
| PowerPC 601 / 604 / 750 | yes | little-endian mode (MSR[LE]) runs on the generic tier (§3.10) |
| auxiliary cores (DSP3210, …) | no | outside `cores.md`'s main-CPU seam |

## 2. Key types & files

### 2.1 The entry

```c
typedef struct pd_entry {
    uint16_t id; // handler id; PD_UNDECODED (0) = not yet decoded
    uint8_t a;   // per id: dst register byte offset / length / CR bit or field
    uint8_t b;   // per id: src register byte offset / quick value / length / shift
    uint32_t c;  // per id: imm32 / sign-extended disp / in-page target index /
                 //         absolute target / raw word(s)
} pd_entry_t;    // 8 bytes
```

The meaning of `a`, `b`, `c` is fixed **per id**: the classifier that emits
an id and the handler that consumes it agree on the layout.
- **Registers by byte offset.** `a` and `b` hold the offset of a register
  inside `cpu_t` / `ppc_t`, not its number.  A handler reaches a register
  with `*(uint32_t *)((uint8_t *)cpu + off)` (`PD_R`, `PPD_R`), with no
  index arithmetic and no D/A split.
  - On the 68K, `d[8]` and `a[8]` must be contiguous and below byte 256.
  - On the PowerPC, `gpr[]` must be the first member.
  - Static asserts in the classifiers enforce both.
- **Variable lengths in a field.** On the 68K, shapes whose instruction
  length is not fixed by the shape (absolute and immediate operands) store
  the length in `a` or `b` instead of a register offset.  The handler's
  length expression (`PD_LEN_S`, `PD_LEN_D`, `PD_LEN_MV`) knows which
  field to read.
- **Packed `c`.** When both operands of a 68K instruction need a 32-bit
  constant, `c` holds two 16-bit halves:
  - MOVE `(d16,An)` to `(d16,An)`;
  - `#imm.B/W` to `(d16,An)`;
  - ADDI and similar with a `(d16,An)` destination.

  Combinations that would need more than 32 bits stay on T1.
- **Branch targets.** A branch target in the same page is stored as an
  entry index (an `_IN` id); any other target is stored as an absolute
  guest address (an `_OUT` id).

### 2.2 The block

```c
typedef struct pd_block {
    uint8_t *host;        // key: host page pointer; NULL = free
    uint32_t guest_lo;    // guest address the entries were decoded at
    uint32_t region;      // code region index (memory.h)
    uint32_t page;        // page index within that region
    uint32_t seq;         // allocation sequence number
    uint32_t writes;      // stores into the page this window (thrash detector)
    uint32_t write_window;// lookup count when the window opened
    uint32_t execs;       // instructions retired from the page this window
    uint32_t enter_budget;// *instructions when the executor entered this block
    uint32_t arch;        // PD_ARCH_68K / PD_ARCH_PPC
    bool thrashed;        // demotion requested; acted on at the next lookup
    pd_entry_t e[2048 + 1];          // +1: the PD_PAGE_END sentinel
    union { uint16_t raw16[2048];    // 68K raw words
            uint32_t raw32[1024]; }; // PPC raw words
} pd_block_t;            // 20,536 bytes on a 64-bit host
```

- **`e[]` holds the entries.**
  - A PowerPC block uses the first 1024.
  - The entry just past the last one is always `PD_PAGE_END`, so straight-
    line execution off the end of the page hits a control id instead of
    indexing out of bounds.
- **`raw16` / `raw32` are the raw shadow:** the guest words each entry was
  decoded from.  They serve two purposes:
  - the 68000 latches `ir` from them (§3.6);
  - the debug build audits every dispatched entry against live memory
    through them (§3.16).
- **The block is keyed by host page, not by guest address.**
  - Two guest mappings of the same RAM share one block; §3.14 covers the
    exception.
  - A guest page that the MMU remaps to different host bytes finds a
    different block.
  - The executor never needs a guest-to-block hash: it already knows the
    host pointer from the fast-path read table (68K) or the fetch window
    (PowerPC).

### 2.3 The id space and the three tiers

Each architecture has its own 16-bit id space, laid out the same way:

| Range | Tier | Meaning |
|---|---|---|
| 0 `PD_UNDECODED` | control | never decoded, or invalidated: decode now, then dispatch |
| 1 `PD_CROSS` | control | the instruction straddles the page end (68K only): one generic step, fetched through memory |
| 2 `PD_GENERIC` | control | a shape the predecoded path never handles (the 68K FPU leaf, a tree epilogue): one generic step |
| 3 `PD_PAGE_END` | control | the sentinel past the last entry: no instruction ran; look up the next page |
| 4..15 | control | reserved |
| 16 .. `T1_END-1` | **T1** | one id per distinct `OP_` leaf of the decode tree (generated, §3.4).  The entry carries the raw word(s); the handler is the core's own `OP_` body |
| `T1_END` .. | **T0** | specialized shapes: the entry carries pre-extracted operands and the handler is a few loads, an ALU op and a store |
| — | **T2** | not an id: the "generic step", which runs one instruction through the unmodified decode tree with the switch core's fetch.  Control ids `PD_CROSS` / `PD_GENERIC`, unknown ids, and pages with no block run here |

Current sizes:

| | 68K | PowerPC |
|---|---|---|
| T1 leaves | 313 | 216 |
| T0 ids | family list in `cpu_pd_ids.h`; roughly 2,000 ids, mostly MOVE (6 destination families × 7 source shapes × 2 × 3 sizes) | 82 (`PPC_PD_T0` in `ppc_pd_ids.h`) |

**68K T0 families** (`PD_FAMILIES` in `cpu_pd_ids.h`).  A family is one
operation at one size over a fixed list of operand shapes.  The shapes are:

| Shape | `PD_SH_*` | Operand |
|---|---|---|
| 0 | `D` | Dn, or An used as a value |
| 1 | `IND` | `(An)` |
| 2 | `INC` | `(An)+` |
| 3 | `DEC` | `-(An)` |
| 4 | `D16` | `(d16,An)` |
| 5 | `ABS` | `(xxx).W`, `(xxx).L`, or `(d16,PC)` resolved to an absolute address |
| 6 | `IMM` | `#imm` |

There are four family kinds:

| Kind | Slots | Used for |
|---|---|---|
| `S7P` | 7 source shapes × 2 (pair) | ea,Dn ALU, TST, CMPA, MULU/MULS, MOVE (one family per destination shape) |
| `S7S` | 7 source shapes × 1 | MOVEA, ADDA/SUBA, DIVU/DIVS |
| `D6P` | 6 destination shapes × 2 | Dn,ea and #imm,ea read-modify-write, ADDQ/SUBQ, CLR |
| `P1` / `S1` / `N(n)` | 2 / 1 / n | register-only unaries, shifts, bit ops, Bcc by condition, control flow, MOVEM, Scc, A-line, TRAP |

In a **pair**, the even id is the full-flags handler and the odd id
(`id + 1`) is its **no-flags twin** (§3.8).  The classifier computes
`family + stride * shape`, adding 1 for the twin.  The executor stamps its
`case` labels from the same family list, so ids and cases cannot drift.

`g_cpu_pd_prop[id]` is built by `cpu_pd_prop_init()` (`cpu.c`) from the
family list.  It holds per-id property bits that the elision pass reads:

| Bit | Meaning |
|---|---|
| `PD_P_WNZVC` | writes N, Z, V, C unconditionally and reads none of them (an *overwriter*) |
| `PD_P_CANFAULT` | may reach memory, raise or trap before its flags are written |
| `PD_P_ELIDABLE` | has a no-flags twin at `id + 1` |
| `PD_P_TWIN` | *is* the no-flags twin of `id - 1` |
| `PD_P_MEMDEF` | the twin touches memory: E2 only (§3.8) |

Control and T1 ids carry only `PD_P_CANFAULT`, the conservative setting:
they are never overwriters and never elided.

**PowerPC T0** (`PPC_PD_T0` in `ppc_pd_ids.h`):

| Group | Ids |
|---|---|
| Register arithmetic and logic | XO/X-form register ALU (`add`, `subf`, `mullw`, `neg`, `and`, `andc`, `or`, `nor`, `xor`, `slw`, `srw`, `extsb`, `extsh`, `cntlzw`, `srawi`), each with an `_RC` twin for the `Rc=1` form |
| Immediates | D-form immediates (`addi`, `li`, `mulli`, `ori`, `xori`, `andi.`, `addic`, `addic.`, `subfic`) |
| Rotates | `rlwinm`, `rlwimi`, `rlwnm` (raw word in `c`) |
| Compares | `cmpwi`, `cmplwi`, `cmpw`, `cmplw` |
| Branches | `b`, `bl`, `bdnz`, `bc` true/false (each `_IN` / `_OUT`), `blr`, `bctr` |
| SPR and CR moves | `mflr`, `mtlr`, `mfctr`, `mtctr`, `mfxer`, `mtxer`, `mfcr`, `mtcrf` |
| CR logic | the eight CR logical ops, `mcrf` |
| Sync and cache | a shared `NOP` (`sync`, `eieio`, `icbi`, `dcbt`, `dcbtst`, `dcbst`, `dcbf`), `isync` |

Loads and stores are **T1** on the PowerPC: their exception paths (DSI,
alignment) need the raw word, and the T1 path already avoids the decode.
On the PowerPC, a T0 `_RC` id sits next to its base id like a 68K pair, but
it is the architectural record form, not an elision twin.

### 2.4 Files and how the templates are instantiated

| File | Role |
|---|---|
| `predecode.h` / `predecode.c` | entry/block types, control ids, the pool (lookup, allocation, eviction, invalidation, demotion), statistics, the debug audit, the `predecode` object node |
| `cpu_pd_ids.h` | the 68K id space: generated T1 ids, `PD_FAMILIES`, the shape enum, the property bits |
| `cpu_pd_classify.h` | the 68K classifier: `cpu_decode.h` with every `OP_` returning an id |
| `cpu_pd_run.h` | the 68K one-instruction executor (T2), lazy decode with the elision pass, operand macros, operation bodies, case stamping, and the sprint loop |
| `cpu.c` | `cpu_pd_prop_init` (the property table) and `cpu_pd_id_name` (names for `predecode.hist`) |
| `ppc/ppc_pd_ids.h`, `ppc/ppc_pd_classify.h`, `ppc/ppc_pd_run.h` | the same three roles for the PowerPC |
| `ppc/ppc_internal.h` | `g_ppc_fetch.blk`: the block for the fetch window's page |
| `scripts/gen_pd_cases.py`, `src/core/cpu/pdgen.mk` | generate `build/gen/{cpu,ppc}_pd_t1_{ids,classify,cases,names}.h` from the decode trees |
| `memory.h` / `memory.c` | code regions, marks, `memory_write_fill`, `memory_host_written`, the refused-write record, `g_mem_slowpath_count` |

`cpu_pd_run.h` is a **template**: each 68K core file includes it once,
after its own `cpu_ops.h` bindings and its switch executor.  The includer
defines:

```c
#define PD_RUN_NAME      cpu_pd_run_68030      // the sprint function
#define PD_STEP_NAME     cpu_pd_step_68030     // the one-instruction executor (T2)
#define PD_DECODE_NAME   cpu_pd_decode_68030   // lazy decode + elision pass
#define PD_TREE_NAME     cpu_pd_tree_68030     // the classifier's tree instantiation
#define PD_CLASSIFY_NAME cpu_pd_classify_68030 // the classifier entry point
#define PD_HW_RESET(c)   ...                   // the double-bus-fault reset
#include "cpu_pd_run.h"
```

The template then includes `cpu_decode.h` twice more:
- once as the T2 step: the tree unchanged, wrapped in `do { } while (0)`
  so a body's `continue` still means "abandon";
- once, at the very end, as the classifier, through `cpu_pd_classify.h`.

The classifier goes last because it `#undef`s and redefines every `OP_`
name; nothing after it may execute an op.  Each core therefore contains
four instantiations of the one tree: the switch loop, the T2 step, the
classifier, and the T1 cases inside the predecoded loop (§3.4).

The public entry point dispatches at run time:

```c
void cpu_run_68000(cpu_t *restrict cpu, uint32_t *instructions) {
    if (predecode_enabled() && g_lisa_mmu == NULL)
        cpu_pd_run_68000(cpu, instructions);
    else
        cpu_run_68000_switch(cpu, instructions);
}
```

`ppc_run.c` does the same with `ppc_pd_run`.

## 3. Behaviour & algorithms

### 3.1 The block pool: lookup, allocation, eviction

`predecode_lookup(host_page, guest_lo, arch)` is the only way an executor
obtains a block.  It returns NULL in five cases:
- predecode is disabled;
- the host page lies in no **code region**;
- the page is demoted and still held (§3.13);
- the page is being demoted by this very lookup;
- allocation failed.

On NULL, the caller runs the generic tier until the next page transition.

Code regions are registered by the memory layer (`g_mem_code_regions`, at
most 4):
- region 0 is the selected memory map's flat RAM+ROM image, re-registered
  by `memory_map_select`;
- unit tests register their own buffers.

Device windows, card VRAM and declaration ROMs are in no region and always
run generic.  Pages are **region-relative**: the image is not 4 KB-aligned
on the host, so "host page" always means "4 KB page of a region".

The per-region tables are created lazily, sized to the region:
- **`g_pd_slots[r][page]`**: page → block;
- **`g_pd_demoted[r][page]`**: the lookup count until which the page is
  held on the generic tier;
- **`g_pd_demote_count[r][page]`**: how many times the page has been
  demoted (the hold backs off with it).

Lookup, in order:

1. **Generation check.** If `g_mem_map_generation` changed since the pool
   last reset, call `predecode_reset()`.  A new machine, a map rebuild or a
   checkpoint restore all bump the generation, so blocks never outlive the
   image they were decoded from.
2. `g_pd_stats.lookups++`.  This counter is the pool's clock: the thrash
   window and the demotion hold are measured in lookups.
3. Find the region and page; no region → `lookup_noregion`, return NULL.
4. **A block exists:**
   - if it is `thrashed`, demote it (§3.13) and return NULL;
   - if its `guest_lo` differs from this lookup's, re-decode it for the new
     alias (§3.14);
   - otherwise return it.
5. **No block:** if the page's hold has not expired, count `lookup_held`
   and return NULL.
6. **Allocate** (`block_take`):
   - below `pool_cap`, allocate a fresh block;
   - at the cap, take the next block in round-robin order over the whole
     pool and detach it from its page (`evictions++`).

   Round-robin evicts the least recently *allocated* page, not the least
   recently used.  Hot code pages allocate once and stay, so the eviction
   order rarely matters.
7. Initialise the block:
   - clear every entry to `PD_UNDECODED`;
   - set the sentinel;
   - record `guest_lo`, `region`, `page`, `arch`;
   - open a fresh thrash window;
   - install it in the slot;
   - **mark the page as code** (`memory_code_page_mark`, §3.11).

Detaching a block (eviction, demotion, reset) clears the slot, **unmarks
the page** (its write entries refill lazily), and clears `g_pd_current` if
the block was current.

Memory budget: a block is 20,536 bytes.  At the default `pool_cap` of 2048
the pool tops out near 40 MB, allocated block by block as pages execute.
Typical Mac OS boots stay well below the cap.  Setting `pool_cap` frees the
whole pool.

### 3.2 Lazy decode

A fresh block is all `PD_UNDECODED`.  The loop decodes an entry the first
time execution reaches it, as the `case PD_UNDECODED` arm of the dispatch
switch, then re-dispatches the same entry with its new id.  Only executed
positions are ever classified:
- data interleaved with code costs nothing;
- a jump into the middle of a 68K instruction simply decodes another entry
  at that word.

On the 68K, `PD_DECODE_NAME(cpu, blk, idx, page_lo)`:

1. Calls the classifier for word `idx` with `avail = 2048 - idx` words
   readable in the page.  It gets back the id, the fields, and (T0 only)
   the length in words.
2. Copies the words the entry was decoded from into `raw16[]`: `len` words
   for T0, and at least 2 (the opcode and first extension word) for T1.
3. Runs the elision pass (§3.8), which may change the id to the twin.
4. Stores the entry and counts the decode (`predecode.decodes`, the
   per-id histogram behind `predecode.hist`).

On the PowerPC, `ppc_pd_decode` reads the word, classifies it, stores the
word in `raw32[idx]`, and stores the entry.  There is no elision pass.

### 3.3 The 68K classifier

`cpu_pd_classify.h` instantiates `cpu_decode.h` as a function that returns
`uint16_t`.  Every `OP_` name starts out as `return T1_<leaf>`; these
defaults are generated (§3.4).  The file then redefines the leaves it
specializes, so they return a T0 id when the operands fit the shape and the
T1 id otherwise.  The tree's own epilogue returns `PD_GENERIC`, as does the
FPU leaf (it computes its own EAs from `cpu->pc`).

The classifier reads instruction words straight from the host page; it
never goes through the memory accessors.  Its entry point:
- returns `PD_CROSS` if fewer than 2 words remain (`avail < 2`): the opcode
  is in the page's last word, and the instruction is fetched through memory
  by the generic step;
- otherwise loads the opcode and first extension word, stores them in the
  entry (`c = opcode`, `a:b = ext_word`), and walks the tree.

The tree reads the 68030's MOVES direction bit through `CPU_MOVES_DIR()`.
The executing decoders read it at `cpu->pc`; the classifier overrides it to
read the extension word it was handed.

**Operand resolution** (`pd_resolve`) turns an EA (mode, register, size,
extension-word offset) into a `pd_shape_t`, or **declines**.  It declines
when:
- the mode is not in the leaf's allow-mask (`PD_ALLOW_DN/AN/MEM/ABS/PCREL/IMM`);
- the operand is `(An)+` or `-(An)` at byte size with A7 (the 2-byte A7
  step is not built into the handlers);
- the mode is `(d8,An,Xn)`, a full-format extension, or `(d8,PC,Xn)`;
- the operand's extension words run past the end of the page.

The last rule matters: **a T0 entry never consumes a word outside its page.**
Only T1 entries read words beyond the page, and they read them through
memory at run time.

A declined resolution makes the leaf return its T1 id: the shape runs the
generic leaf body, already decoded but without pre-extracted operands.

Resolution rules worth knowing:
- `(d16,PC)` becomes `ABS`: the classifier adds the extension word's own
  guest address (`page_lo + 2 * (idx + pre + 1)`) to the displacement, so
  the entry holds an **absolute** address.  This is why the block records
  `guest_lo` (§3.14).
- `(xxx).W` is sign-extended into `c`.
- An immediate is stored in `c` at its operand size (bytes masked to 8 bits).
- Byte-size sources never accept An (the architecture does not allow it).
- `#imm,ea` with a long immediate and a `(d16,An)` destination stays T1:
  the immediate and the displacement cannot share `c`.  Absolute
  destinations of `#imm,ea` stay T1 for the same reason.
- MOVE with both operands needing `c` is T0 only for the packed forms
  (§2.1).  MOVE with both lengths variable (ABS/IMM source and ABS
  destination) stays T1, since only one length field is free.
- Branches (`pd_cls_bcc`, `pd_cls_dbcc`) compute the target
  `ipc + 2 + disp`.  An even target inside the page becomes an `_IN` id
  with the entry index in `c`.  Anything else (odd, or another page)
  becomes `_OUT` with the absolute target.
- Model-dependent allow-masks (`PD_TST_ALLOW`, `PD_CMPI_ALLOW`) follow what
  each model's tree accepts.  For example, the 68020+ allows `TST An` and
  `TST #imm`; the 68000 does not.
- On the 030/040 the tree routes `EXTB.L Dn` through the LEA leaf (mode 0,
  register field 4); the classifier's `OP_LEA_EA_AN` splits it back out.

### 3.4 Generating T1 from the decode tree

A decode tree invokes one object-like `OP_` macro per leaf, nested inside
`switch` statements.  The predecoded loop needs a **flat** switch over
those leaves.  Re-including the nested tree would not give one, because a C
`case` binds to the innermost enclosing switch.

`scripts/gen_pd_cases.py` (run by `pdgen.mk` whenever the tree or the
script changes) scans the tree for every `OP_[A-Z0-9_]+` token, sorts the
set so ids are stable across builds, and writes four headers that agree by
construction:

| Header | Content |
|---|---|
| `<p>_pd_t1_ids.h` | `enum { T1_<name> = 16, …, T1_END }` |
| `<p>_pd_t1_classify.h` | `#undef OP_<name>` / `#define OP_<name> return T1_<name>`: the classifier defaults |
| `<p>_pd_t1_cases.h` | `case T1_<name>: PD_T1_BODY(OP_<name>); break;` |
| `<p>_pd_t1_names.h` | the leaf names by id, for `predecode.hist` |

The cases header is included **inside the predecoded loop's switch, after
the core's own `OP_` bindings**.  Each T1 case therefore runs exactly the
body the core binds to that leaf, so a T1 instruction is decode-free but
otherwise identical to the switch core.  `PD_T1_BODY` binds the names the
leaf bodies expect, then runs the body:

| | `PD_T1_BODY` binds |
|---|---|
| 68K | `opcode` from `c`, `ext_word` from `a:b`, `instruction_pc = ipc`, `pc = ipc + 2`, plus the `PD_ENTER(2)` prologue |
| PowerPC | `iw` from `c`, `instruction_pc = ipc`, `pc = ipc + 4` |

After the body, the loop takes the relookup path (§3.5, §3.10).

A new leaf in either tree gets a T1 id and a `case` without any edit to the
predecoded code.

### 3.5 The 68K sprint loop

`PD_RUN_NAME(cpu, instructions)` keeps the sprint ABI of the switch core
(`cores.md`):

- `*instructions` is the budget: the number of instructions this sprint
  may retire.
- The memory layer can force an early exit by zeroing it through
  `g_bus_error_instr_ptr`, which happens on an I/O penalty or a bus error.
- At return it is 0.

**Loop state:**

| Variable | Meaning |
|---|---|
| `blk` | the block of the page being executed; NULL on the generic tier |
| `cur` | the entry to dispatch next |
| `page_lo` | the guest address of `blk`'s page |
| `ipc` | the guest address of the instruction being dispatched (`page_lo + 2 * (cur - blk->e)`) |
| `pd_held` | the current page was declined by the pool: stay generic without re-asking until the PC leaves it |
| `pd_slow` | the switch core's post-fault user-mode tracking is armed (`last_bus_error_pc != 0 && !supervisor`): every instruction takes the generic step, whose prologue does that tracking |

**Prologue** (the switch core's, minus the loop):
- if `cpu->halted` is set (a double bus fault), run `PD_HW_RESET`;
- on the 030/040, select the active SoA tables for the current mode;
- `cpu_check_interrupt`;
- publish `g_bus_error_instr_ptr`;
- on the 030/040 with trace (T1) set, clamp the budget to one instruction.

Then jump to `relookup`.

**`top`:** dispatch one entry.

1. If the budget is 0, go to `done`.
2. Compute `ipc`.  If `pd_slow`, count `generic_slowmode` and take the
   generic step.
3. Decrement the budget **before** dispatch.
4. Load the entry.  If this is the **last slot** (the budget is now 0) and
   the id is a twin, step back to the full-flags id (§3.8).
5. `switch (id)`:
   - **`PD_UNDECODED`:** decode (§3.2), re-apply the last-slot rule,
     re-dispatch.
   - **`PD_CROSS` / `PD_GENERIC`** (and `default`, an id the executor does
     not stamp): give the slot back (the T2 path takes it after its fetch),
     set `cpu->pc = ipc`, go to `t2_step`.
   - **`PD_PAGE_END`:** give the slot back, set `cpu->pc = ipc`, go to
     `relookup`.  No instruction ran.
   - **T0:** run the handler.  It ends with one of:
     - `PD_NEXT(len)`: `cur += len`, back to `top`;
     - `PD_JUMP_IN(index)`: `cur = e + index`, back to `top`;
     - `PD_JUMP_PC(target)`: `cpu->pc = target`, go to `relookup`;
     - an exception, which sets `cpu->pc` to the vector and goes to
       `relookup`.
   - **T1:** run the leaf with `cpu->pc` materialized; it falls out of the
     switch to `relookup`.

**`t2_step`** (the generic tier) is the switch core's per-instruction
prologue, verbatim:
- on the 68000, mask the PC to 24 bits and raise an address error on an
  odd PC;
- fetch the opcode and extension word with `memory_read_prefetch32`;
- set `instruction_pc`;
- on the 68000, latch `ir`;
- do the `last_bus_error_pc` bookkeeping;
- `pc += 2`, take the slot;
- run `PD_STEP_NAME`, the unmodified tree;
- go to `relookup`.

**`relookup`** is the only place a block pointer is derived from an
address:

1. Take the PC (24-bit masked on the 68000) and its bus address
   `pc & g_address_mask`.  The fast-path tables are sized to the mask, so a
   stray PC above the map must not index past them.
2. Re-derive `pd_slow`.
3. **Same page, same mapping:** if the PC is even, still in
   `[page_lo, page_lo + 4K)`, and
   `g_active_read[bus >> 12] + (page_lo & mask) == blk->host`, set
   `cur = blk->e + (pc - page_lo)/2` and go to `top`.  No memory access.

   The mapping comparison matters on the 030/040.  An `RTE` or a
   `MOVE to SR` that drops to user mode swaps the active table, and A/UX
   maps the same logical page differently in the two spaces.  A
   `PMOVE`/`PFLUSH` run through the generic step can remap the page in
   place.
4. If the page is held (`pd_held`, same page), go straight to the generic
   step.
5. **New page:**
   - read the page's fast-path read entry;
   - if it is nonzero, call `predecode_lookup(base + page, page_lo,
     PD_ARCH_68K)`; a NULL result sets `pd_held` and counts
     `relookup_nopool`;
   - if it is zero (MMU page not walked yet, or a device), count
     `relookup_nomap` and take one generic step: its fetch fills the entry,
     and the next relookup finds the page.
6. `predecode_enter(blk, *instructions)` charges the instructions retired
   since the last transition to the block being left (§3.13).
7. If there is a block, go to `top`; otherwise take the generic step.

**`done`** (epilogue):
- materialize `cpu->pc` from the cursor.  The 68000 keeps a control
  transfer's full 32-bit target in the register until the next prologue
  masks it, as the switch core does;
- set `instruction_pc = ipc`;
- `predecode_enter(NULL, …)`;
- then the switch core's epilogue:
  - a pending bus error is raised as `exception_bus_error`, or
    `exception_bus_error_retry` for a PMMU fault on the 030/040; the 68000
    variant sets `pc = instruction_pc + 2` unless it is an address error;
  - a pending trace exception is raised (030/040);
  - `cpu_check_interrupt`.

The 68K checks interrupts only at sprint entry and exit, exactly as the
switch core does.  There is no per-instruction interrupt poll on the 68K.

### 3.6 68K handler conventions

Every handler is stamped by `PD_CASE(ID, MEM, LEN, BITS, BODY)`:

```c
case ID: {
    uint32_t _len = LEN;
    PD_ENTER(_len);          // ir latch, debug audit, slow-path snapshot
    PD_MAT_IF(MEM, _len);    // materialize instruction_pc/pc if the shape touches memory
    BODY;
    PD_NEXT(_len);
}
```

- **`PD_ENTER(words)`** does three things:
  - on the 68000 only, latches `cpu->ir` from `raw16[]` and sets
    `ir_pc = ipc`.  The Lisa OS reads `ir` from the group-0 frame, and the
    checkpoint carries it;
  - runs the debug audit over `words` words (§3.16);
  - snapshots `g_mem_slowpath_count` into `_sp0` for the E2 guard (§3.8).
- **`PD_MAT_IF(MEM, len)`** sets `instruction_pc = ipc` and
  `pc = ipc + 2 * len` when the shape touches memory.  Register-only
  handlers do not materialize at all; the exit does it from the cursor.
- **Source loads by shape** (`PD_LD_<SH>`) follow `calculate_ea` +
  `read_ea_*`.  `(An)+` and `-(An)` move the register before the read, and
  a faulting read leaves it moved, as the switch core does.
- **Destination stores by shape** (`PD_ST_<SH>`) follow `write_ea_*`:
  - a bus error already pending from an earlier access of the same
    instruction aborts before the data cycle;
  - `(An)+` and `-(An)` restore An when the write faults or was aborted.
- **Read-modify-write** shapes (`PD_RMW_<SH>`) read without updating An;
  the store does the update.
- **MOVE source roll-back:** for `(An)+` / `-(An)` sources, MOVE snapshots
  An and restores it if any fault is pending after the store
  (`PD_SAVE_SRC_*` / `PD_RESTORE_SRC_*`), matching the switch core's MOVE
  restart rule.
- **Condition codes** use the switch core's own macros (`UPDATE_C_ADD`,
  `UPDATE_NZ_CLEAR_CV`, `GENERIC_SUB`, …), so the raw flag words are
  bit-identical, which checkpoint equality requires.
- **The stack** (`PD_PUSH32`) uses `cpu_ops.h`'s write-first order: write
  at `SP - 4`, then update SP only if no fault is pending.
- **MOVEM** uses the staged, restart-safe loops of `cpu_internal.h`
  (`pd_movem_to_regs`, `pd_movem_from_regs`, `pd_movem_from_regs_predec`).

Branches:
- `Bcc` has one case per condition and per in/out (`PD_BCC_CASES`).  An
  `_IN` case that is taken does `PD_JUMP_IN(e.c)` and never leaves the
  loop or touches `cpu->pc`.  An `_OUT` case does `PD_JUMP_PC(e.c)` through
  relookup.
- `DBF` / `DBcc`, `BSR`, `JSR`, `RTS`, `RTD`, `JMP` follow the same split.
  Anything that pushes a return address or reads the stack materializes
  the PC first (`PD_MAT`).
- `ATRAP` and `TRAP` materialize, raise through the switch core's exception
  helpers, and go to relookup.

### 3.7 PC discipline, exceptions and bus errors (68K)

The architectural `cpu->pc` / `cpu->instruction_pc` are **not** updated per
instruction.  While a block is current, the cursor *is* the PC.  The
registers are written only where something can observe them:

| Where | What is written |
|---|---|
| T0 handlers whose shape touches memory | both, before the first access (`PD_MAT_IF`) |
| handlers that push a return address, read the stack, or raise | both (`PD_MAT`) |
| T1 bodies | both, before the leaf runs |
| `_OUT` branches, generic steps, `PD_CROSS`/`PD_GENERIC`/`PD_PAGE_END` | `cpu->pc` (the target or `ipc`) |
| sprint exit (`done`) | both, from the cursor and `ipc` |

An exception raised **inside** a handler therefore sees the same
`instruction_pc` / `pc` pair as on the switch core.

**Bus errors.**  The memory layer handles a fault the same way for both
executors:
1. it sets `g_bus_error_pending` and zeroes `*instructions` through
   `g_bus_error_instr_ptr`;
2. the handler finishes under the store rules above, so the data cycle is
   skipped and An is restored;
3. the loop sees a zero budget at `top` and goes to `done`;
4. the epilogue raises the exception from `instruction_pc = ipc`.

An **I/O penalty** zeroes the budget the same way without a fault, so the
sprint ends right after the instruction that touched the device.

### 3.8 Flag-liveness elision (68K)

Most 68K instructions write the condition codes [1], and most of those
writes are dead: the next flag-writing instruction overwrites them before any
instruction reads them.  `predecode.elide` controls how far the executor
exploits that:

| Level | Rule |
|---|---|
| 0 | none: every handler writes the flags the hardware writes |
| 1 (E1) | a definer whose NZVC result is dead on the sequential path runs its **no-flags twin**, but only definers that do not touch memory (`!PD_P_MEMDEF`) |
| 2 (E2, default) | E1, plus memory-form definers, whose twins carry the slow-path guard |

**The decision** is made at decode, in `PD_DECODE_NAME`.  Entry `i` with id
`id`, length `len` and properties `p = g_cpu_pd_prop[id]` becomes `id + 1`
when all of the following hold:

1. the level is above 0, `p` has `PD_P_ELIDABLE`, and `len != 0` (`i` is
   T0);
2. at level 1, `p` lacks `PD_P_MEMDEF`;
3. the successor `j = i + len` lies in the same page;
4. the successor's **classified** id (the classifier is called again for
   `j`; this does not depend on whether `j` itself was elided) has
   `PD_P_WNZVC` and lacks `PD_P_CANFAULT`.  In other words, `j` writes all
   four of N, Z, V, C, reads none of them, and cannot fault or trap before
   writing them.

Because T1 and control ids carry only `PD_P_CANFAULT`, a T1 successor
never lets its predecessor elide.  The property bits are declared once per
family in `cpu_pd_prop_init`, next to a reviewable list of names.

Not overwriters:
- the bit operations write only Z;
- `ADDX`/`SUBX`/`NEGX`, `ABCD`/`SBCD`, `ROXL`/`ROXR` read X or Z (and are
  T1 anyway);
- `Scc`, `Bcc`, `DBcc` read the condition.

**The twin's body** is the same macro instantiated with flag-liveness
argument `FL = 0`.  The compiler folds the `if (FL)` blocks away:
- **X is still written wherever the architecture writes X.**  For example,
  `PD_OP_ADD`'s no-flags arm computes `CC_X = (r < d)`.  X is not part of
  NZVC, and the overwriter rule says nothing about it.
- **Pure-flag ops** (TST, CMP, CMPA, CMPM) still perform their operand
  reads (address updates, faults and device side effects all happen), but
  skip the compare.

**Where dead flags could still be observed**, and the rule that closes
each case:

| Observer | When | Rule |
|---|---|---|
| sprint exit, single step, breakpoint, logpoint, checkpoint, an interrupt taken at the boundary | the budget runs out after `i` | **the last slot never elides**: at dispatch, if the budget just reached 0 and the id has `PD_P_TWIN`, `id - 1` (the full-flags handler) runs instead.  The same check follows a lazy decode |
| exception raised by `j` before `j` writes the flags | `j` can fault | the decision requires `!CANFAULT(j)` |
| the sprint ends **during** `i` (an I/O penalty or a fault in `i`'s memory access zeroes the budget) | `i` touches memory | **the E2 guard**: the twin of a memory shape is instantiated with `FL = PD_FLM`, i.e. `g_mem_slowpath_count != _sp0`.  Every slow path increments that counter, and only a slow path can end a sprint from inside an instruction, so the twin computes the flags after all exactly when the sprint might end on it |
| trace | always | trace limits the sprint to one instruction, so this is the first row |

The write-only forms (MOVE, CLR) normally set the flags *before* the store.
For a guard-armed twin the flags must instead be computed *after* the
store, once it is known whether a slow path ran.  `PD_FL_FULL(FL)` (true
only for the literal 1) selects the first order and `PD_FL_GUARD(FL)` (a
runtime `FL`) the second.  An earlier `(FL) == 1` test let a guard that had
fired (`PD_FLM` is 1 then) pass for the constant and skip both; the
`__builtin_constant_p` split fixes that.

With these rules CCR is architecturally exact at every observation point,
and a checkpoint taken after any sprint is byte-identical to the switch
core's at every level.  The predecode-diff rows check this at levels 0/1/2
(§6).

**Re-decoding:**
- setting `predecode.elide` resets the pool, because entries carry
  decisions made at the old level;
- a store anywhere in `j`'s words invalidates `i` too (the look-back
  window, §3.12), so a decision is never made on stale successor words.

The PowerPC executor has no elision.  `Rc` forms are separate ids, and CR0
writes are cheap enough that the analysis would not pay.

### 3.9 The PowerPC classifier

`ppc_pd_classify.h` instantiates `ppc_decode.h` with every `OP_` returning
an id.  The defaults are generated as on the 68K; the overrides cover the
T0 shapes in §2.3.  The tree's own validity checks [3] decide what is illegal:
reserved fields, invalid BO encodings, `if (PPC_RB(iw)) { OP_ILLEGAL; … }`.

**Model gating** (`M601()`, `M604()`) stays in the handlers.  Ids are
model-independent.  The pool is reset when the model changes: a new
machine selects a new memory map, and the generation bump resets it (§3.1).

The entry point sets `a = b = 0` and `c = iw` before walking the tree, so
every T1 entry carries its raw word.  A PowerPC instruction is always one
in-page word, so there is no `PD_CROSS`.

Shape rules:
- **XO-form** with `OE = 1`: T1 (overflow handling stays in the generic
  body).
- `Rc` selects the `_RC` id (`id + PPC_RC(iw)`).
- **`addi` / `addis` with `rA = 0`** become `LI` (the architecture's
  "load immediate" reading of `rA = 0`).  `addis`, `oris`, `xoris`,
  `andis.` reuse the base id with the immediate pre-shifted by 16.
- **Rotates** keep the raw word in `c`.  The handler re-derives
  `SH`/`MB`/`ME` at run time, which is cheap and keeps the entry small.
- **`b`:**
  - absolute (`AA`) or relative target;
  - `LK` selects `BL_*`;
  - an in-page target becomes an `_IN` index.
- **`bc`:** only three BO shapes are T0, with no `LK`.  Every other BO
  (CTR-and-condition combinations, `bcl`) stays T1.
  - `BO & 0x1E == 0x10` → `bdnz`;
  - `BO & 0x1E == 0x0C` → branch if `CR[BI]` is set;
  - `BO & 0x1E == 0x04` → branch if `CR[BI]` is clear.

  `a = 31 - BI`, so the test is `(cr >> a) & 1`.
- **`bclr` / `bcctr`:** only `BO = 20` (branch always) without `LK` (`blr`,
  `bctr`).  The rest keep `ppc_do_bclr` / `ppc_do_bcctr`, including the
  601's decrement-then-fetch-the-old-CTR behaviour.
- **`mfspr` / `mtspr`:** only LR, CTR and XER are T0.  Every other SPR
  stays T1 for its privilege and side-effect checks.
- **`mtcrf`:** the CRM field is expanded to a 32-bit mask in `c` at decode.
- **Cache and sync no-ops** share `PPD_NOP`.  `isync` is its own id
  because it calls `ppc_context_sync`.

### 3.10 The PowerPC sprint loop

`ppc_pd_run` follows the same structure as the 68K loop.  The differences
come from the switch loop it mirrors.

**The fetch window decides where the PC runs from.**  `g_ppc_fetch` (the
switch loop's one-page fetch window: `lo`, `span`, `host_adjust`) gains
`blk`, the block for the window's page.  At sprint start `blk` is cleared,
so it is re-validated once per sprint against demotions and logpoints.

**Interrupts are polled at every instruction boundary**, as in the switch
loop.  At `top`, if `ext_irq` or `dec_pending` is set and `MSR[EE]` is
set, the loop materializes the PC, calls `ppc_poll_interrupt`, and goes to
relookup.

**The budget is retired after the instruction**, in `retire` /
`retire_relookup`, not before.  This keeps the switch loop's fold rule
intact:
- a branch handler sets `p->fold` exactly where `ppc_do_b/bc/bclr/bcctr`
  would.  The fold covers taken branches other than a branch to itself,
  and not-taken `bc`/`bdnz`, which still fold;
- the retire tail then **keeps the slot** for a folded branch, unless it is
  the last slot (`*instructions > 1` is required) or the fold budget
  `folds_left` (4 × the sprint budget) is exhausted.  Two branches folding
  into each other must not spin a sprint forever;
- `PPD_JUMP_PC` always folds.  A branch out of the page can never target
  itself.

**T0 handlers do not materialize the PC.**  None of them can fault:
loads, stores and every exception-raising leaf are T1.  `PPD_CASE` is
audit + body + `cur++` → `retire`.  T1 bodies set `instruction_pc` and
`pc = ipc + 4` and run the leaf, then go to `retire_relookup`.

**`t2_step`** is the switch loop's iteration, verbatim:
1. set `instruction_pc`;
2. `ppc_fetch`.  An ISI goes to relookup at the vector; a bus error goes
   to `done`;
3. `pc += 4`;
4. `ppc_execute`;
5. go to `retire_relookup`.

**`relookup`:**

1. **Same page:** if the PC is word-aligned and still in the page, and the
   window still stands behind the block (`span == 4K`, `lo == page_lo`,
   `g_ppc_fetch.blk == blk`), set `cur` and go to `top`.

   Several things flush the window (`ppc_mmu_flush_fetch`), because the
   same logical page may now fetch from another physical page:
   - an `rfi` or `mtmsr` that changes IR or PR;
   - a TLB invalidation;
   - setting `MSR[LE]`.

   The switch loop re-resolves on every fetch; this check is its
   equivalent.
2. **Leaving the block:**
   - `predecode_enter(NULL, …)` charges the page being left;
   - if the budget is 0, go to `done`;
   - from here `p->pc` is authoritative.
3. If the PC is aligned and **`MSR[LE]` is clear**:
   - **Refill the window.** If the PC is outside the window, call
     `ppc_fetch_fill` right here (an ftlb hit for a page seen before).
     This avoids a generic step for the common page transition.  An ISI or
     a fetch bus error is handled as in `t2_step`.
   - **Use the window's block.** If the window is a full page:
     - check that `g_ppc_fetch.blk` still holds this page (`host` and
       `guest_lo` match; the pool may have recycled it);
     - otherwise look the page up;
     - enter the block and go to `top`.
4. Otherwise take one generic step, polling interrupts first.  This covers
   an unaligned PC, little-endian mode, a device window (`span = 0`), a
   logpointed page, and a demoted page.

**Little-endian mode** (MSR[LE], 604 and 750) munges every fetch (XOR 4).
It is rare, so it runs entirely on the generic tier, whose fetch applies
the munge.  Setting the bit flushes the window, so no block outlives the
switch into LE.

**`done`:**
- materialize `pc` from the cursor and set `instruction_pc = ipc`;
- `predecode_enter(NULL, …)`;
- deliver a pending bus error as a **machine check**, as the switch loop
  does (the 604 variant also clears `SRR1[RI]` and `MSR[ME]`).

### 3.11 Coherence: code-page marks

A block is valid as long as its page's words are the words it was decoded
from.  The memory layer guarantees that without a check on any fast path
(`memory.md`, "Code-Page Coherence", has the full writer inventory):

1. **Marking.** When the pool allocates a block it calls
   `memory_code_page_mark(host_page)`:
   - this sets a per-page byte in the code region's `marks[]`;
   - it zeroes every **write** SoA entry, supervisor and user, that
     currently maps to that host page.

   To find the logical aliases, the reverse scan walks only the 256-page
   chunks flagged in `g_mem_soa_chunk` (chunks where a write entry was
   ever planted), not the whole 1M-entry tables.
2. **One planter.** Every site that plants a write entry goes through
   `memory_write_fill(page, host, adjusted, tables)`.  It flags the chunk
   and **returns 0 for a marked page**, so the refill leaves the store on
   the slow path.  The sites are:
   - `rebuild_soa_page`, `memory_populate_pages`, `memory_populate_ram_mirror`;
   - the 030 PMMU fill, the 68040 fill, the PowerPC `user_soa_fill`;
   - the PDM / TNT / Gossamer / Mac II glue.

   Read entries are untouched, so **fetches and loads stay on the fast
   path**.
3. **Stores reach the slow path, which notifies.**  `write_slow_n` calls
   `code_write_notify(host, len)` **before** the store lands.  If the
   target is marked, this counts `g_mem_code_write_count` (exposed as
   `predecode.suppressed_writes`) and calls `g_mem_code_written_hook`,
   which is `predecode_invalidate_host`.  The store then completes through
   the host pointer as usual.
4. **Direct host writers notify themselves.**  DMA engines, debug pokes,
   the PowerPC HTAB R/C write-back, and `machine.restart` all call
   `memory_host_written(host, len)`.  It splits the range per region page
   and calls the hook for marked pages.
5. **Unmarking.** Eviction, demotion and reset unmark the page.  Its write
   entries come back lazily on the next slow-path refill.

The hook is installed by `predecode_object_install()`.  Unit tests that
never install the node get it installed on the first lookup.

Consequences the executors rely on:

- **A store from inside the current block** goes through the slow path,
  which invalidates the affected entries (§3.12) before the store lands.
  The executor re-reads `*cur` on every dispatch, so it never runs a stale
  entry: an invalidated entry reads `PD_UNDECODED` and is decoded afresh
  from the new words.
- **Memory map changes** (`memory_map_select`) re-register the code regions
  and bump `g_mem_map_generation`.  The pool resets lazily on the next
  lookup (§3.1), so a rebooted or restored machine never runs blocks
  decoded from the old image.

### 3.12 Invalidation and the look-back window

`predecode_invalidate_host(host, len)` locates the block for the page (if
any) and resets entries to `PD_UNDECODED`.  The range is the entries
covering `[off, off + len)`, extended **backwards**:

| Arch | Look-back | Why |
|---|---|---|
| 68K | 11 entries (`PD_INVALIDATE_BACK_68K`) | an instruction may start up to 10 words before the changed word (the longest 68020+ instruction is 11 words), and an entry's elision decision depends on its successor's words |
| PowerPC | 1 entry (`PD_INVALIDATE_BACK_PPC`) | |

It counts `invalidations` if anything was reset, then feeds the thrash
detector (§3.13).  The block itself stays in place even if the executor is
running from it: only its entries change.

### 3.13 Thrash demotion

Some pages mix code with hot data:
- a stack that briefly held a trampoline;
- a data page a handler jumped through;
- a system-heap page that holds patches next to the structures they work
  on.

Every store to such a page is a slow-path access plus an invalidation and a
re-decode, which makes it slower than the switch core.  The pool detects
these pages and **demotes** them back to the generic tier, which restores
their write fast path.

**Measuring execution.**  The executors call
`predecode_enter(blk, *instructions)` at every block transition and at
sprint exit.  The call adds `enter_budget - budget` to the block being left
(`execs`) and records the current budget on the block being entered.  The
current block is `g_pd_current`.

**The rule**, applied at each invalidating store into a block's page:

1. If more than `thrash_window` lookups have passed since the window
   opened, open a fresh window: zero `writes` and `execs`, and re-base the
   current block's `enter_budget`.
2. `execs` is the block's retired instructions this window, plus the
   current visit's if the block is current.  The current visit is read
   through `g_bus_error_instr_ptr`, and only while that block is current:
   between sprints the pointer is stale.
3. If `++writes > thrash_limit` **and** `writes * thrash_ratio > execs`,
   set `thrashed`.

The reasoning behind the rule: a suppressed store costs about
`thrash_ratio` instructions' worth of predecode gain.  A hot loop that
stores into its own page retires many instructions per store and is kept.
A data page that is executed occasionally is dropped quickly.

**Acting on it.**  The block stays in place until the next
`predecode_lookup` of its page (the executor may be running from it
right now).  That lookup:
- detaches the block, which unmarks the page;
- records a hold of `demote_hold << min(n, 8)` lookups, where `n` is the
  number of earlier demotions of that page.  The hold doubles with each
  demotion, because a page that keeps coming back and thrashing is a data
  page;
- counts `demotions`;
- returns NULL.

Lookups during the hold return NULL (`lookup_held`).  The 68K loop
remembers this in `pd_held` and stays generic until the PC leaves the page.

Defaults: `thrash_limit = 32`, `thrash_window = 4096` lookups,
`thrash_ratio = 64`, `demote_hold = 65536` lookups.
`predecode_set_thrash_ratio(0)` disables the rule (the conformance suites
use it, §6).

### 3.14 Host-page aliasing

A block is keyed by host page but **decoded at one guest address**:
- `(d16,PC)` operands and `_OUT` branch targets are stored as absolute
  guest addresses;
- `_IN` targets are page-relative but only valid within the same alias.

The same host page can execute through another alias:
- the 840AV's ROM overlay at address 0;
- a 24-bit mirror;
- a second BAT mapping;
- an MMU mapping of one physical page at two logical addresses.

When that happens, `predecode_lookup`'s `guest_lo` differs from the
block's.  The pool then clears every entry, re-sets the sentinel, adopts
the new `guest_lo`, and counts `realiases`.  Two aliases executing in
alternation thrash the block's entries but stay correct.

### 3.15 The refused-write record (68K MMU)

With the 030/040 PMMU enabled, the code-page marks have a guest-visible side
effect unless it is compensated.  The switch core keeps a live write entry
for a page it has already translated, so a store there is an ATC hit and
never walks the tables [2].  With the page marked, the same store reaches the
slow path, and an unmodified slow path would call `mmu_handle_fault` and
walk.  A walk sets the descriptor's U and M bits and updates MMUSR.  The
guest can see both: A/UX ages its pages by U, and a divergent U bit led to
a kernel panic on the IIfx.

The memory layer therefore records **exactly the write entries the marks
took away**.  It keeps one bit per page and write table (`MEM_WT_SUPER`,
`MEM_WT_USER`), per memory map.

| Event | Bit |
|---|---|
| `memory_write_fill(..., tables)` refuses an entry because the page is code | set for `tables` |
| `memory_code_page_mark` zeroes an existing entry (`zero_write_aliases`) | set for that table |
| `mmu_invalidate_tlb` (after the ATC flush), the 68040's invalidate, a physical logpoint install | all cleared (the switch core loses its entries there too) |
| an MMU fill that leaves the page read-only | cleared for that page |

In `write_slow_n`'s MMU branch, if the page's bit is set for the current
mode and its read entry is nonzero, the store **completes through the read
entry's host pointer** with no walk, and therefore no U/M or MMUSR change.
It notifies the cache first, as every store into code does.  If the page
is no longer code (its block was evicted or demoted since), the write entry
the switch core would hold is re-planted and the bit is cleared.

While predecode marks no page, nothing ever sets a bit.

The PowerPC needs a different fix for user mode, `user_phys_fallback`
(`memory.md`).  Its user tables are filled logically, and a refused fill
falls back to a physical index, which must not alias another page's slot.

### 3.16 The debug-build audit

In `GS_DEBUG` builds, `PD_AUDIT_68K(blk, idx, words)` (in `PD_ENTER`) and
`PD_AUDIT_PPC(blk, idx)` (in every PowerPC case and T1 body) compare the
dispatched entry's raw shadow against live memory.  On the first mismatch,
`predecode_audit_fail` prints the cached and live words and aborts.  A
mismatch means some writer bypassed the marks, typically a DMA engine
missing its `memory_host_written` call.

Release builds compile the audit to nothing.  The design's correctness
argument is §3.11; the audit is how a missing writer gets found.

## 4. Object-model / shell surface

The `predecode` node is a root sibling of `machine` (order 22, next to
`pacing`, in the Advanced category).  Its counters are Advanced-only.

| Path | Kind | Meaning |
|---|---|---|
| `predecode.enabled` | RW | 1 = predecoded executors, 0 = switch cores (also `machine.cpu.predecode`) |
| `predecode.elide` | RW | 68K elision level 0 / 1 / 2; setting it resets the pool |
| `predecode.pool_cap` | RW | maximum blocks (16..65536, default 2048); setting it frees the pool |
| `predecode.thrash_limit` | RW | stores per window before the ratio rule applies (32) |
| `predecode.thrash_window` | RW | the window, in pool lookups (4096) |
| `predecode.thrash_ratio` | RW | demote when stores × ratio exceed the instructions retired in the window (64; 0 disables) |
| `predecode.demote_hold` | RW | base hold, in pool lookups, of a demoted page (65536; doubles per repeat demotion) |
| `predecode.blocks` | RO | blocks currently holding a page |
| `predecode.lookups` / `allocs` / `evictions` | RO | pool activity |
| `predecode.decodes` / `invalidations` / `demotions` / `elided` / `realiases` | RO | §3.2, §3.12, §3.13, §3.8, §3.14 |
| `predecode.suppressed_writes` | RO | stores that reached a marked page (`g_mem_code_write_count`) |
| `predecode.generic_steps` | RO | instructions run through the generic step, split into `generic_cross`, `generic_declined` and `generic_slowmode`; the remainder are steps on pages with no block |
| `predecode.relookup_nomap` / `relookup_nopool` | RO | page transitions that found no fast-path read entry / that the pool declined (split into `lookup_noregion`, `lookup_held`) |
| `predecode.hist([top])` | method | print the most-decoded ids per architecture with names and share (consumes the counts) |
| `predecode.reset()` | method | drop every block and zero the counters |

Typical uses:

```
predecode.enabled = 0                    # A/B a row against the switch core
predecode.hist 30                        # which shapes does this workload decode?
echo ${predecode.generic_steps} ${predecode.demotions}   # how much falls back, and why
```

## 5. Checkpointing

Nothing in this subsystem is checkpointed.  Blocks, marks, the
refused-write record and the counters are all derived from guest memory and
the memory map:
- **Save:** writes nothing.  The guest state is identical between the two
  executors at every sprint boundary (§1.2 rule 5), which is what makes a
  checkpoint written by one executor loadable by the other.
- **Restore:** `memory_map_init` / `memory_map_select` re-register the code
  regions and bump `g_mem_map_generation`.  The next lookup resets the
  pool, and blocks rebuild on demand.
- `predecode.enabled` and the tunables are host settings, not machine
  state.

## 6. Testing

| Test | What it checks |
|---|---|
| `tests/unit/suites/cpu_predecode` | the 68K executor in isolation: a basic loop, a differential run against the switch core, self-modifying code, the invalidation look-back, `memory_host_written`, page-crossing instructions, entry into the middle of an instruction, a logpointed page, eviction, demotion, the page-end sentinel, the generation reset; `PD_BENCH=1` adds a loop microbenchmark |
| `tests/unit/suites/memory_codepage` | the memory half: marks and every refill site, the reverse alias scan, the refused-write record |
| every CPU unit suite with `CPU_TEST_PREDECODE=1` (`CPU_TEST_ELIDE=<level>`) | the 68000 single-step vectors (236,266), the 030/040 suites, the `m68k_vectors` and `ppc_vectors` conformance replays (m68k-test / powerpc-test vectors with unlisted state randomized), the PowerPC, PowerPC FPU and PowerPC MMU suites.  `m68k_vectors` runs every vector through **both** executors and fails on any difference.  These suites set `thrash_ratio = 0`: every vector rewrites its own page and retires about one instruction, which the thrash rule would otherwise demote to the generic tier, leaving the T0/T1 handlers untested |
| CI | runs the unit suites with the predecoded default and again with `CPU_TEST_PREDECODE=0` |
| `tests/integration/predecode-diff` (matrix tier) | the differential definition of "same guest timeline": boot each machine with predecode off and on (at each elision level), checkpoint at fixed instruction counts chosen to land mid-boot, and compare guest state block by block (`scripts/cmp-checkpoints.py`, which masks host pointers and storage bookkeeping).  Rows: Plus (68000), IIcx (68030 + PMMU), 840AV (68040, the ROM-overlay alias), 6100 (601), 7500 (604) |
| `tests/integration/perf-pdm`, the floors on `suite-pdm` / `tnt-hd-boot` / `ans-aix-boot` | throughput floors |
| `tests/e2e/web2-specs/perf-predecode.spec.ts` | the in-browser A/B (`PERFBENCH` lines, sanity asserts only) |
| every other integration and e2e test | runs predecoded, since that is the default |

To run the differential locally:
`make -C tests/integration test-predecode-diff` (`ROW=<name>` limits it to
one row).

When the two executors diverge on a full boot, bisect by instruction count:
1. checkpoint both runs at a series of counts; the first count whose
   checkpoints differ brackets the divergence;
2. narrow it with `scheduler.run N` + `machine.cpu.*` reads, or with a trace
   build;
3. with the switch core's state as the oracle, the first differing
   instruction is the bug.

The RTC is reseeded from the host clock at `machine.boot`, so pin
`machine.rtc.time` after the boot (as the predecode-diff scripts do), or
start both runs at the same moment.

## 7. Known debts

- **The Lisa runs the switch core.**  Its segment MMU bypasses the SoA:
  every access, fetch included, goes through `lisa_resolve`.  There is no
  host-pointer fast path to hang blocks on, and no write entries to
  suppress.  A Lisa fast path would need a `lisa_resolve`-based relookup
  and a suppression hook inside `lisa_mmu_write*`.
- **PowerPC little-endian mode** runs on the generic tier (§3.10).
- **No PowerPC T0 loads or stores.**  They are T1 (decode-free but not
  operand-specialized).  A T0 form would have to reproduce the DSI and
  alignment paths exactly.
- **No PowerPC elision.**
- **68K elision is single-successor and in-page.**  A definer whose
  overwriter is two instructions away, on the next page, or T1, keeps its
  flags.
- **The thrash rule is a heuristic** tuned on Mac OS boots.  It costs
  coverage in synthetic workloads that rewrite their own page every
  instruction; the conformance suites switch it off.
- **Eviction is round-robin by allocation**, not LRU.  It has not mattered
  at the default cap.
- **Generic-tier residue in the 68K conformance replay.**  Even with the
  thrash rule off, a few percent of `m68k_vectors`' instructions run on the
  generic step: page-crossing instructions (`PD_CROSS`) and `PD_GENERIC`
  leaves (the FPU, and opcodes the tree's epilogue rejects).  They are
  still compared against the switch core, but through the shared tree
  rather than a predecoded handler.

## 8. See also

- `docs/internals/core/cpu/cores.md`: the core-module contract, the sprint
  ABI, and where the predecoded executor sits in it.
- `docs/internals/core/memory/memory.md`, "Code-Page Coherence": the
  writer inventory and the PowerPC user-mode fallback.
- `docs/internals/core/cpu/ppc.md`: the fetch window, folding, and the
  601/604/750 models.
- `docs/internals/core/scheduler/scheduler.md`: sprints, budgets and I/O
  penalties.
- `docs/internals/core/object/object-model.md`: the root-node table (order
  22).

## Appendix A. Adding a shape

1. **Declare it.**
   - On the 68K, add the family to `PD_FAMILIES` with its slot count, and
     extend `cpu_pd_prop_init` if the family needs property bits that its
     name prefix does not imply.  A wrong `PD_P_WNZVC` or `PD_P_CANFAULT`
     bit is an elision bug.
   - On the PowerPC, add the id (and its `_RC` twin) to `PPC_PD_T0`.
2. **Teach the classifier to emit it.**  Override the tree's `OP_` name in
   `cpu_pd_classify.h` / `ppc_pd_classify.h`.  Return the T1 id whenever the
   operands are not what the handler assumes: the A7 byte step, an operand
   past the page, an `OE` form, a BO shape the handler does not implement.
3. **Write the handler.**
   - On the 68K, use `PD_CASE` and the shape macros.  For an elidable
     operation, write the body once against `FL` and let the pair macros
     (`PD_S7P`, `PD_D6P`, `PD_PAIR`, `PD_MV_SIZE`) stamp both ids.
   - On the PowerPC, use `PPD_CASE` / `PPD_PAIR`.
   - Use the switch core's flag macros and accessors, never local
     re-implementations.
4. **Verify.**
   - Run every CPU suite both ways (`CPU_TEST_PREDECODE=0/1`, and
     `CPU_TEST_ELIDE=0/1/2` on the 68K).
   - Run the predecode-diff rows.
   - Check `predecode.hist` to confirm the new id is actually decoded.
   - A T0 handler is correct when nothing can observe the difference from
     T1.

## References

1. Motorola, *M68000 Family Programmer's Reference Manual* (M68000PM/AD,
   Rev. 1, 1992): condition-code semantics per instruction (§3.8), effective
   addressing modes (§3.3).
2. Motorola, *MC68030 Enhanced 32-Bit Microprocessor User's Manual*
   (MC68030UM/AD, 3rd ed., 1990): ATC and table-walk U/M updates, MMUSR
   (§3.15).
3. IBM / Motorola, *PowerPC Microprocessor Family: The Programming
   Environments for 32-Bit Microprocessors* (MPCFPE32B/AD, Rev. 1, 1997):
   BO encodings, record forms, MSR[LE] (§3.9, §3.10).
