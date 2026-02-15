# Playwright End-to-End Tests

Browser-based end-to-end tests for the Granny Smith emulator using Playwright.

All Node dev dependencies (Playwright, pngjs, types) live here to keep the
repository root focused on the C/WebAssembly sources.

## Directory Structure

```
tests/e2e/
├── playwright.config.ts          # Playwright configuration (testDir → specs/)
├── package.json                  # Node dependencies (Playwright, pngjs, etc.)
├── tsconfig.json                 # TypeScript config
├── fixtures.ts                   # Shared test fixtures (logging, xterm capture)
├── global-setup.ts               # Builds project, starts test server on :18080
├── global-teardown.ts            # Stops test server
├── test_server.py                # COOP/COEP-enabled HTTP server for tests
│
├── specs/                        # All test suites (Playwright testDir)
│   ├── appletalk/                # AppleTalk / AFP networking tests
│   ├── apps/                     # MacTest application harness
│   ├── basic-ui/                 # UI smoke tests (terminal toggle, etc.)
│   ├── boot-matrix/              # ROM × System disk matrix comparisons
│   ├── configuration/            # URL-param boot + full-screen baselines
│   ├── debug/                    # Debugger shell command tests
│   ├── drag-drop/                # File drop (ROM, disk, archive) tests
│   ├── floppy/                   # Floppy insert/eject/swap workflows
│   ├── peeler/                   # Archive extraction (StuffIt, HQX, etc.)
│   ├── scsi/                     # SCSI hard-disk boot + image baselines
│   ├── state/                    # Checkpoint save/restore/reload tests
│   └── terminal/                 # Terminal panel interaction tests
│
├── helpers/                      # Shared utilities imported by specs
│   ├── boot.ts                   # bootWithMedia, bootWithUploadedMedia
│   ├── config-helpers.ts         # URL/config manipulation
│   ├── drop.ts                   # Synthetic drag-and-drop event dispatch
│   ├── logging.ts                # Test logger setup
│   ├── memfs.ts                  # Emscripten MEMFS inspection
│   ├── mouse.ts                  # Mouse click/double-click/drag helpers
│   ├── run-command.ts            # Shell command execution + prompt waiting
│   ├── screen.ts                 # Screen capture + baseline image matching
│   └── terminal.ts               # xterm.js capture + test shim install
│
├── types/                        # TypeScript type stubs
│   └── pngjs.d.ts
│
├── README-assertions.md          # GS_ASSERT integration docs
├── ASSERTION-IMPLEMENTATION.md   # Assertion implementation details
└── test-results/                 # Generated: traces, screenshots, reports
```

## Running Tests

From the repository root (recommended):

```bash
# Via Makefile
make e2e-test

# Via npx directly
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts

# Run a single suite
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts specs/floppy/

# Headed mode (watch the browser)
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts --headed
```

From this directory:

```bash
npx playwright test
npx playwright test specs/scsi/
```

Global setup automatically builds the project and starts a test server on port
18080 — no manual server step is needed.

## Prerequisites

Tests require proprietary test data (ROM images, disk images) fetched via
`scripts/fetch-test-data.sh`. See [docs/TEST_DATA.md](../../docs/TEST_DATA.md).

Install Playwright and browsers:

```bash
cd tests/e2e && npm ci
npx playwright install --with-deps chromium
```

## Video Recording

Video recording is **disabled by default** to save CPU (avoids ffmpeg overhead).

To enable (useful for debugging failures):

```bash
PWTEST_VIDEO=1 npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts
```

Videos are saved to `test-results/` when enabled.

## Baselines & Snapshots

Reference PNG screenshots sit beside their spec files in each suite directory.
Update baselines by running with `UPDATE_SNAPSHOTS=1`.

## GS_ASSERT Integration

Tests automatically detect and fail when any `GS_ASSERT()` fires in the emulator
C code. See [README-assertions.md](./README-assertions.md) for details.

## Adding a New Test Suite

1. Create a new directory under `specs/` (e.g. `specs/my-feature/`).
2. Add `my-feature.spec.ts` importing fixtures and helpers:
   ```typescript
   import { test, expect } from '../../fixtures';
   import { bootWithMedia } from '../../helpers/boot';
   ```
3. Add any baseline PNGs alongside the spec file.
4. Run: `npx playwright test --config=tests/e2e/playwright.config.ts specs/my-feature/`