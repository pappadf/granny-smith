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
├── unit/                           # Native C unit tests (tests/unit/README.md)
│   ├── Makefile                    #   Orchestrator (discovers suites/*/Makefile)
│   ├── common.mk                   #   The build recipe every suite includes
│   ├── suites/<name>/              #   One directory per suite
│   └── support/                    #   Harnesses, stubs, header shims
│
├── integration/                    # Native C integration tests (headless)
│   ├── Makefile                    #   Auto-discovers test dirs with test.script
│   ├── lib/                        #   Shared row/wait/golden library (include'd)
│   ├── suite-<family>/             #   Per-machine suites (rows + goldens/)
│   └── <name>/                     #   One directory per test (config.mk, test.script)
│
└── e2e/                            # Browser Playwright tests (tests/e2e/README.md)
    ├── playwright.web2.config.ts  #   Main config (testDir → web2-specs/)
    ├── playwright.prod-smoke.config.ts # Production-bundle boot smoke
    ├── playwright.webkit-local.config.ts # upload.spec.ts on WebKit (macOS)
    ├── test_server.py             #   COOP/COEP static server
    ├── web2-specs/                #   Functional suite (checkpoint, drop, url-boot, …)
    ├── ui-prod-smoke/             #   Production-bundle smoke test
    └── helpers/web2-fs.ts         #   OPFS staging + drag helpers
```

The lists themselves come from the tools: `make -C tests/unit list`,
`make -C tests/integration list` (each test with its tier), and the spec
tree in [e2e/README.md](e2e/README.md).

Running Tests
-------------

### All tests (unit + integration)

```bash
make test
```

### Unit tests (native C)

```bash
make -j$(nproc) -C tests/unit run # Build (in parallel) + run all
make -C tests/unit list          # List discovered test names
```

### Integration tests (native headless emulator)

Requires test data from `scripts/fetch-test-data.sh`.

```bash
make integration-test                     # Build headless + run all
make integration-test TIER=matrix -j8     # One tier: unit | matrix | extended
make integration-test-valgrind TIER=unit  # Under Valgrind
```

Scope Valgrind to the unit tier or to single tests
(`make -C tests/integration test-valgrind-<name>`): at its 20–50x slowdown a
sweep of every test does not finish.

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
tests will be skipped if test data is not available. The unit tests do not
read `tests/data/`, but the CPU suites need the `third-party/single-step-tests`
and `third-party/powerpc-test` submodules (`git submodule update --init`).

Adding New Tests
----------------

### New unit test

1. Create `tests/unit/suites/foo/` with a `Makefile` and `test.c`.
2. The Makefile declares `TEST_NAME`, `TEST_SRCS`, `TEST_HARNESS`, and includes
   `../../common.mk`. See [unit/README.md](unit/README.md) for the variables,
   harness modes and stubs.
3. Run: `make -C tests/unit test-foo`.

### New integration test

1. Create `tests/integration/foo/` with `config.mk` and `test.script`.
2. `config.mk` sets `TEST_ROM`, `TEST_ARGS` and `TEST_TIER` (`unit`, `matrix`
   or `extended`; a test without a tier runs in no CI step). A boot belongs as
   a row in its machine's `suite-<family>` instead of a directory of its own.
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

