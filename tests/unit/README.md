# Native Unit Tests

This directory hosts native (non-Emscripten) C tests for focused emulator
validation. Test suites live under `suites/`, shared infrastructure under
`support/`. Each suite is one test binary that: (1) declares the sources it
needs, (2) pulls in only the minimal emulator modules, and (3) links lightweight
stubs for everything else. The goal is very fast edit/compile/run cycles while
avoiding the complexity of the full WebAssembly target.

Artifacts go to `tests/unit/build/` (git-ignored). The system is intentionally
simple — one compile+link command per test — to reduce overhead.

For detailed architecture documentation, see `docs/tests.md`.

## High-Level Layout

```
tests/unit/
  Makefile              # Orchestrator: discovers suites/*/Makefile, drives build/run
  common.mk             # Shared rules & harness selection (TEST_HARNESS variable)
  suites/               # All test suites
    cpu/                #   CPU single-step instruction tests
      Makefile
      test.c
    disasm/             #   Disassembler corpus test
      Makefile
      test.c
      disasm.txt
    storage/            #   Storage subsystem tests
      Makefile
      test.c
  support/              # Shared infrastructure (harness, stubs, overrides)
    test_assert.h       #   Tiny harness macros (TEST, RUN, ASSERT_*)
    harness.h           #   Main harness API
    harness_common.c    #   Shared context management
    harness_isolated.c  #   Stub-only harness
    harness_cpu.c       #   CPU+memory harness
    stub_platform.c     #   Platform function stubs
    stub_shell.c        #   Shell command stubs
    stub_checkpoint.c   #   Checkpoint I/O stubs
    stub_system.c       #   System accessor stubs (routes to harness context)
    stub_memory.c       #   Memory stubs (isolated mode only)
    stub_debugger.c     #   Debugger stubs
    stub_peripherals.c  #   Peripheral stubs
    stub_assert.c       #   Assertion handler
    platform.h          #   Platform header override for native build
    log.h               #   Logging header override (no-op)
  build/                # Generated artifacts (git-ignored)
```

## Orchestrator Commands

From repo root (or inside `tests/unit/`):
```
make -C tests/unit            # build all test binaries
make -C tests/unit run        # build + run all (fails fast on first failure)
make -C tests/unit list       # list discovered test names
make -C tests/unit test-<name># build + run one test (e.g. test-disasm)
make -C tests/unit clean      # remove build artifacts
```
Run a single previously built binary directly: `./tests/unit/build/<name>`.

## Per-Test Makefile Contract

Inside each test subdirectory (`suites/<name>/`), create a short Makefile then
`include ../../common.mk`:

Variables (define before including `common.mk`):
* `TEST_NAME` (required) — Output binary name.
* `TEST_SRCS` (required) — Local sources (e.g. `test.c`).
* `TEST_HARNESS` (required) — Harness mode: `isolated` or `cpu`. Default is `isolated`.
* `EXTRA_SRCS` (optional) — Additional emulator `.c` files (relative paths).
* `EXTRA_CFLAGS` (optional) — Extra compile flags (e.g. feature `#define`s).

Example minimal test:
```make
TEST_NAME := example
TEST_SRCS := test.c
TEST_HARNESS := isolated
include ../../common.mk
```

`test.c` example:
```c
#include "test_assert.h"
#include "harness.h"

TEST(basic) { ASSERT_TRUE(1); }

int main(void) {
    test_context_t *ctx = test_harness_init();
    RUN(basic);
    test_harness_destroy(ctx);
    return 0;
}
```

## Test Harness Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| `isolated` | Stub-only environment | Tests that don't need real emulator subsystems |
| `cpu` | Real CPU + memory subsystems | CPU and disassembler tests |

The harness provides a `test_context_t` that encapsulates emulator state and
automatically configures system accessor stubs (`system_memory()`, etc.) to
return the appropriate pointers.

## Selecting Emulator Sources vs Stubbing

Rule of thumb: Prefer adding a 2-line stub over dragging in a large subsystem.
Only list emulator sources in `EXTRA_SRCS` when their real logic is essential
to what you're testing.

Common scenarios:
* Need CPU + memory (disassembler, CPU tests) — use `TEST_HARNESS := cpu`
* Need storage module only — use `TEST_HARNESS := isolated` and add `EXTRA_SRCS`
* Need nothing emulator-specific — use `TEST_HARNESS := isolated`

## Disassembler Corpus Skipping / Enforcement

`suites/disasm/test.c` looks for `disasm.txt`. Behavior:
* Present: Full corpus executed (expects 65,536 instruction lines). Fails on any mismatch.
* Absent: Prints:
  ```
  [disasm] disasm.txt not present; skipping full corpus test.
  ```
  and returns success (treated as a skip) for fast dev loops.

To force failure if the corpus is missing (e.g. in a stricter CI job), run with:
```
REQUIRE_DISASM_CORPUS=1 make -C tests/unit test-disasm
```

## Harness Macros

`TEST(name)` defines a static test function.
`RUN(name)` executes, wrapping output in `[RUN ]` / `[PASS]` lines.
`ASSERT_TRUE(expr)` / `ASSERT_EQ_INT(a,b)` abort the process on failure (makes
failures loud & simple).

## Writing a New Test Suite

1. Create `tests/unit/suites/mytest/` with a `Makefile`:
   ```makefile
   TEST_NAME := mytest
   TEST_SRCS := test.c
   TEST_HARNESS := isolated
   include ../../common.mk
   ```
2. Write `test.c`:
   ```c
   #include "test_assert.h"
   #include "harness.h"

   TEST(my_first_test) { ASSERT_TRUE(1 == 1); }
   TEST(my_second_test) { ASSERT_EQ_INT(42, 42); }

   int main(void) {
       test_context_t *ctx = test_harness_init();
       RUN(my_first_test);
       RUN(my_second_test);
       test_harness_destroy(ctx);
       return 0;
   }
   ```
3. Run: `make -C tests/unit test-mytest`

## Troubleshooting Cheat Sheet

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| Duplicate symbol from stub | Multiple stubs define same symbol | Check harness mode |
| Undefined reference to Xsetup_* | CPU / checkpoint code pulled in without stubs | Use appropriate harness mode or add to EXTRA_SRCS |
| Missing header include errors | Wrong relative path | Recalculate `../../` depth; emulator sources live in `src/` |

## Philosophy

Keep tests *surgical*: each binary should prove one narrow behavior slice. Fast
native binaries make it easy to iterate on core CPU / disassembly logic without
waiting for a WebAssembly rebuild or browser automation.

The Playwright suite continues to validate integrated behaviors; these native
tests are for low-level determinism and rapid feedback.

---
Happy hacking! Add new tests liberally — just keep the dependency surface tight.
