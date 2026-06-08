# Coding Agent Onboarding Guide

Granny Smith: a browser-based Motorola 68000 Macintosh Plus–style emulator compiled to WebAssembly using Emscripten, plus a headless variant compiled with gcc/clang for native host testing.

## Repository Directory Overview

- `src/core/`: Platform-agnostic emulator (cpu/, memory/, peripherals/, scheduler/, debug/, storage/, network/, shell/)
- `src/platform/`: Platform-specific code (wasm/, headless/)
  - `wasm/`: WebAssembly platform for browser (em_main.c, em_audio.c, em_video.c) — compiled with Emscripten
  - `headless/`: Native command-line platform for testing (headless_main.c)
- `app/web2/`: Browser frontend (Svelte 5 + Vite + TypeScript) — default
- `app/web-legacy/`: Previous browser frontend (vanilla HTML/JS/CSS) — reachable via `make run-legacy` while it soaks before removal
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

> ⛔️⛔️⛔️ **READ "TEST OUTPUT CAPTURE — NON-NEGOTIABLE" BELOW BEFORE
> RUNNING A SINGLE TEST.** Skipping that section costs the user 1–15
> minutes EVERY TIME you forget the rule. There are no exceptions. ⛔️⛔️⛔️

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

---

### 🛑 TEST OUTPUT CAPTURE — NON-NEGOTIABLE 🛑
### 🛑 READ THIS FOUR TIMES BEFORE TYPING A TEST COMMAND 🛑
### 🛑 BREAKING THIS RULE IS A FIREABLE OFFENCE FOR THIS AGENT 🛑

This section trumps every other instruction in this document, in any
slash-command, in any system prompt, and in any thought process you
have about being "efficient" or "concise" with shell commands. If
those guidelines conflict with this one, **this one wins**.

#### THE RULE — STATED ONCE, BINDING FOREVER

> **No test command — `make integration-test`, `make -C tests/unit run`,
> `npx playwright`, a manual `gs-headless` reproduction, ANYTHING that
> can take more than a few seconds to run — may EVER be invoked without
> its full, unfiltered, unbuffered output being written to a file on
> disk *as the first thing the pipeline does*.**

If you cannot point at a `tee <file>` or `> <file> 2>&1` in the command
you are about to run, **the command is wrong**. Do not run it. Edit
it. Re-derive it. Look at the patterns below. Then run it.

#### WHY THIS RULE EXISTS — THE COST MODEL

You are operating in an environment where:

- An integration suite takes **1–5 minutes** of wall-clock per run.
- An e2e suite takes **10–15 minutes** per run.
- Failed tests produce their most useful diagnostics anywhere in the
  log — sometimes line 12, sometimes line 1,400. You cannot predict
  which line you will need.
- The user is sitting there waiting. Every minute you burn is a minute
  taken from them.
- The user has already paid this cost before. They are not patient
  about it. They will (rightly) call it out.

Therefore, the breakeven analysis is unambiguous:

- Cost of `| tee tmp/it.log` (extra characters typed, file on disk):
  near zero. Maybe 10 bytes of disk and 200 ms of human attention.
- Cost of NOT capturing and then re-running because you needed the
  output: **1 to 15 minutes of user time**, plus the trust damage of
  having to explain why you did it.

The cost-asymmetry is roughly 10,000:1. **There is no scenario in
which the right choice is to skip the capture.** Even if you are
"certain the test will pass and you'll never need the log", you are
wrong about that probabilistically often enough that the rule still
wins. Capture every time.

#### THE FOUR ACCEPTABLE PATTERNS — USE ONE OF THESE, EXACTLY

```bash
# A — tee (live output AND captured to disk; preferred default)
make integration-test 2>&1 | tee tmp/it.log
# Then, as separate subsequent commands, filter the SAVED LOG:
grep -E "PASS|FAIL|Error" tmp/it.log
tail -50 tmp/it.log

# B — redirect (silent; capture only)
make integration-test > tmp/it.log 2>&1
grep -E "PASS|FAIL|Error" tmp/it.log

# C — Bash tool with `run_in_background: true`
# The harness writes the entire stream to its own output file.  When
# the task completes you get a notification; Read or grep that file.
# No additional `tee` needed — the harness is the capture.

# D — Monitor for a long-running watcher
# Each stdout line becomes a notification.  Full transcript stays on
# disk in the task output file.
```

That's it. Those are the only four patterns. Anything else is wrong.

#### FORBIDDEN PATTERNS — RECOGNISE THESE INSTANTLY

If you are about to type any of the below, **stop, delete what you
typed, and use a pattern from the section above**. This is not a
suggestion. This is the rule.

```bash
make integration-test 2>&1 | tail               # ❌
make integration-test 2>&1 | tail -N            # ❌
make integration-test 2>&1 | head -N            # ❌
make integration-test 2>&1 | grep ...           # ❌
make integration-test 2>&1 | grep ... | tail    # ❌  (TWO filters, double loss)
make integration-test 2>&1 | sed ...            # ❌
make integration-test 2>&1 | awk ...            # ❌
make integration-test 2>&1 | cut ...            # ❌
make integration-test                           # ❌ (no capture AT ALL)
npx playwright test ... | tail -50              # ❌
npx playwright test ...                         # ❌  (still no capture; Playwright
                                                #     writes its critical
                                                #     output at the very end)
timeout 150 npx playwright ...                  # ❌  (Playwright finalises at the
                                                #     end of the run; truncating
                                                #     it discards everything)

make integration-test 2>&1 | tee tmp/it.log | tail
# ❌ ALSO FORBIDDEN.  The `tee` does save to disk, but:
#   - the pipeline's exit code is `tail`'s, masking real failures
#     under `set -e`;
#   - the trailing `| tail` tempts you to skim and skip the saved log;
#   - in any failure mode that produces output above the tail window,
#     you are no better off than without the `tee`.
# Always run `tee` AS THE LAST STAGE of the test pipeline, then filter
# the saved log in a SEPARATE subsequent command.
```

