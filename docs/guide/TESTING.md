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
└── e2e/                            # Browser Playwright E2E tests (web2 UI)
    ├── web2-specs/                #   Functional suite (playwright.web2.config.ts)
    ├── ui-prod-smoke/             #   Production-bundle boot smoke
    ├── webkit-local/             #   Local WebKit upload/OPFS checks
    ├── helpers/web2-fs.ts         #   OPFS staging + drag helpers
    ├── test_server.py             #   COOP/COEP static server
    └── playwright.web2.config.ts  #   Main Playwright configuration
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

Browser-based tests that exercise the full WASM emulator through the **web2**
UI (`app/web2/`) in Chromium. Specs live under `tests/e2e/web2-specs/`; the
one shared helper is `tests/e2e/helpers/web2-fs.ts`. See
[tests/e2e/README.md](../../tests/e2e/README.md) for the full layout.

> The legacy web UI and its `tests/e2e/specs/**` suite were retired; unique
> coverage moved here or into the headless integration tests above.

### Specs (`tests/e2e/web2-specs/`)

| Spec | What it tests |
|------|---------------|
| `checkpoint-resume` | Checkpoint save → reload → resume; SE/30 profile restore |
| `display-card-config` | New Machine dialog: video card selected by name |
| `display-drop` | Drag-and-drop onto the Display: ROM boot, floppy mount, checkpoint restore |
| `filesystem-tab` | Filesystem tab: descend image, copy/move/rename, unpack archive |
| `iicx-video-modes` | Post-shader WebGL canvas baselines (monitor × depth) |
| `iifx-aux3-realtime` | A/UX 3.0.1 boot to login under the real RAF scheduler |
| `lisa-xenix-profile` | Lisa/XL ProFile-vs-SCSI config + boot |
| `url-boot` | `?rom=…` URL-parameter boot |

Two more configs run separately: `ui-prod-smoke/` (production-bundle boot
smoke, no data) and `webkit-local/` (local WebKit OPFS upload).

### Running E2E Tests

```bash
make ui2-e2e                        # Build + run the functional web2 suite (alias: make e2e-test)
make ui2-prod-smoke                 # Production-bundle smoke test

# A specific spec
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts url-boot

# Headed mode
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts --headed
```

The config's `webServer` block builds + serves `app/web2/dist` automatically.

### Prerequisites

```bash
cd tests/e2e && npm ci
npx playwright install --with-deps chromium
./scripts/fetch-test-data.sh        # functional specs boot real machines
```

### Baselines

Playwright `toMatchSnapshot` baselines live in `<spec>.ts-snapshots/` beside
the spec. Regenerate with `--update-snapshots`. Raw-framebuffer pixel oracles
live in the headless integration tests, not here.

### Writing a New E2E Spec

1. Add `tests/e2e/web2-specs/foo.spec.ts`.
2. Import Playwright + the web2 helpers:
   ```typescript
   import { test, expect } from '@playwright/test';
   import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
   ```
3. Drive through the shipped UI (dialog, Terminal panel, drag-and-drop); web2
   has no `window.gsEval` — reach the object model via the Terminal.
4. Run: `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts foo`

### Debugging E2E Failures

- Traces: `npx playwright show-trace tests/e2e/test-results/<test>/trace.zip`
- Screenshots/artifacts land under `tests/e2e/test-results/<test>-<project>/`
- Drive the emulator's shell from a spec via the Terminal panel (`.xterm`)
