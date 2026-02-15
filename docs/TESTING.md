# Testing

This document describes the test architecture for Granny Smith. The project uses
three test tiers: native C unit tests, native C integration tests (headless
emulator), and browser-based end-to-end tests (Playwright).

## Quick Reference

| Tier | Command | Duration | Test Data Required |
|------|---------|----------|--------------------|
| Unit | `make -C tests/unit run` | 1–5 min | No (uses `third-party/single-step-tests`) |
| Integration | `make integration-test` | 1–2 min | Yes |
| E2E | `make e2e-test` | 10–15 min | Yes |
| Unit + Integration | `make test` | 2–7 min | Partially |

Test data is fetched via `scripts/fetch-test-data.sh` (requires
`GS_TEST_DATA_TOKEN`). See [TEST_DATA.md](TEST_DATA.md).

## Directory Layout

```
tests/
├── data/                           # Proprietary test assets (.gitignored)
├── unit/                           # Native C unit tests
│   ├── Makefile                   #   Orchestrator (discovers suites/*/Makefile)
│   ├── common.mk                 #   Shared build rules + harness selection
│   ├── suites/                    #   All test suites
│   │   ├── cpu/                   #     CPU single-step instruction tests
│   │   ├── cputest/               #     CPU instruction dataset harness
│   │   ├── disasm/                #     Disassembler corpus test
│   │   └── storage/               #     Storage subsystem tests
│   └── support/                   #   Shared infrastructure
│       ├── test_assert.h          #     Assertion macros
│       ├── harness.h              #     Harness API
│       ├── harness_*.c            #     Harness implementations
│       ├── stub_*.c               #     Focused stub modules
│       ├── platform.h             #     Platform header override
│       └── log.h                  #     Logging header override
├── integration/                    # Headless emulator integration tests
│   ├── boot/                      #   Basic boot + shell commands
│   ├── checkpoint/                #   Checkpoint save/restore
│   ├── checkpoint2/               #   Consolidated checkpoint restore
│   └── scsi/                      #   SCSI disk boot
└── e2e/                            # Browser Playwright E2E tests
    ├── specs/                     #   All test suites (12 suites)
    ├── helpers/                   #   Shared TypeScript utilities
    ├── types/                     #   TypeScript type stubs
    ├── fixtures.ts                #   Shared Playwright fixtures
    ├── global-setup.ts            #   Build + server startup
    └── playwright.config.ts       #   Playwright configuration
```

---

## Unit Tests

### Overview

The unit test infrastructure uses **explicit test harnesses** and **dependency
injection**. Tests declare their requirements through a harness mode, and the
build system provides the appropriate stubs and real implementations.

### Harness Modes

Each test declares a harness mode in its Makefile via `TEST_HARNESS`:

| Mode | Description | Use Case |
|------|-------------|----------|
| `isolated` | Pure stub-only environment | Tests that don't need emulator subsystems |
| `cpu` | Real CPU + memory subsystems | CPU and disassembler tests |

Example Makefile (in `suites/<name>/`):
```makefile
TEST_NAME := mytest
TEST_SRCS := test.c
TEST_HARNESS := isolated
include ../../common.mk
```

### Test Context

Tests using the harness work with a `test_context_t` structure:

```c
#include "harness.h"

int main(void) {
    test_context_t *ctx = test_harness_init();
    cpu_t *cpu = test_get_cpu(ctx);
    memory_map_t *mem = test_get_memory(ctx);
    // ... run tests ...
    test_harness_destroy(ctx);
    return 0;
}
```

### Running Unit Tests

```bash
make -C tests/unit run            # Build + run all
make -C tests/unit list           # List discovered test names
make -C tests/unit test-cputest   # Run a single test
make -C tests/unit test-disasm    # Run disassembler test
```

### Writing a New Unit Test

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

### Assertion Macros

From `test_assert.h`:

- `TEST(name)` — Declare a test function
- `RUN(testfn)` — Run a test and report pass/fail
- `ASSERT_TRUE(expr)` — Assert expression is true
- `ASSERT_EQ_INT(expected, actual)` — Assert integer equality

### Stub Modules

Stubs live in `tests/unit/support/` and are split into focused modules that can
be selectively linked:

