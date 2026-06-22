# Granny Smith Architecture

Granny Smith is a Motorola 68000 Macintosh Plus–style emulator. Its primary
target is the browser, where it is compiled to WebAssembly using Emscripten. The
project currently supports two host variants:

1. **wasm-emscripten** — Browser target (WebAssembly via Emscripten)
2. **native-headless** — Native, headless build for testing (no UI;
   CLI/scriptable)

This document describes the high level architecture, the main subsystems, the
flow of execution, and the repository layout of the project.

## Design Goals

Granny Smith is built around two core design goals: simplicity and preservation.

**Simplicity** guides both implementation and user experience. The emulator
architecture is intentionally straightforward—CPU emulation is a pure
interpreter, with instruction decoding handled by a simple switch statement.
There are no advanced JITs or meta-tools; instead, we rely on modern compilers
and hardware performance to deliver a usable experience. For users, simplicity
means instant access: Granny Smith runs directly in the browser, with no
installation required. State is automatically checkpointed and preserved across
browser sessions, and the emulator natively supports classic Macintosh file
formats such as `.sit` and `.hqx`.

**Preservation** is the heart of the project. As all emulators, we aim to
safeguard the computer architecture and its software ecosystem for future
generations. To achieve this, the implementation is highly portable C code with
minimal dependencies, ensuring longevity and ease of maintenance. Comprehensive
documentation accompanies every emulated hardware device, preserving technical
details for posterity. All code and documentation are released as open source
under a permissive license, supporting long-term accessibility and community
contributions.

## Core and Frontend Integration

At its heart, the emulator core is intentionally headless and platform-agnostic.
It emulates the full memory and device state of a Macintosh, including video
RAM, but does not itself render graphics or provide a graphical user interface.
Instead, the core exposes a simple command-line shell for user interaction,
communicating via standard input and output streams. File system access is
performed using standard C99/POSIX primitives, ensuring portability and ease of
integration.

To present a user-friendly experience, the core is typically wrapped by a
platform-specific frontend. These frontends are responsible for visualizing the
emulated screen, handling user input, and providing enhanced access to emulator
features. For example, the browser-based frontend renders the Macintosh display
in a canvas, maps host keyboard and mouse events to the emulator, and may
surface shell commands through a graphical interface. Similarly, desktop or
headless builds can provide their own mechanisms for display, input, and
automation, all while relying on the same portable core logic.

This separation of concerns allows the emulator to be easily embedded in
different environments, from web browsers to native applications, while
maintaining a clean and testable architecture.

## Modularized Architecture

The emulator is architected as a collection of well-defined subsystems, commonly
referred to as "modules." Each module encapsulates its state using an opaque
data type, provides explicit constructor and destructor functions, and may
optionally implement a checkpoint serializer for state persistence. This modular
approach promotes encapsulation, testability, and code reuse.

Key subsystems (modules) include:

- **CPU** (`cpu`) — Motorola 68000 and 68030 emulation cores (the 68030 with integrated PMMU/FPU), template-instantiated from a shared decoder
- **Checkpoint** (`checkpoint`) — Checkpoint file I/O and serialization
- **Scheduler** (`scheduler`) — Event scheduling and timing
- **VIA** (`via`) — Versatile Interface Adapter (I/O controller)
- **SCC** (`scc`) — Serial Communications Controller
- **RTC** (`rtc`) — Real-Time Clock
- **Sound** (`sound`) — Audio output and buffer management
- **Keyboard** (`keyboard`) — Keyboard input handling
- **Mouse** (`mouse`) — Mouse input handling
- **Floppy subsystem** (`floppy`) — Floppy disk controller and drive logic
- **SCSI subsystem** (`scsi`) — SCSI bus and device emulation
- **Disk image handling** (`image`) — Disk image management and storage backends
- **AppleTalk integration** (`appletalk`) — Networking and AppleShare support
- **Object model** (`object`) — Typed substrate (classes, members, nodes, value type, path resolver, expression interpolator, alias table) used by the shell, JS bridge, scripts, and the inspector UI
- **Shell and CLI** (`shell`) — Line tokeniser, path-form dispatcher, tab completion, scripting; sits on top of the object model
- **Debugger** (`debug`, `debug_mac`) — Debugging and diagnostics tools

