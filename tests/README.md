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
└── e2e/                            # Browser Playwright tests (web2 UI)
    ├── playwright.web2.config.ts  #   Main config (testDir → web2-specs/)
    ├── playwright.prod-smoke.config.ts # Production-bundle boot smoke
    ├── playwright.webkit-local.config.ts # Local WebKit upload/OPFS checks
    ├── test_server.py             #   COOP/COEP static server
    ├── web2-specs/                #   Functional suite (checkpoint, drop, url-boot, …)
    ├── ui-prod-smoke/             #   Production-bundle smoke test
    ├── webkit-local/              #   Local WebKit OPFS upload test
    └── helpers/web2-fs.ts         #   OPFS staging + drag helpers
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
make -C tests/unit list          # List discovered test names```

### Integration tests (native headless emulator)

Requires test data from `scripts/fetch-test-data.sh`.

```bash
make integration-test             # Build headless + run all
make integration-test-valgrind    # Same, under Valgrind
```

### End-to-end tests (Playwright, browser)

Requires test data + Playwright + Chromium. See `tests/e2e/README.md`.

```bash
make ui2-e2e                      # Build + run the web2 functional suite (alias: make e2e-test)
make ui2-prod-smoke               # Production-bundle boot smoke (no data needed)
```

Or with more control:

```bash
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts url-boot
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts --headed
```

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

### New e2e test

1. Add `tests/e2e/web2-specs/foo.spec.ts`.
2. Import `@playwright/test` and `../helpers/web2-fs`; drive through the shipped
   web2 UI (web2 has no `window.gsEval` — use the Terminal panel).
3. Run: `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts foo`

Baselines & Snapshots
---------------------
Playwright `toMatchSnapshot` baselines live in a `<spec>.ts-snapshots/`
directory beside the spec; regenerate with `--update-snapshots`. Raw-framebuffer
pixel oracles live in the integration tests.