| Stub Module | Contents |
|-------------|----------|
| `stub_platform.c` | `platform_bsr32()`, `platform_ntz32()`, timing functions |
| `stub_shell.c` | `register_cmd()`, `shell_init()`, `shell_dispatch()` |
| `stub_checkpoint.c` | `system_read_checkpoint_data_loc()`, `checkpoint_has_error()` |
| `stub_system.c` | `system_memory()`, `system_cpu()`, etc. (uses harness context) |
| `stub_memory.c` | Memory globals and access functions (for isolated mode) |
| `stub_debugger.c` | `debugger_init()`, `debug_break_and_trace()` |
| `stub_peripherals.c` | `floppy_new()`, `process_packet()` |
| `stub_assert.c` | `gs_assert_fail()`, `init_tests()` |

Each test picks the minimal subset of emulator `.c` files it needs via
`EXTRA_SRCS`. Prefer adding a focused stub over pulling a large subsystem.

### Disassembler Corpus

`tests/unit/suites/disasm` expects a `disasm.txt` corpus. If absent, the test
logs a skip message and passes. Set `REQUIRE_DISASM_CORPUS=1` to fail when
missing.

---

## Integration Tests

Integration tests exercise the full emulator in headless mode (native binary,
no browser). Each test has a `config.mk` (ROM/disk paths, arguments) and a
`test.script` (shell commands + expected-output checks).

### Running Integration Tests

```bash
make integration-test               # Build headless + run all
make integration-test-valgrind      # Same, under Valgrind
make -C tests/integration test-boot # Run a single test
make -C tests/integration list      # List available tests
```

### Writing a New Integration Test

1. Create `tests/integration/foo/` with `config.mk` and `test.script`.
2. `config.mk` sets `ROM`, `FD_IMAGE` or `HD_IMAGE`, and emulator arguments.
3. `test.script` contains shell commands sent to the headless emulator.
4. Run: `make -C tests/integration test-foo`

---

## End-to-End Tests (Playwright)

Browser-based tests using Playwright that exercise the full WASM emulator in
Chromium. Test suites live under `tests/e2e/specs/`, shared helpers under
`tests/e2e/helpers/`.

### Test Suites

| Suite | What it tests |
|-------|---------------|
| `appletalk/` | AppleTalk networking, AFP volume mounting |
| `apps/` | MacTest application boot + execution |
| `basic-ui/` | Terminal toggle, UI element presence |
| `boot-matrix/` | ROM × System disk boot combinations |
| `configuration/` | URL parameter boot, full-screen baselines |
| `debug/` | Shell debugger commands |
| `drag-drop/` | File drop (ROM, disk, archive files) |
| `floppy/` | Floppy insert, eject, swap, disk-copy |
| `peeler/` | Archive extraction (StuffIt, BinHex) |
| `scsi/` | SCSI hard disk boot + image baselines |
| `state/` | Checkpoint save/restore across page reloads |
| `terminal/` | Terminal panel collapse/expand |

### Running E2E Tests

```bash
make e2e-test                       # Build + run all

# Run a specific suite
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts specs/floppy/

# Headed mode
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts --headed
```

### Prerequisites

```bash
cd tests/e2e && npm ci
npx playwright install --with-deps chromium
./scripts/fetch-test-data.sh
```

### Video Recording

Disabled by default. Enable with `PWTEST_VIDEO=1`:
```bash
PWTEST_VIDEO=1 make e2e-test
```

### GS_ASSERT Integration

E2E tests automatically detect and fail when any `GS_ASSERT()` fires in the
emulator C code. See `tests/e2e/README-assertions.md`.

### Baselines

Reference PNG screenshots sit beside their spec files. Update with
`UPDATE_SNAPSHOTS=1`.

### Writing a New E2E Suite

1. Create `tests/e2e/specs/foo/` with `foo.spec.ts`.
2. Import fixtures and helpers:
   ```typescript
   import { test, expect } from '../../fixtures';
   import { bootWithMedia } from '../../helpers/boot';
   import { matchScreenFast } from '../../helpers/screen';
   ```
3. Place baseline PNGs alongside the spec.
4. Run: `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts specs/foo/`

### Debugging E2E Failures

- Terminal output is captured in `tests/e2e/test-results/<test>-<project>/xterm/*.txt`
- Traces: `npx playwright show-trace tests/e2e/test-results/<test>/trace.zip`
- Enable logging in specs: `await runCommand(page, 'log <category> <level>')`
- Enable video: `PWTEST_VIDEO=1`