Each module is responsible for its own initialization, teardown, and (where
applicable) state serialization, following the conventions described in this
document.

### Module Pattern and Conventions

Each emulator subsystem is implemented as a self-contained module, following a
consistent pattern to maximize encapsulation, maintainability, and testability:

- **Opaque data types:**
  - Each module defines its state as a private struct in the `.c` file.
  - The header exposes only a forward declaration and a typedef, e.g.:
    - `struct cpu; typedef struct cpu cpu_t;`

- **Explicit construction and destruction:**
  - Modules provide a constructor named `<module>_init(...)` and a destructor
    `<module>_delete(...)`.
  - Constructors may accept dependencies (such as pointers to other modules)
    and, if supported, a `checkpoint_t` for state restoration.

- **Object-model class registration:**
  - Module-specific operations are exposed through a `class_desc_t`
    declared alongside the module's other code. The class names the
    path segment (`cpu`, `floppy`, `scsi`, …) and lists its members:
    typed attributes (`M_ATTR`), methods (`M_METHOD`), and child
    objects (`M_CHILD`, named or indexed).
  - The class is attached to the object root by the module's `_init`
    function (cfg-scoped subsystems) or by a dedicated
    `<module>_class_register` called from `shell_init`
    (process-singletons that don't need per-machine state).
  - Built-in `$reg` aliases that the module owns are registered in the
    same `_init` (`alias_register_builtin`).
  - The shell, the JavaScript / WASM bridge, scripts, tab completion,
    and the inspector UI all walk the same tree, so a new class is
    visible everywhere as soon as it's attached. There is no separate
    "command registry" or "JS API" layer to maintain in lock-step.
  - See [`docs/core/shell/object-model.md`](object-model.md) for the substrate
    and the conventions modules follow when adding a class.

- **Checkpointing (optional):**
  - Modules that support state serialization implement
    `<module>_checkpoint(<type> *self, checkpoint_t cp)`.
  - Checkpointing routines use `system_read_checkpoint_data` and
    `system_write_checkpoint_data` to efficiently load and save contiguous POD
    (plain old data) regions, serializing any dynamic buffers separately.
  -

### Object model and shell

Every emulator subsystem is exposed through a single typed tree rooted
at `emu`. Top-level paths are the subsystem names (`cpu`, `memory`,
`scc`, `via1`/`via2`, `rtc`, `scsi`, `floppy`, `sound`, `storage`,
`appletalk`, `mouse`, `keyboard`, `screen`, `debug`, `archive`,
`rom`, `vrom`, `machine`, `checkpoint`, `scheduler`, …). Each
subsystem's class lives next to its other code and self-registers via
`<module>_init`.

Four caller surfaces walk that tree:

- **Interactive shell** (terminal): `src/core/shell/` tokenises the
  line and translates the path forms (`path` / `path = value` /
  `path arg` / `path(args)`) to `node_get` / `node_set` / `node_call`.
- **Headless scripts**: same dispatcher, reading from a script file
  instead of the terminal. Integration tests in `tests/integration/`
  are scripts.
- **JavaScript / WASM bridge**: `gs_eval(path, args_json, out, size)`
  resolves the same path, JSON-encodes the result, and returns to JS.
  The web frontend reaches it through a single shared-memory region
  (`js_bridge_t` in [`src/platform/wasm/em.h`](../src/platform/wasm/em.h)),
  exposed via one `_get_js_bridge` WASM export. JS calls
  `gsEval(path, args)`; that puts the request in the bridge slot and
  parks on `Atomics.waitAsync(done)` until the worker's `shell_poll()`
  services it. C→JS state pushes (run-state, prompt) flow the other
  way through `Module.*` callbacks fired with `MAIN_THREAD_*_EM_ASM`.
  See [`web.md`](web.md) for the wire layout and protocol.
- **Inspector UI**: walks `objects()` / `attributes()` / `methods()` /
  `help()` to render the live tree.

There is no separate command framework, no parallel JS API. Adding a
new operation is one act — declare a member on the right class — and
every caller sees it.

See [`docs/core/shell/object-model.md`](object-model.md) for the substrate, the
path forms in detail, the lifecycle invariants (process-singleton vs.
cfg-scoped), and the recipe for adding a new class.

See [`docs/core/shell/shell.md`](shell.md) for the line-input layer specifically:
tokenisation, `$alias` expansion, `${expr}` interpolation, shell
variables, scripts, and tab completion.

### Orchestration and System Wiring

The orchestration layer, implemented in `system.c`, is responsible for
constructing, connecting, and managing the lifecycle of all emulator modules.
This layer acts as the central coordinator, ensuring that each subsystem is
initialized in the correct order, dependencies are satisfied, and cross-module
interactions are handled cleanly.

- **Global instance management:**
  - The global emulator state is referenced via a single pointer, defined in
    `system.c` and declared in `system.h`.
  - All other modules avoid global variables, instead receiving context through
    constructor parameters or referencing global state for read-only needs.

- **Lifecycle management:**
  - `setup_init()`: Performs one-time, machine-independent setup — log
    category and process-singleton class registration — run once at startup.
  - `system_create(const hw_profile_t *profile, checkpoint_t *)`: Allocates the
    `config_t`, wires the selected machine descriptor, and dispatches to
    `profile->substrate->init(cfg, cp)`; the machine's substrate constructs all
    modules in dependency order and optionally restores from a checkpoint.
  - `system_destroy(config_t *)`: Deletes the NuBus cards, calls
    `profile->substrate->teardown(cfg)` (the family delete-chain, reverse
    order), closes images, and frees the configuration.

- **Module interconnection via callbacks:**
  - Peripherals use function pointer callbacks for hardware signal routing,
    enabling multi-machine support without global functions. For example:
    - VIA accepts `via_output_fn`, `via_shift_out_fn`, and `via_irq_fn`
      callbacks during construction, used to control floppy drive selection,
      video buffer switching, sound lines, and interrupt routing.
    - SCC accepts a `scc_irq_fn` callback for interrupt routing.
  - These signal callbacks live in the owning machine or family file under
    `src/machines/` (e.g. `plus_via_output`/`plus_scc_irq`, or a GLUE family's
    shared `mac030_glue_via1_irq`) — not in `system.c` — and are passed to
    peripherals at construction, so machines route signals differently without
    changing peripheral code.
  - Machine *behavior* (init, teardown, reset, checkpoint, IRQ→IPL, VBL,
    NuBus-slot IRQ, and host floppy/keyboard/mouse/display input) is itself a
    vtable — `machine_substrate_t`, bound by the profile's `.substrate` field.
    `system.c` and `nubus.c` dispatch every per-machine action through it, so
    neither carries `if (model == …)` branches. See **Multi-machine support**
    below.

This design centralizes system wiring and cross-module logic, keeping individual
modules focused and decoupled, while ensuring accurate emulation of
hardware-level interactions.

### Dependency Injection and Accessor Functions

To reduce coupling between modules and enable testability, the emulator uses a
combination of explicit dependency injection and system accessor functions:

- **Explicit dependencies (preferred pattern):**
  - When a module needs another subsystem, it receives a pointer during
    construction and stores it internally. For example, the sound module
    receives a `memory_map_t*` pointer during `sound_init()` and stores it for
    later use.
  - This pattern makes dependencies explicit, supports testing with mock
    implementations, and avoids hidden global state access.

- **System accessor functions:**
  - For convenience and backward compatibility, `system.c` provides accessor
    functions that return pointers to core subsystems:
    - `system_scheduler()` — Returns the current scheduler
    - `system_memory()` — Returns the memory map (use `memory_map_interface()` to get read/write functions)
    - `system_cpu()` — Returns the CPU instance
    - `system_debug()` — Returns the debugger instance
    - `system_framebuffer()` — Returns the video RAM pointer
    - `system_is_initialized()` — Checks if the emulator is initialized
  - These accessors internally check the global emulator pointer and return
    NULL if the system is not initialized.
  - They are useful for shell command handlers and cross-cutting concerns
    (like logging) that need occasional access to shared state.

- **Input wrappers:**
  - `system_mouse_update(button, dx, dy)` — Routes mouse events to the device
  - `system_keyboard_update(event, key)` — Routes keyboard events to the device
  - These wrappers hide the global emulator reference from external callers.

This design keeps the `config_t` struct mostly opaque to external code while
providing controlled access to subsystems. The platform layer and core modules
can access what they need without depending on internal struct layout.

## Scheduler and Timing Model

The scheduler is the core component responsible for managing emulated time. It
tracks clock cycles, schedules time-based events on a single priority queue,
and runs the CPU in event-bounded sprints. See `docs/core/scheduler/scheduler.md` for the
detailed design and `docs/core/scheduler/timing.md` for the practical timing rules.

There are three execution modes (`schedule_max_speed`, `schedule_real_time`,
`schedule_hw_accuracy`) that control the cycles-per-instruction (CPI) ratio
and, on the WASM target, how many VBL frame-units run per host frame.

Both targets run the **same** model — a sequence of *VBL frame-units*
(`scheduler_run_frame`: pulse VBL, then run one VBL period) — and differ only
in **pacing**:

- **Headless** issues frame-units back-to-back, as fast as the host allows, with
  no host-time input. Result: a fixed `scheduler.run N` is byte-deterministic —
  same budget always produces the same output.
- **WASM** drives frame-units from `scheduler_main_loop` on the browser's render
  rhythm. Result: paced to the user's display refresh. The guest sequence is the
  same as headless; only how many frame-units a host tick batches differs.

See `docs/core/scheduler/scheduler.md` §10 for the full design.

## Checkpointing

Checkpointing enables the emulator to save its complete state to persistent
storage and later restore it, ensuring seamless continuity across sessions or
devices. Each subsystem that maintains state requiring preservation must
implement a `<subsystem>_checkpoint(...)` function to serialize its internal
state to a binary stream. Conversely, the `<subsystem>_init(...)` function is
responsible for restoring state from such a stream. The orchestration of
checkpointing—coordinating all modules and managing the overall process—is
handled by the "system" subsystem.

There are two primary types of checkpoints, each serving a distinct purpose:

- **Quick checkpoint:** Automatically triggered by the emulator, either at
  regular intervals or in response to specific events (such as a browser page
  reload or closure). Quick checkpoints are optimized for speed and may omit
  serializing state that already exists in persistent storage (e.g., modified
  disk image blocks), relying on the underlying file system to persist those
  changes. This approach allows for fast, frequent state saves without
  unnecessary duplication.

- **Consolidated checkpoint:** Explicitly created by the user to export the
  entire machine state into a single file or stream. In this mode, _all_
  emulator state—including any data normally left in persistent storage—must be
  fully serialized. Consolidated checkpoints are typically slower to create but
  ensure a complete, portable snapshot suitable for backup, transfer, or
  archival purposes.

By supporting both quick and consolidated checkpointing, the emulator balances
performance and reliability, providing robust state persistence for both
automated recovery and user-driven export scenarios.

### Per-machine state directory

Each machine owns a directory under `/opfs/checkpoints/<machine_id>-<created>/`
that is treated as one atomic unit. The quick checkpoint (`state.checkpoint`),
all writable image deltas and journals (`<id>.delta`, `<id>.journal`), and a
small informational `manifest.json` live together there. `/opfs/images/` holds
strictly read-only base content; nothing writable lands there any more.

`<machine_id>` is a 16-hex-char opaque token in `localStorage`; it rotates only
on explicit "new machine" actions and is pushed to the C side once per process
via `checkpoint --machine <id> <created>`. A startup sweep deletes any sibling
machine directories whose name does not match. See [`docs/core/storage/checkpointing.md`](checkpointing.md)
for the full design and [`docs/core/storage/image.md`](image.md) for the image-layer API
that backs it.

## Repository Layout

The repository is organized as follows:

- **src/** — Emulator source code
  - **core/** — Platform-agnostic emulator core (CPU, memory, *generic*
    devices, scheduler, etc.). Single-machine silicon (RBV, Egret, OSS, IOP,
    COPS, the Lisa FDC/ProFile, the built-in video cards) lives with its family
    under `src/machines/`, not here. The public machine descriptor + capability
    types are in `core/machine_profile.h` (the one machine header core may
    include).
    - **cpu/** — CPU emulation
      - _cpu.c_ — Lifecycle, public API, runtime dispatch
      - _cpu_68000.c_ — 68000 instruction decoder instantiation
      - _cpu_68030.c_ — 68030 decoder instantiation (integrated PMMU/FPU)
      - _cpu_internal.h_ — Shared struct and static inline helpers
      - _cpu_ops.h_ / _cpu_decode.h_ — Template-based decoder generation
      - _cpu_disasm.c_ — Disassembler
    - **memory/** — Page-table-based memory map (see `docs/core/memory/memory.md`)
    - **peripherals/** — VIA, SCC, SCSI, floppy, RTC, keyboard, mouse, sound
    - **scheduler/** — Event scheduling and timing
    - **debug/** — Debugger, logging, and diagnostics
    - **storage/** — Disk image, delta-file storage backend, and the
      `checkpoint_machine` module that owns the per-machine state directory
      (`/opfs/checkpoints/<machine_id>-<created>/`)
    - **network/** — AppleTalk and networking modules
    - **shell/** — Command framework (types, parser, symbol resolver, I/O
      capture, completion, JSON bridge, dispatcher)
  - **machines/** — Machine profiles, substrates, and chipset families
    (machine *implementations* — the platform-agnostic core must not include
    these; the public types live in `core/machine_profile.h`)
    - _machine.c_ — Static `const` profile registry + the `machine.*` object surface
    - _runtime/_ — Family-agnostic helpers (image + MMU checkpoint, host I/O)
    - _mac030/_ — Macintosh II-family 68030 substrate (lifecycle spine,
      table-driven I/O dispatch engine, ROM overlay, MMU register block)
    - _glue/_ — GLUE chipset family (se30, iicx, iix) + built-in SE/30 video
    - _mdu/_ — MDU+RBV chipset family (iici, iisi) + rbv, egret, built-in RBV video
    - _oss/_ — OSS+FMC chipset family (iifx) + oss, iop\*
    - _compact/_ — Compact-68000 substrate (plus; Mac SE assumed next)
    - _lisa/_ — Lisa segment-MMU substrate (lisa, macxl) + cops, lisa_fdc, lisa_profile
  - **platform/** — Platform abstraction layer (PAL)
    - **wasm/** — Emscripten/WebAssembly-specific implementation (browser
      target)
    - **headless/** — Native headless implementation (CLI/testing)
    - **stub/** — Minimal stubs for unit tests

- **app/** — Application frontends
  - **electron/** — Placeholder for future Electron desktop app
  - **web/** — Browser frontend (HTML, JS, CSS, xterm.js, etc.)

- **build/** — All build outputs (never edit)
  - **wasm/** — WebAssembly build artifacts (browser)
  - **headless/** — Native headless binary and outputs

- **tests/** — Test suites and data
  - **unit/** — Native unit tests (C, portable); see `docs/guide/TESTING.md` for architecture
  - **e2e/** — Playwright end-to-end (browser) tests
  - **integration/** — Headless integration tests (native CLI)
  - **data/** — Shared test data (ROMs, disk images, etc.)

- **third-party/** — External dependencies (e.g., peeler archive library)

- **docs/** — Reference documentation, mirroring the code tree: `guide/`
  (dev/process), `core/<subsystem>/` (mirrors `src/core/`), `machines/<family>/`
  (mirrors `src/machines/`), and `notes/` (dated investigation logs)

- **scripts/** — Build helpers, dev tools, and automation scripts

### Multi-machine support

The emulator runs nine models — `plus`, `se30`, `iicx`, `iix`, `iifx`, `iici`,
`iisi`, `lisa`, and `macxl` — on one shared core. Hardware is shared **by
subsystem**, not by cloning a file per machine, across three layers. (The
precedent is the NuBus card subsystem: a static descriptor that advertises its
own capabilities, a vtable of NULL-safe hooks, and an explicit registry.)

- **Tier 1 — generic chips** (`src/core/peripherals/`): the platform-agnostic
  silicon every family reuses — VIA, SCC, RTC, ADB, ASC, SWIM/IWM floppy,
  NCR-5380 SCSI, sound, the NuBus subtree. Single-machine chips do **not** live
  here.
- **Tier 2 — substrates** (`src/machines/<substrate>/`): a substrate owns a
  machine's lifecycle. Three exist: **`mac030`** (the Macintosh II-family 68030
  core — lifecycle spine, a table-driven `$50Fxxxxx` I/O dispatch engine, the
  ROM overlay, and the 68030 MMU register block), **`compact`** (the compact
  68000 Macs — Plus today, Mac SE assumed next), and **`lisa`** (the Lisa
  segment-MMU machines).
- **Tier 3 — chipset families** compose `mac030` as *siblings* (not as
  descendants of any one chipset): **GLUE** (`glue/` — se30/iicx/iix), **MDU+RBV**
  (`mdu/` — iici/iisi), and **OSS+FMC** (`oss/` — iifx). Each family supplies its
  own I/O window table, IRQ routing, and the few code hooks a data table can't
  express; the shared `mac030` engine drives all three.

**A machine is mostly data.** Each model is a `hw_profile_t` (defined in
`core/machine_profile.h`) holding identity, the CPU/MMU facts the init reads as
the single source of truth (`cpu_model`, `freq`, `address_bits`, `mmu_kind`,
RAM/ROM sizes), and the slot tables that shape the config dialog. It binds two
behavior pointers:

- **`.substrate`** — a `machine_substrate_t` lifecycle + host-input vtable
  (`init`/`reset`/`teardown`/`checkpoint_save`/`update_ipl`/`trigger_vbl`/
  `nubus_slot_irq`/`fd_*`/`input_*`/`display`), shared **per family**: all three
  GLUE machines bind the same `glue_substrate`, both MDU machines bind
  `mdu_substrate`, and the bespoke IIfx binds `iifx_substrate`. `system.c` and
  `nubus.c` dispatch through this vtable, so they hold no machine knowledge.
- **`.board`** — a typed-by-convention board descriptor (e.g.
  `mac030_glue_board_t`) the family substrate interprets: the ROM window, the
  I/O window table, NuBus slots, and per-machine hooks (VIA output, machine-ID
  strap, memory layout, optional checkpoint/VBL deltas). Adding a same-family
  machine is a new profile + board descriptor and one registry line — no new
  lifecycle code.

**Layout rule:** a chip lives with the narrowest scope that uses it — generic →
`core/peripherals/`; one family → that family's dir (`rbv`/`egret` → `mdu/`,
`oss`/`iop*` → `oss/`, `cops`/`lisa_fdc`/`lisa_profile` → `lisa/`, the built-in
video cards with their family); one machine → that machine's file. The core may
include the public `core/machine_profile.h` but **not** any machine
*implementation* header — a CI layering check enforces this
(`tests/integration/core-layering/`).

**Capability probe (no machine knowledge in the UI).** `machine.profile(id)`
returns a JSON map that includes a *derived* `capabilities` block
(`cpu.{model,address_bits,fpu}`, a **typed** `mmu.{present,kind}` —
`none`/`68030_pmmu`/`lisa_segment` — and `nubus`) plus a per-slot/per-card
`video_slots` block carrying each card's `requires_vrom`. The frontend probes
these instead of regex-matching the model's display name, so the debug panels,
the VROM prompt, and the slot labels all follow from data. Because capabilities
are *derived* from the facts, they can never drift from behavior.

The CPU is split into template-instantiated decoders (`cpu_68000.c`,
`cpu_68030.c`) sharing helpers via `cpu_internal.h`. The memory subsystem uses a
page-table architecture serving the static map of the 68000 machines and the
PMMU-rebuilt map of the 68030 machines alike. See `docs/core/memory/memory.md`
for details. Checkpoint I/O is factored into a standalone `checkpoint.c` module.
