# Coding Agent Onboarding Guide

Granny Smith: a browser-based Motorola 68000 Macintosh Plus–style emulator compiled to WebAssembly using Emscripten, plus a headless variant compiled with gcc/clang for native host testing.

## Repository Directory Overview

- `src/core/`: Platform-agnostic emulator (cpu/, memory/, peripherals/, scheduler/, debug/, storage/, network/, shell/)
- `src/platform/`: Platform-specific code (wasm/, headless/)
  - `wasm/`: WebAssembly platform for browser (em_main.c, em_audio.c, em_video.c) — compiled with Emscripten
  - `headless/`: Native command-line platform for testing (headless_main.c)
- `app/web/`: Browser frontend (HTML, JS, CSS)
- `docs/`: Design, architecture, and developer docs
- `build/`: Generated artifacts — do not edit
- `scripts/`: Tools and helpers
- `tests/unit`: Unit tests (native, suites in `suites/`, infrastructure in `support/`)
- `tests/e2e`: Playwright end-to-end tests (specs in `specs/`, helpers in `helpers/`)
- `third-party/`: External libraries

Emulator modules (e.g., scsi, cpu, via, scc, rtc) have `.c`/`.h` files in `src/core/*/` and documentation in `docs/`.
  
  - `third-party/peeler` is a required git submodule for a full build. To clone with submodules, use:
    - `git clone --recurse-submodules <repo-url>`
    - Or, if already cloned, run: `git submodule update --init --recursive`

## Tools and Environments

**Devcontainer (recommended):**
- Prebuilt image: `ghcr.io/pappadf/granny-smith-dev:ubuntu24-emsdk4.0.10-nodeLTS`
- All prerequisites preinstalled: Emscripten 4.0.10, Node.js 18.x, Playwright, build tools

**Manual setup (outside devcontainer):**
1. Install Emscripten 4.0.10:
   ```bash
   git clone https://github.com/emscripten-core/emsdk
   cd emsdk && ./emsdk install 4.0.10 && ./emsdk activate 4.0.10
   source ./emsdk_env.sh
   ```
2. Install Node.js 18+ and npm
3. Install Playwright for e2e tests:
   ```bash
   cd tests/e2e && npm ci
   npx playwright install --with-deps chromium
   ```

**Required tools:** `emcc` (4.0.10), `make`, `node` (18+), `python3`, `git`  
**Local server:** `scripts/dev_server.py` or `python3 -m http.server 8080`  
**CI:** GitHub Actions workflows in `.github/workflows/`

## Building and Testing

**Build Emscripten (WASM) variant:**
- Clean build: `make clean && make` (~30 sec)
- Debug build: `make debug`
- Output: `build/` directory with `index.html`, `main.mjs`, etc.

**Run tests:**
- Unit tests (CPU): `make -C tests/unit run` (~1–5 min) — uses `third-party/single-step-tests`
- Integration tests: `make integration-test` (~1–2 min) — builds headless emulator, runs tests in `tests/integration/`
- Single integration test: `make integration-test-<name>` (e.g., `make integration-test-se30-format-hd`)
- List available integration tests: `make -C tests/integration list`
- E2E/UI tests (Playwright): 
  ```bash
  npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts
  ```
  (~10–15 min, requires test data from `scripts/fetch-test-data.sh`)
- In general, no need to run unit tests unless explicitly asked

**Environment-specific Testing Guidelines:**

*When running in a codespace (prepared devcontainer):*
- All prerequisites are preinstalled (node, Playwright, valgrind, etc.)
- All test types available: unit, integration, and e2e tests

*When NOT in a codespace:*
- Focus on headless target and integration tests (`make integration-test`)—these are portable C programs
- Only install valgrind if needed for memory checking
- Avoid e2e tests unless full toolchain is confirmed available
- If builds fail (especially HTTP 403 errors from emsdk), see `docs/COPILOT_ENV_SETUP.md`

**Important: Running Playwright Tests**
- A) **Never use the `timeout` command** with Playwright tests (e.g., `timeout 150 npx ...`). Playwright writes test data and logs at the end of tests; premature interruption will lose this data. Use built-in test timeout mechanisms instead (e.g., `test.setTimeout(180_000)` in test files).
- B) **Never pipe Playwright output** through `tail`, `grep`, or similar filters (e.g., `npx ... | tail -10`). Tests can take minutes to run. Instead, run the test once and save all output to a file (e.g., `npx ... 2>&1 | tee output.log`), then filter the saved output as needed without re-running.
- C) **Video recording is disabled by default** to save CPU (avoids ffmpeg overhead). Enable with `PWTEST_VIDEO=1` when debugging test failures.

**CI is kept green.** Never assume a test failure is pre-existing — treat every failure as caused by your changes until proven otherwise.

## Debugging and Tracing

