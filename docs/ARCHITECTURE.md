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

- **CPU** (`cpu`) — Motorola 68000 emulation core (with 68030 stub for IIcx)
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
- **Shell and CLI** (`shell`) — Command-line interface and scripting
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

- **Command registration:**
  - Module-specific shell commands are registered within the module’s
    constructor using `register_cmd`.
  - Commands that span multiple modules are registered centrally in `system.c`
    to avoid cross-module coupling.

- **Checkpointing (optional):**
  - Modules that support state serialization implement
    `<module>_checkpoint(<type> *self, checkpoint_t cp)`.
  - Checkpointing routines use `system_read_checkpoint_data` and
    `system_write_checkpoint_data` to efficiently load and save contiguous POD
    (plain old data) regions, serializing any dynamic buffers separately.
  -

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
  - `setup_init()`: Performs one-time setup, including registration of log
    categories and module-owned commands, as well as cross-module command
    registration.
  - `setup_plus(checkpoint_t)`: Allocates and initializes the `config_t`
    structure, constructs all modules in dependency order, and optionally
    restores state from a checkpoint.
  - `setup_teardown(config_t *)`: Tears down all modules in reverse order and
    frees the global configuration.

- **Module interconnection via callbacks:**
  - Peripherals use function pointer callbacks for hardware signal routing,
    enabling multi-machine support without global functions. For example:
    - VIA accepts `via_output_fn`, `via_shift_out_fn`, and `via_irq_fn`
      callbacks during construction, used to control floppy drive selection,
      video buffer switching, sound lines, and interrupt routing.
    - SCC accepts a `scc_irq_fn` callback for interrupt routing.
    - `trigger_vbl(config)`: Toggles VIA vertical blanking lines and invokes
      `sound_vbl()` for audio output.
  - Machine-specific callbacks (e.g., `plus_via_output`, `plus_scc_irq`) are
    defined in `system.c` and passed to peripherals during construction. This
    allows different machines to route signals differently without changing
    peripheral code.

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

The scheduler is the core component responsible for managing emulated time and
synchronizing it with real-world (host) time. It tracks clock cycles, schedules
time-based events, and ensures that the emulator's internal timing model matches
the intended behavior of the original hardware.

The scheduler supports three distinct modes, each defining a different
relationship between emulated time and host time:

**Fast mode:**

- Emulation runs as quickly as possible, with no attempt to synchronize with
  real time.
- Emulated time and host time are fully decoupled.
- Hardware events (such as VBL interrupts) may occur much more frequently than
  on real hardware, and instructions may execute in fewer cycles per event.
- This mode is useful for performance testing or fast-forwarding through
  uninteresting periods.

**Strict mode:**

- Emulated time is advanced according to the timing characteristics of the
  original hardware, regardless of host performance.
- Host and emulated time remain decoupled, but the emulator carefully models
  instruction timing and event scheduling to match real hardware as closely as
  possible.
- This mode is ideal for accuracy and compatibility testing, ensuring that
  software behaves as it would on a real Macintosh.

**Live mode:**

- Emulated time is actively synchronized with host wall-clock time.
- The scheduler aligns VBL interrupts and other periodic events with the host's
  display refresh or system clock, pacing the emulation to match real time.
- This mode provides the most authentic user experience, with the emulator
  "perceiving" and responding to real time as the original machine would.

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

## Repository Layout

The repository is organized as follows:

- **src/** — Emulator source code
  - **core/** — Platform-agnostic emulator core (CPU, memory, devices,
    scheduler, etc.)
    - **cpu/** — CPU emulation
      - _cpu.c_ — Lifecycle, public API, runtime dispatch
      - _cpu_68000.c_ — 68000 instruction decoder instantiation
      - _cpu_68030.c_ — 68030 decoder stub (IIcx placeholder)
      - _cpu_internal.h_ — Shared struct and static inline helpers
      - _cpu_ops.h_ / _cpu_decode.h_ — Template-based decoder generation
      - _cpu_disasm.c_ — Disassembler
    - **memory/** — Page-table-based memory map (see `docs/memory.md`)
    - **peripherals/** — VIA, SCC, SCSI, floppy, RTC, keyboard, mouse, sound
    - **scheduler/** — Event scheduling and timing
    - **debug/** — Debugger, logging, and diagnostics
    - **storage/** — Disk image and storage backends
    - **network/** — AppleTalk and networking modules
    - **shell/** — CLI and shell integration
  - **machines/** — Machine profile definitions
    - _machine.h/c_ — Profile struct and registry
    - _plus.c_ — Macintosh Plus hardware profile
    - _iicx.c_ — Macintosh IIcx hardware profile (stub)
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
  - **unit/** — Native unit tests (C, portable); see `docs/tests.md` for architecture
  - **e2e/** — Playwright end-to-end (browser) tests
  - **integration/** — Headless integration tests (native CLI)
  - **data/** — Shared test data (ROMs, disk images, etc.)

- **third-party/** — External dependencies (e.g., peeler archive library)

- **docs/** — Documentation, architecture, and developer notes

- **scripts/** — Build helpers, dev tools, and automation scripts

### Multi-machine support

The emulator supports multiple machine profiles built on the same shared core.
The `src/machines/` layer defines hardware profiles that describe each machine's
characteristics:

- **`machine.h`** — Defines `hw_profile_t` with CPU model, clock speed, address
  bus width, RAM/ROM sizes, VIA count, and callback hooks
- **`plus.c`** — Macintosh Plus profile (68000, 7.8 MHz, 24-bit, fully implemented)
- **`iicx.c`** — Macintosh IIcx profile (68030, 15.7 MHz, 32-bit, stub/placeholder)
- **`machine.c`** — Profile registry for lookup by name

The CPU is split into template-instantiated decoders (`cpu_68000.c`,
`cpu_68030.c`) sharing helpers via `cpu_internal.h`. The memory subsystem uses a
page-table architecture that supports both the Plus (static page table) and the
IIcx (page table rebuilt by the 68030 MMU). See `docs/memory.md` for details.

Peripherals use callback-based signal routing (function pointers passed at
construction time) rather than hard-coded global functions, enabling different
machines to wire signals differently. Checkpoint I/O is factored into a
standalone `checkpoint.c` module.