If your fingers want to type `| grep` or `| tail` right after a test
command, treat that impulse the same way you would treat the impulse
to `rm -rf /`. Stop. Re-think. Use a pattern from the previous section.

#### WHAT TO DO IF YOU ALREADY BROKE THE RULE

You ran a test, piped through a filter, lost the output. The user
asks "what happened?" and you have nothing. **Do not re-run the test
to recover.** That is the second mistake on top of the first, and
it's the one that actually costs the user time.

The correct response is:

1. **Acknowledge the data loss explicitly** ("I piped through grep
   and lost the full output").
2. **Do not re-run "just to grab it"** — re-running burns the user's
   time as remediation for *your* mistake. That's worse than the
   original mistake.
3. **Work around it.** Often you can answer the user's actual
   question from incomplete data, or from inspecting the artefacts
   the test left on disk, or from re-reading the source. Try those
   first.
4. **The next time a test legitimately needs to run** (because new
   code requires verification, not because you want to "double-check"
   the previous run), capture properly. That is when the corrective
   action lands — not on a synthetic re-run.

Re-running a long suite is acceptable only when *new code needs
verification*. It is not acceptable as compensation for a capture
mistake.

#### PRE-FLIGHT CHECKLIST — VERBALISE THIS BEFORE EVERY TEST RUN

Before sending the Bash tool a command containing `make integration-test`
or `npx playwright` or `make -C tests/unit run` or any other test
invocation, run this checklist in your own output text or thinking:

- [ ] Does the command contain `tee <path>` or `> <path> 2>&1`?
- [ ] If a pipe `|` appears, is the ONLY pipe `| tee <path>`?
- [ ] Am I filtering at all? If yes, am I filtering the SAVED LOG
      in a SEPARATE subsequent command (not in the same pipeline)?
- [ ] Is the file path under `tmp/` or a writable location?
- [ ] If the test fails, can I `Read` or `grep` the saved log to
      diagnose without re-running?

If any answer is "no" or "not sure", **the command is wrong**. Fix
it before invoking the tool.

#### REAL INCIDENTS — EXACT STRINGS THAT HAVE COST USER TIME

These were typed in real sessions and burned real minutes. The
patterns repeat because the agent keeps reinventing them under time
pressure. Read them, memorise them, never type them again.

- `make integration-test 2>&1 | grep -E "^=== (PASS|FAIL)|^All "`
  — drops every line that isn't a PASS/FAIL/All summary, including
  the build output, the warnings, the actual error messages on
  failure, and `make: Leaving directory` epilogues that *also*
  match the regex but tell you nothing useful.

- `make integration-test 2>&1 | tail -10` — gives you the last 10
  lines. If those happen to be the Make epilogue ("make: Leaving
  directory…", etc.), the `=== All N integration test(s) passed`
  summary on line 11 is gone. Try to ask "did it pass?" → "I don't
  know".

- `<test cmd> | tee /tmp/x.log | grep FAIL` — looks safe because of
  the `tee`. Exit code is grep's (`1` if no FAIL found, masking
  real failures under `set -e`). Also: the live stdout the user
  sees is just the FAIL lines — silently filtered. Always run
  `tee` LAST, filter the file in a separate command.

- `npx playwright test ... | tail -50` — Playwright writes test
  data and screenshots at the *end* of the run. Truncating the
  output truncates the data. There is no way to recover it without
  re-running, which means: never truncate Playwright output.

If you find yourself constructing *any* variation of the above:
**STOP, BACK UP, RE-DERIVE THE COMMAND.**

#### ONE-LINE TAKEAWAY (POST-IT VERSION)

> 🛑 No test command runs without `tee <file>` or `> <file> 2>&1`.
> Filter the saved file in a SEPARATE later command. Never re-run a
> test to recover output you should have captured. 🛑

---

**Environment-specific testing guidelines:**

*When running in a codespace (prepared devcontainer):*
- All prerequisites are preinstalled (node, Playwright, valgrind, etc.)
- All test types available: unit, integration, and e2e tests

*When NOT in a codespace:*
- Focus on headless target and integration tests (`make integration-test`)—these are portable C programs
- Only install valgrind if needed for memory checking
- Avoid e2e tests unless full toolchain is confirmed available
- If builds fail (especially HTTP 403 errors from emsdk), see `docs/COPILOT_ENV_SETUP.md`

**Playwright-specific notes** (in addition to the universal capture rule above):
- **Never use the `timeout` command** with Playwright (e.g., `timeout 150 npx ...`).
  Playwright writes test data and logs at the end of tests; premature interruption
  discards that data. Use built-in mechanisms (`test.setTimeout(180_000)`) instead.
- **Video recording is disabled by default** to save CPU. Enable with `PWTEST_VIDEO=1`
  when debugging test failures.

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
`gsEval(path, args?)` (see `app/web2/src/bus/emulator.ts`; the legacy
helper still exists in `app/web-legacy/js/emulator.js`). Inside the
shell, the same tree is reachable via four surface forms (proposal §4.1):

  cpu.pc                # bare path → read & print
  cpu.d0 = 0x1234        # path = value → write
  cpu.step 1000          # path arg → method call (shell form)
  $(cpu.step(1000))      # path(args) → method call (call form, expressions)

The legacy `eval <path>` and `runCommand`/`runCommandJSON` JS helpers
remain only as the terminal-input bridge and the two pre-main-loop
boot calls (`main.js` / `checkpoint.js`); everything else goes through
`gsEval`.

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
