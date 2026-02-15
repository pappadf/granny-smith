# GS_ASSERT Integration with Playwright E2E Tests

## Overview

The emulator now includes comprehensive GS_ASSERT failure detection for Playwright e2e tests. When a `GS_ASSERT()` fails in the C code, the test will:

1. **Immediately fail** with detailed diagnostic information
2. **Log extensively** to console with assertion details
3. **Capture assertion data** as test artifacts
4. **Provide terminal snapshots** showing the full context

## How It Works

### C Side (WASM)
When `GS_ASSERT(condition)` fails in any C source file:
1. The assertion prints diagnostic info to stdout (terminal)
2. Calls `js_notify_assertion_failure()` via Emscripten EM_JS
3. This triggers a JavaScript callback in the browser

### JavaScript Side (Browser)
The web UI (`web/index.html`) installs a global `__gsAssertionHandler`:
1. Logs assertion details to console with prominent formatting
2. Stores failure info in `window.__gsAssertionFailures[]`
3. Dispatches `gs-assertion-failure` custom event
4. In test mode, throws a `GSAssertionError` exception

### Test Side (Playwright)
The test fixtures (`tests/e2e/fixtures.ts`) automatically:
1. Listen to console.error for assertion messages
2. Listen to page errors for thrown exceptions
3. Check `__gsAssertionFailures` in afterEach hook
4. Fail the test with comprehensive error details

## Automatic Failure Detection

**All tests automatically detect assertion failures** via the `afterEach` hook in `fixtures.ts`. No additional code is required in individual tests.

When an assertion fails during a test:
- The test immediately fails
- Console shows highlighted assertion details
- Test artifacts include JSON with full assertion info
- Terminal snapshot is captured with all output

## Manual Assertion Checking

For tests that need explicit control, use these helper functions from `tests/e2e/utils/terminal.ts`:

### Check for Any Failures
```typescript
import { checkForAssertionFailures } from '../utils/terminal';

test('my test', async ({ page }) => {
  // ... test actions ...
  
  // Explicitly verify no assertions have fired
  await checkForAssertionFailures(page);
});
```

### Wait for Expected Failure (Negative Tests)
```typescript
import { waitForAssertionFailure } from '../utils/terminal';

test('should trigger assertion on invalid state', async ({ page }) => {
  // ... set up invalid condition ...
  
  // Wait for the expected assertion to fire
  const failure = await waitForAssertionFailure(page, 5000);
  
  expect(failure.expr).toContain('expected_condition');
  expect(failure.file).toContain('via.c');
});
```

## Console Output Format

When an assertion fails, you'll see:

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

And in the test output:
```
======================================================================
ASSERTION FAILURE DETECTED IN TEST:
======================================================================
GS_ASSERT failed during test: port < 2
  at src/via.c:514 in via_input
  timestamp: 2025-12-25T10:30:45.123Z
======================================================================
```

## Assertion Failure Object

The failure info stored in `__gsAssertionFailures` contains:
```typescript
{
  expr: string;      // The assertion expression that failed
  file: string;      // Source file (e.g., "src/via.c")
  line: number;      // Line number
  func: string;      // Function name (e.g., "via_input")
  timestamp: number; // Unix timestamp in milliseconds
}
```

## Example Test Pattern

```typescript
import { test, expect } from '../fixtures';
import { bootWithMedia } from '../utils/boot';

test('my emulator test', async ({ page, log }) => {
  // Boot the emulator
  await bootWithMedia(page, 'roms/Plus_v3.rom');
  
  // Perform test actions
  await page.evaluate(() => window.queueCommand('some-command'));
  
  // The test will automatically fail if any GS_ASSERT fires
  // No additional code needed!
  
  // But you can also check explicitly if desired:
  // await checkForAssertionFailures(page);
});
```

## Debugging Assertion Failures

When a test fails due to assertion:

1. **Check console output** - Look for the boxed assertion message
2. **Review terminal snapshot** - File saved to `test-results/<test-name>/xterm.txt`
3. **Check assertion artifact** - JSON file attached to test results
4. **Review video** (if enabled) - Visual recording of the failure
5. **Examine C source** - Go to the file:line shown in the error

## CI/CD Integration

Assertion failures are treated as critical test failures:
- Exit with non-zero status
- Marked as failed in test reports
- Artifacts automatically uploaded
- Clear visibility in CI logs

## Best Practices

1. **Use GS_ASSERT liberally** - All assertions are now test-visible
2. **Don't catch exceptions** - Let assertion errors propagate
3. **Review terminal output** - Contains full diagnostic info from C
4. **Keep assertions meaningful** - Clear expressions help debugging
5. **Test assertions work** - Write negative tests when appropriate

## Technical Details

### Files Modified
- `src/debug.c` - Added EM_JS callback for assertion notification
- `web/index.html` - Installed global assertion handler
- `tests/e2e/fixtures.ts` - Added automatic assertion checking
- `tests/e2e/utils/terminal.ts` - Added shim and helper functions

### No Performance Impact
The assertion checking has negligible performance impact:
- EM_JS calls are only made when assertions fail
- Event listeners are lightweight
- No polling or active checking during normal execution
