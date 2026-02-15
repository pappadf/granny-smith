# GS_ASSERT E2E Test Integration - Implementation Summary

## What Was Implemented

A comprehensive system that ensures **any GS_ASSERT failure in the emulator automatically fails Playwright e2e tests** with extensive diagnostic information.

## Changes Made

### 1. C/WASM Layer (`src/debug.c`)
- Added Emscripten EM_JS callback `js_notify_assertion_failure()`
- Modified `gs_assert_fail()` to call JavaScript notification after printing diagnostics
- Passes assertion details (expression, file, line, function) to JavaScript

### 2. Web UI (`web/index.html`)
- Installed global `window.__gsAssertionHandler` callback
- Handler logs failures with prominent console formatting
- Stores failures in `window.__gsAssertionFailures[]` array
- Dispatches `gs-assertion-failure` custom events
- In test mode, throws `GSAssertionError` exceptions

### 3. Test Infrastructure (`tests/e2e/`)

#### `tests/e2e/fixtures.ts`
- Modified `page` fixture to listen for console errors and page errors
- Added `afterEach` hook that checks for assertion failures
- Automatically fails tests with detailed error messages
- Attaches assertion details as JSON artifacts

#### `tests/e2e/utils/terminal.ts`
- Enhanced `installTestShim()` to initialize assertion tracking
- Created promise-based mechanism for waiting on assertions
- Added `checkForAssertionFailures()` helper function
- Added `waitForAssertionFailure()` for negative tests

#### Documentation
- Created `tests/e2e/README-assertions.md` - comprehensive guide
- Updated `tests/e2e/README.md` with references

## How It Works - Complete Flow

### When GS_ASSERT Fails:

1. **C Code** (`src/debug.c`)
   ```c
   GS_ASSERT(port < 2);  // This fails
   ```

2. **Diagnostic Output** (to terminal)
   - Prints "==================== ASSERT ===================="
   - Shows expression, file, line, function
   - Calls diagnostic functions (backtraces, process info)
   - Pauses scheduler

3. **JavaScript Notification** (via EM_JS)
   ```javascript
   window.__gsAssertionHandler({
     expr: "port < 2",
     file: "src/via.c",
     line: 514,
     func: "via_input",
     timestamp: 1766704970600
   });
   ```

4. **Browser Actions**
   - Logs to console with box formatting
   - Stores in `__gsAssertionFailures[]`
   - Dispatches event
   - Throws exception (in test mode)

5. **Test Detection** (multiple layers)
   - Console listener detects error log
   - Page error listener catches exception
   - `afterEach` hook checks `__gsAssertionFailures`
   - **Test fails immediately with full details**

## Benefits

### Immediate Feedback
- Tests fail as soon as assertion fires
- No need to wait for test completion
- Clear indication of root cause

### Rich Diagnostics
- Full C stack trace and backtraces
- Terminal output captured
- JSON artifact with details
- Console logs with formatting
- Video recording (if enabled)

### Zero Test Overhead
- No manual checking required
- Works for all tests automatically
- No performance impact during normal execution
- Only activates when assertions fail

### Developer-Friendly
- Clear error messages
- File and line numbers
- Function context
- Timestamp for correlation

## Example Output

### Console (during test):
```
╔═══════════════════════════════════════════════════════════════╗
║ GS_ASSERT FAILURE DETECTED                                     ║
╠═══════════════════════════════════════════════════════════════╣
║ Expression: port < 2
║ Location: src/via.c:514
║ Function: via_input
║ Timestamp: 2025-12-25T10:30:45.123Z
╚═══════════════════════════════════════════════════════════════╝
```

### Playwright Test Output:
```
======================================================================
ASSERTION FAILURE DETECTED IN TEST:
======================================================================
GS_ASSERT failed during test: port < 2
  at src/via.c:514 in via_input
  timestamp: 2025-12-25T10:30:45.123Z
======================================================================

  1) [chromium] › my-test.spec.ts:10:5 › Test Suite › failing test

    Error: GS_ASSERT failed during test: port < 2
      at src/via.c:514 in via_input
      timestamp: 2025-12-25T10:30:45.123Z
```

## Advanced Usage

### Explicit Checking
```typescript
import { checkForAssertionFailures } from '../utils/terminal';

await checkForAssertionFailures(page);
```

### Negative Tests (Expecting Failure)
```typescript
import { waitForAssertionFailure } from '../utils/terminal';

const failure = await waitForAssertionFailure(page, 5000);
expect(failure.expr).toContain('expected_condition');
```

### Accessing Failure Data
```typescript
const failures = await page.evaluate(() => window.__gsAssertionFailures);
console.log(failures); // Array of all assertions that fired
```

## Testing the Implementation

Build and test:
```bash
cd /workspaces/granny-smith
make clean && make
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts
```

The build was successful and tests pass with the new infrastructure in place.

## Future Enhancements (Optional)

1. **Severity Levels**: Add WARNING vs FATAL distinction
2. **Assertion Categories**: Group by subsystem (VIA, CPU, SCSI, etc.)
3. **Statistics**: Count assertion types across test runs
4. **Recovery**: Optional continue-on-assert mode for certain tests
5. **Visual Indicators**: Red overlay on canvas when assertion fires

## Compatibility

- Works with all existing tests automatically
- No changes needed to existing test code
- Backward compatible (tests without assertions unaffected)
- CI/CD compatible (proper exit codes and artifacts)

## Performance

- Zero overhead during normal execution
- EM_JS only called when assertion fails
- Lightweight event listeners
- No polling or active monitoring
- Minimal memory footprint

---

**Status**: ✅ Implementation complete and tested
**Documentation**: ✅ README-assertions.md created
**Build**: ✅ Successful compilation with Emscripten
**Tests**: ✅ Existing tests pass with new infrastructure
