# Native Unit Tests

Native (non-Emscripten) C tests for focused emulator validation. Each suite
under `suites/` is one test binary that compiles only the emulator sources it
tests and links stubs for everything else, so an edit/compile/run cycle takes
seconds. Artifacts go to `tests/unit/build/` (git-ignored).

Where the unit suites sit among the other test tiers is in
[docs/guide/TESTING.md](../../docs/guide/TESTING.md).

## Layout

```
tests/unit/
  Makefile         # Orchestrator: discovers suites/*/Makefile, builds and runs them
  common.mk        # The one build recipe every suite includes
  suites/<name>/   # One directory per suite: a Makefile, test.c, any fixtures
  support/         # Harnesses, stubs and header shims shared by the suites
```

## Commands

From the repository root:

```
make -j$(nproc) -C tests/unit run   # build every suite in parallel, then run them all
make -C tests/unit test-<name>      # build and run one suite (e.g. test-disasm)
make -C tests/unit list             # list the suites
make -C tests/unit run-wasm32       # rerun the parser/storage suites on wasm32 under node
make -C tests/unit clean            # remove build artifacts
```

`run` builds with `-k`, so a suite that fails to compile does not stop the
others from building. It then runs every suite, including after a failure, and
ends with `All N tests passed` or `passed/total passed, FAILED: <suites>`,
exiting non-zero if any suite failed to build or run. Within one suite the
first failed assertion ends that binary (see [Assertions](#assertions)).

A built binary also runs on its own: `./tests/unit/build/<name>`.

## The suite Makefile

A suite's `Makefile` sets variables and includes `../../common.mk`. The
orchestrator refuses a suite that does not (`peeler_corpus`, which drives the
archive library's own test script, is the one exemption).

| Variable | | Meaning |
|---|---|---|
| `TEST_NAME` | required | Binary name. |
| `TEST_SRCS` | required | The suite's own sources, relative to its directory. |
| `TEST_HARNESS` | default `isolated` | `isolated`, `cpu` or `none`; see below. |
| `EXTRA_SRCS` | | Emulator `.c` files under test (relative paths into `src/`). |
| `STUBS` | | Extra `support/stub_<name>.c` files to link, by name (`STUBS := assert checkpoint`). |
| `OMIT_STUBS` | | Harness stubs to leave out, by name, when the suite links the real module instead (`OMIT_STUBS := memory`). |
| `EXTRA_CFLAGS` | | Extra compile flags. |
| `EXTRA_LDFLAGS` | | Extra link flags. |
| `SANITIZE` | | Sanitizer flags, applied to both compile and link (`SANITIZE := -fsanitize=address,undefined`). |
| `INCLUDE_FLAGS` | | Replaces the default include list, for a suite that deliberately compiles against a narrower set of headers. |
| `RUN_ENV` | | Environment variables for the run (`RUN_ENV := FOO=1`). |

Every suite compiles with the product builds' dialect and warnings
(`-std=gnu11 -Wall -Wextra`). `WERROR=1`, which CI sets, makes warnings
errors. Objects are stamped with a hash of their compile flags, so changing
`EXTRA_CFLAGS`, `SANITIZE` or `CC` rebuilds the suite.

Minimal example:

```make
TEST_NAME    := example
TEST_SRCS    := test.c
TEST_HARNESS := isolated
include ../../common.mk
```

## Harness modes

| Mode | What it links | `main()` |
|---|---|---|
| `isolated` | `harness_isolated.c` and the default stubs: platform, shell, checkpoint, system, memory, debugger, peripherals, assert | The suite's, calling `test_harness_init()` |
| `cpu` | `harness_cpu.c`, the real 68k CPU, FPU, memory, MMU and object-model sources, and the same stubs minus memory plus `stub_lisa_mmu.c` | The suite's, calling `test_harness_init()` |
| `none` | Nothing but what the suite names in `EXTRA_SRCS` and `STUBS` | The suite's; it brings its own mocks |

The two harnessed modes give the test a `test_context_t` that owns the CPU,
memory and scheduler, and `stub_system.c` routes `system_memory()`,
`system_cpu()` and the rest to it:

```c
#include "harness.h"
#include "test_assert.h"

TEST(basic) { ASSERT_TRUE(1); }

int main(void) {
    test_context_t *ctx = test_harness_init();
    RUN(basic);
    test_harness_destroy(ctx);
    return 0;
}
```

`none` suits a test of one device or library linked against hand-written
mocks: the `rtc` suite, for instance, links `rtc.c` and the object model, and
`STUBS := machine_object assert checkpoint`.

Prefer a two-line stub over dragging in a subsystem. List a real module in
`EXTRA_SRCS` only when its logic is what the test is about.

## Stubs

All in `support/`. The harnessed modes link the defaults listed above; any
suite can add one with `STUBS` or drop one with `OMIT_STUBS`.

| Stub | Default in | Provides |
|---|---|---|
| `stub_assert.c` | isolated, cpu | `gs_assert_fail()`, `gs_unimplemented_fail()`, `init_tests()` |
| `stub_checkpoint.c` | isolated, cpu | No-op checkpoint read/write (`system_read_checkpoint_data_loc()`, `checkpoint_has_error()`, ...) |
| `stub_cpu.c` | | `cpu_set_an()`, `cpu_set_pc()` |
| `stub_debugger.c` | isolated, cpu | No-op debugger hooks (`debugger_init()`, `debug_break_and_trace()`, ...) |
| `stub_display_class.c` | | `display_attach_video_node()` / `display_detach_video_node()`, for a device linked without the framebuffer object |
| `stub_lisa_mmu.c` | cpu | The Lisa segment-MMU entry points the real `memory.c` slow path calls |
| `stub_machine_object.c` | | `machine_object()` alone, for `none` suites that do not link `stub_system.c` |
| `stub_memory.c` | isolated | No-op memory access, for tests that do not use real memory |
| `stub_peripherals.c` | isolated, cpu | No-op floppy and RTC entry points |
| `stub_platform.c` | isolated, cpu | Sound and timing platform hooks |
| `stub_shell.c` | isolated, cpu | `shell_init()`, `shell_dispatch()`, `parse_address()` |
| `stub_system.c` | isolated, cpu | `system_*()` accessors routed to the harness context |

`scripts/check-doc-inventories.py` (Hygiene workflow) fails if a
`support/stub_*.c` is missing from this table.

The other support files: `harness.h`, `harness_common.c`,
`harness_isolated.c`, `harness_cpu.c` (the harness), `test_assert.h` (the
macros), `platform.h`, `platform_clock.h` and `log.h` (header shims
force-included into every compile), and `hfsplus_builder.[ch]` (builds HFS+
volumes for the storage suites).

## Assertions

From `test_assert.h`:

- `TEST(name)` declares a test function; `RUN(name)` runs it between
  `[RUN ]` and `[PASS]` lines.
- `ASSERT_TRUE(expr)` and `ASSERT_EQ_INT(a, b)` print `[FAIL] file:line` and
  **exit the binary**, so the tests after a failed one in the same suite do
  not run. `TEST_ONLY=<name>` runs one test alone, which is how a fixture is
  checked against unfixed code when an earlier test would end the run first.

Some suites keep their own counters and report every failure before exiting
non-zero; either way a non-zero exit is the suite's failure.

## Disassembler corpus

`suites/disasm` reads `disasm.txt` (65,536 instruction lines). Without it the
suite prints a skip line and passes, for fast local loops.
`REQUIRE_DISASM_CORPUS=1 make -C tests/unit test-disasm` makes a missing
corpus a failure.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Duplicate symbol | A default stub and a real module both define it | `OMIT_STUBS` the stub, or use `TEST_HARNESS := none` |
| Undefined reference | A real module calls something no linked file provides | Add the defining source to `EXTRA_SRCS`, or name a stub in `STUBS` |
| Suite ignores a flag change | It should not: the flags stamp rebuilds it | `make -C tests/unit/suites/<name> clean` and report it |