**Debug tools:**
- `tools/disasm/disasm` — standalone 68K disassembler for ROM images and binaries (see `.agents/skills/disasm-tool/`)
- `build/headless/gs-headless` — headless emulator with TCP shell for interactive debugging (see `.agents/skills/headless-debug/`)

- `printf()` and `LOG(...)` output goes to **xterm.js** (browser terminal panel), not the JS console
- In E2E tests, terminal output is captured in: `tests/e2e/test-results/<test-name>-<project>/xterm/*.txt`
- Test specs live in `tests/e2e/specs/<suite>/`, shared helpers in `tests/e2e/helpers/`

**Log categories and levels:**
- Each module has its own log category (e.g., `cpu`, `floppy`, `scsi`, `logpoint`)
- Categories have a level threshold (0 = off); a `LOG(level, ...)` call only emits if `level <= category_level`
- Enable via shell: `log <category> <level>` (e.g., `log cpu 10`)
- Redirect to file: `log cpu 10 file=/tmp/cpu.log`

**Logpoints (`src/debug.c`):**
- PC logpoints emit a log message when the CPU executes a specific address or range, without stopping
- Set via shell: `logpoint <addr> [message] [category=<name>] [level=<n>]`
- The default category is `logpoint` for PC logpoints; enable it with `log logpoint 10`
- Memory logpoints fire on **read** or **write** accesses without halting:
  - `logpoint --write <addr>[.b|.w|.l] [msg]` — log every write
  - `logpoint --read <addr>[.b|.w|.l] [msg]` — log every read
  - `logpoint --rw <addr>[.b|.w|.l] [msg]` — log either direction
  - Default category is `memory` (enable with `log memory 1`)
  - Messages may reference `$pc`, `$value`, `$instruction_pc`, `$cpu.d0..d7`, `$cpu.a0..a7`, `$addr`
  - Implemented without slowing the fast path: covered pages are zeroed in the
    SoA arrays so only logged pages take the slow-path penalty (see `docs/memory.md`)
- Bus-error / exception trace ring is always on; dump with `info exceptions [N]`,
  stream live with `log exceptions 1`

**In Playwright E2E tests:** Use `await runCommand(page, 'log <category> <level>')` or `await runCommand(page, 'logpoint ...')` to enable logging or set logpoints programmatically.

## Coding Guidelines

- Use `tmp/` (under the project root) for temporary files — avoid `/tmp/` or `/dev/null`, as those paths require individual user approval for shell commands
- Find and fix the root cause: prefer robust fixes over workarounds and fallbacks
- Keep changes small: minimal diffs focused on the problem
- Match existing code style; put prototypes in headers if needed
- Use `snake_case` for identifiers
- Use `//` for short, inline comments
- For each significant statement or calculation, add a concise one-line comment
- Each function and structure needs a one-line comment above describing its purpose
- Do not change or remove existing comments unnecessarily
- Refer to `docs/STYLE_GUIDE.md` for conventions when unsure

## Object model (`gs_eval`)

Every emulator subsystem is exposed through a single tagged-union value
type and an opaque `object_t` tree rooted at `emu`. Top-level paths are
`cpu`, `memory`, `scc`, `via1`/`via2`, `rtc`, `scsi`, `floppy`, `sound`,
`storage`, `network.appletalk`, `input.mouse`, `debugger`, `mac`, plus
the root methods (`cp`, `peeler`, `rom_probe`, `rom_load`, `fd_insert`,
`hd_attach`, `run`, `checkpoint_*`, `register_machine`, `running`,
`hd_models`, `dump_tree`, …).

The browser frontend and the e2e helpers call into the tree via
`window.gsEval(path, args?)` (see `app/web/js/emulator.js`). Inside the
shell, the same tree is reachable via four surface forms (proposal §4.1):

  cpu.pc                # bare path → read & print
  cpu.d0 = 0x1234        # path = value → write
  cpu.step 1000          # path arg → method call (shell form)
  $(cpu.step(1000))      # path(args) → method call (call form, expressions)

The legacy `eval <path>` and `runCommand`/`runCommandJSON` JS helpers
remain only as the terminal-input bridge and the two pre-main-loop
boot calls (`main.js` / `checkpoint.js`); everything else goes through
`gsEval`.

A live, auto-generated reference is regenerated by:

  python3 scripts/dump_object_model.py

The output lands in `docs/object-model-reference.md` — re-run after
any change to a class table or a root method, and check the diff.

## When AGENTS.md Is Wrong

If instructions in AGENTS.md are incorrect, incomplete, or misleading (e.g., wrong paths, missing build steps, outdated commands), log it in `notes/feedback.md`:

```
- [YYYY-MM-DD] <What was wrong in AGENTS.md>
  - Problem: <Instruction that didn't work or was missing>
  - Fix: <What you had to do instead>
  - Update needed: <How to fix AGENTS.md>
```

This is ONLY for documentation gaps in AGENTS.md — not for general code changes or feature work.

---
End of onboarding guide.
