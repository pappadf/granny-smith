Test Structure
==============

The repository has three test tiers: native C unit tests, native C integration
tests (headless emulator), and browser-based end-to-end tests (Playwright).

Directory Overview
------------------
```
tests/
├── data/                           # Proprietary test assets (.gitignored)
│   ├── roms/                       #   ROM images
│   └── systems/                    #   System disk images
│
├── unit/                           # Native C unit tests
│   ├── Makefile                    #   Orchestrator (discovers suites/*/Makefile)
│   ├── common.mk                  #   Shared build rules + harness selection
│   ├── suites/                    #   All test suites
│   │   ├── cpu/                   #     CPU single-step instruction tests
│   │   ├── disasm/                #     Disassembler corpus test
│   │   └── storage/               #     Storage subsystem tests
│   └── support/                   #   Shared infrastructure
│       ├── test_assert.h          #     Assertion macros (TEST, RUN, ASSERT_*)
│       ├── harness.h              #     Harness API
│       ├── harness_common.c       #     Shared context management
│       ├── harness_isolated.c     #     Stub-only harness
│       ├── harness_cpu.c          #     CPU+memory harness
│       ├── stub_*.c               #     Focused stub modules
│       ├── platform.h             #     Platform header override
│       └── log.h                  #     Logging header override (no-op)
│
├── integration/                    # Native C integration tests (headless)
│   ├── Makefile                   #   Auto-discovers test dirs with test.script
│   ├── boot/                      #   Basic boot + shell command tests
│   ├── checkpoint/                #   Checkpoint save/restore cycle
│   ├── checkpoint2/               #   Consolidated checkpoint restore
│   └── scsi/                      #   SCSI disk boot tests
│
└── e2e/                            # Browser Playwright tests
    ├── playwright.config.ts       #   Playwright config (testDir → specs/)
    ├── fixtures.ts                #   Shared fixtures (logging, xterm capture)
    ├── global-setup.ts            #   Builds project, starts server on :18080
    ├── global-teardown.ts         #   Stops server
    ├── specs/                     #   All test suites
    │   ├── appletalk/             #     AppleTalk / AFP networking
    │   ├── apps/                  #     MacTest application harness
    │   ├── basic-ui/              #     UI smoke tests
    │   ├── boot-matrix/           #     ROM × System disk matrix comparisons
    │   ├── configuration/         #     URL-param boot + baselines
    │   ├── debug/                 #     Debugger shell commands
    │   ├── drag-drop/             #     File drop tests
    │   ├── floppy/                #     Floppy insert/eject/swap
    │   ├── peeler/                #     Archive extraction (StuffIt, HQX)
    │   ├── scsi/                  #     SCSI hard-disk boot + baselines
    │   ├── state/                 #     Checkpoint save/restore/reload
    │   └── terminal/              #     Terminal panel interaction
    ├── helpers/                   #   Shared utilities (boot, screen, mouse, …)
    └── types/                     #   TypeScript type stubs
```

Running Tests
-------------

### All tests (unit + integration)

```bash
make test
```

### Unit tests (native C)

```bash
make -C tests/unit run           # Build + run all
make -C tests/unit list          # List discovered test names
make -C tests/unit test-cputest  # Run a single test
```

### Integration tests (native headless emulator)

Requires test data from `scripts/fetch-test-data.sh`.

```bash
make integration-test             # Build headless + run all
make integration-test-valgrind    # Same, under Valgrind
```

### End-to-end tests (Playwright, browser)

Requires test data + Playwright + Chromium. See `tests/e2e/README.md`.

```bash
make e2e-test                     # Build + run all e2e tests
```

Or with more control:

```bash
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts specs/floppy/
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts --headed
```

**Video recording** is disabled by default. Enable with `PWTEST_VIDEO=1`.

Test Data
---------
ROM images and disk images are proprietary and not committed to the repository.
Fetch them with:

```bash
./scripts/fetch-test-data.sh      # Requires GS_TEST_DATA_TOKEN
```

Files are placed under `tests/data/` (`.gitignored`). Most integration and e2e
tests will be skipped if test data is not available. Unit tests (CPU instruction
tests) do not require test data.

Adding New Tests
----------------

### New unit test

1. Create `tests/unit/suites/foo/` with a `Makefile` and `test.c`.
2. The Makefile declares `TEST_NAME`, `TEST_SRCS`, `TEST_HARNESS`, and includes
   `../../common.mk`. See the "Writing a New Test Suite" section in
   `tests/unit/README.md` for a template.
3. Run: `make -C tests/unit test-foo`.

### New integration test

1. Create `tests/integration/foo/` with `config.mk` and `test.script`.
2. `config.mk` sets ROM/disk paths and emulator arguments.
3. `test.script` contains shell commands and expected-output checks.
4. Run: `make -C tests/integration test-foo`.

### New e2e test suite

1. Create `tests/e2e/specs/foo/` with `foo.spec.ts`.
2. Import from `../../fixtures` and `../../helpers/`.
3. Place baseline PNGs alongside the spec.
4. Run: `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts specs/foo/`

Baselines & Snapshots
---------------------
E2e image baseline PNGs sit beside their spec files. Update with
`UPDATE_SNAPSHOTS=1` when running the relevant suite.

