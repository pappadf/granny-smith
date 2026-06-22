# Copilot Agent Environment Setup Guide

This document provides detailed guidance for troubleshooting build environment issues when running outside a devcontainer (e.g., in GitHub Actions via copilot-setup-steps.yml).

## Critical Rules

### Rule 1: Verify the Build Environment FIRST

Before making ANY code changes, verify that you can successfully:

1. **Build the WASM target**: `source /opt/emsdk/emsdk_env.sh && make clean && make`
2. **Build the headless target**: `make -f Makefile.headless clean && make -f Makefile.headless`
3. **Run e2e tests** (if test data is available): `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts`

If any of these fail due to environment issues (missing tools, download failures, etc.), **STOP and report the issue**. Do NOT proceed with code changes if you cannot verify them.

### Rule 2: Never Commit Untested Code

- If you cannot run the test suite, you cannot verify your changes work
- If tests cannot be run, do NOT make code commits
- Making optimizations or fixes without testing them is worse than making no changes at all
- State clearly in your response if you were unable to test changes

### Rule 3: Known Environment Issues

The Emscripten SDK installation may fail with HTTP 403 errors when downloading from `storage.googleapis.com`. This is a known issue with GitHub Actions runners being rate-limited.

**If emsdk installation fails:**

1. Check the workflow output for "HTTP Error 403: Forbidden" messages
2. The `copilot-setup-steps.yml` workflow has retry logic and fallback to system Node.js
3. If setup still fails after retries, the environment is NOT ready for WASM builds
4. You can still work with the **headless** build target (`make -f Makefile.headless`) which doesn't require Emscripten
5. Report the environment issue clearly rather than working around it silently

**What NOT to do when emsdk fails:**
- Do NOT try to patch emsdk Python scripts
- Do NOT try to create fake directory structures
- Do NOT commit "optimizations" that you couldn't test
- Do NOT spend excessive time (>15 minutes) on workarounds

## Build Targets

| Target | Command | Requires Emscripten? | Purpose |
|--------|---------|---------------------|---------|
| WASM (release) | `make` | Yes | Browser emulator |
| WASM (debug) | `make debug` | Yes | Browser with debug symbols |
| Headless | `make -f Makefile.headless` | No | CLI testing, integration tests |
| Integration tests | `make integration-test` | No | Native headless tests |
| E2E tests | `npx --prefix tests/e2e playwright test ...` | Yes (needs WASM build) | Browser automation tests |

## When to Escalate

Report to the user and do NOT proceed if:
- Emscripten installation fails after all retry attempts
- Test data cannot be fetched (for tasks that require test data)
- Build failures that are not caused by your changes
- Network/infrastructure issues blocking downloads

Your role is to make **verified, tested improvements**. An unverified change is not an improvement.
